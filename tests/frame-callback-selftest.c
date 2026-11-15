/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "modbus-private.h"
#include "modbus.h"

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "Line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
            exit(EXIT_FAILURE);                                                       \
        }                                                                             \
    } while (0)

enum {
    RTU,
    TCP,
    TCP_PI
};

typedef struct {
    modbus_t *ctx;
    int peer;
} channel_t;

typedef struct {
    modbus_t *ctx;
    modbus_frame_direction_t direction;
    int length;
    int error;
    uint8_t data[MODBUS_MAX_ADU_LENGTH];
} frame_t;

typedef struct {
    int count;
    frame_t frames[8];
} frame_log_t;

static const uint8_t raw_request[] = {1, 3, 0, 0, 0, 2};
static const uint8_t rtu_request[] = {1, 3, 0, 0, 0, 2, 0xC4, 0x0B};
static const uint8_t rtu_response[] = {1, 3, 4, 0, 100, 0, 200, 0xBA, 0x7A};
static const uint8_t tcp_request[] = {0, 1, 0, 0, 0, 6, 1, 3, 0, 0, 0, 2};
static const uint8_t tcp_response[] = {0, 1, 0, 0, 0, 7, 1, 3, 4, 0, 100, 0, 200};

static channel_t open_channel(int backend)
{
    channel_t channel;
    int sockets[2];

    if (backend == RTU)
        channel.ctx = modbus_new_rtu("/unused", 9600, 'N', 8, 1);
    else if (backend == TCP)
        channel.ctx = modbus_new_tcp("127.0.0.1", 1502);
    else
        channel.ctx = modbus_new_tcp_pi("localhost", "1502");
    CHECK(channel.ctx != NULL);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(modbus_set_socket(channel.ctx, sockets[0]) == 0);
    channel.peer = sockets[1];
    CHECK(modbus_set_slave(channel.ctx, 1) == 0);
    CHECK(modbus_set_response_timeout(channel.ctx, 0, 100000) == 0);
    CHECK(modbus_set_byte_timeout(channel.ctx, 0, 100000) == 0);
    return channel;
}

static void close_channel(channel_t *channel)
{
    /* No serial port was configured, so only close the test descriptors. */
    CHECK(close(modbus_get_socket(channel->ctx)) == 0);
    CHECK(close(channel->peer) == 0);
    modbus_free(channel->ctx);
}

static void record_frame(modbus_t *ctx,
                         modbus_frame_direction_t direction,
                         const uint8_t *data,
                         int length,
                         void *user_data)
{
    frame_log_t *log = user_data;
    frame_t *frame;

    CHECK(log != NULL && log->count < 8);
    CHECK(data != NULL && length > 0 && length <= MODBUS_MAX_ADU_LENGTH);
    frame = &log->frames[log->count++];
    frame->ctx = ctx;
    frame->direction = direction;
    frame->length = length;
    frame->error = errno;
    memcpy(frame->data, data, length);
    /* A logging callback must not change the caller's errno. */
    errno = ERANGE;
}

static void check_frame(const frame_log_t *log,
                        int index,
                        modbus_t *ctx,
                        modbus_frame_direction_t direction,
                        const uint8_t *data,
                        int length)
{
    CHECK(log->count > index);
    CHECK(log->frames[index].ctx == ctx);
    CHECK(log->frames[index].direction == direction);
    CHECK(log->frames[index].length == length);
    CHECK(memcmp(log->frames[index].data, data, length) == 0);
}

static void write_frame(int fd, const uint8_t *data, int length)
{
    CHECK(write(fd, data, length) == length);
}

static void check_wire(int fd, const uint8_t *data, int length)
{
    uint8_t received[MODBUS_MAX_ADU_LENGTH];
    int offset = 0;

    while (offset < length) {
        int rc = read(fd, received + offset, length - offset);
        CHECK(rc > 0);
        offset += rc;
    }
    CHECK(memcmp(received, data, length) == 0);
}

static void test_registration_and_instances(void)
{
    channel_t channels[4];
    frame_log_t logs[4] = {0};
    frame_log_t replacement = {0};
    int i;

    errno = 0;
    CHECK(modbus_set_frame_callback(NULL, record_frame, NULL) == -1);
    CHECK(errno == EINVAL);
    CHECK(modbus_set_frame_callback(NULL, NULL, NULL) == -1);
    CHECK(errno == EINVAL);

    /* Two RTU instances, TCP and TCP PI all start with callbacks disabled. */
    for (i = 0; i < 4; i++) {
        const uint8_t *expected = i < 2 ? rtu_request : tcp_request;
        int length = i < 2 ? sizeof(rtu_request) : sizeof(tcp_request);
        channels[i] = open_channel(i < 2 ? RTU : (i == 2 ? TCP : TCP_PI));
        CHECK(modbus_send_raw_request_tid(channels[i].ctx, raw_request, 6, 1) == length);
        check_wire(channels[i].peer, expected, length);
        CHECK(modbus_set_frame_callback(channels[i].ctx, record_frame, &logs[i]) == 0);
    }

    for (i = 3; i >= 0; i--) {
        const uint8_t *expected = i < 2 ? rtu_request : tcp_request;
        int length = i < 2 ? sizeof(rtu_request) : sizeof(tcp_request);
        errno = EDOM;
        CHECK(modbus_send_raw_request_tid(channels[i].ctx, raw_request, 6, 1) == length);
        CHECK(errno == EDOM);
        CHECK(logs[i].count == 1);
        check_frame(&logs[i], 0, channels[i].ctx, MODBUS_FRAME_TX, expected, length);
        check_wire(channels[i].peer, expected, length);
    }

    CHECK(modbus_set_frame_callback(channels[0].ctx, NULL, &logs[0]) == 0);
    CHECK(modbus_send_raw_request(channels[0].ctx, raw_request, 6) == 8);
    CHECK(logs[0].count == 1);
    check_wire(channels[0].peer, rtu_request, 8);
    CHECK(modbus_set_frame_callback(channels[1].ctx, record_frame, &replacement) == 0);
    CHECK(modbus_set_debug(channels[1].ctx, TRUE) == 0);
    CHECK(modbus_send_raw_request(channels[1].ctx, raw_request, 6) == 8);
    CHECK(modbus_set_debug(channels[1].ctx, FALSE) == 0);
    CHECK(logs[1].count == 1 && replacement.count == 1);
    check_frame(&replacement, 0, channels[1].ctx, MODBUS_FRAME_TX, rtu_request, 8);
    check_wire(channels[1].peer, rtu_request, 8);
    for (i = 0; i < 4; i++)
        close_channel(&channels[i]);
}

/* 开关只控制通知，关闭时仍须发送和接收完整、正确的线缆数据。 */
static void
check_switched_io(channel_t *channel, int backend, frame_log_t *log, int expected_count)
{
    const uint8_t *query = backend == RTU ? rtu_request : tcp_request;
    const uint8_t *response = backend == RTU ? rtu_response : tcp_response;
    int query_length = backend == RTU ? sizeof(rtu_request) : sizeof(tcp_request);
    int response_length = backend == RTU ? sizeof(rtu_response) : sizeof(tcp_response);
    uint8_t received[MODBUS_MAX_ADU_LENGTH];
    int previous_count = log->count;

    errno = EDOM;
    CHECK(modbus_send_raw_request_tid(channel->ctx, raw_request, 6, 1) == query_length);
    CHECK(errno == EDOM);
    check_wire(channel->peer, query, query_length);
    write_frame(channel->peer, response, response_length);
    errno = EDOM;
    CHECK(modbus_receive_confirmation(channel->ctx, received) == response_length);
    CHECK(errno == EDOM);
    CHECK(memcmp(received, response, response_length) == 0);
    CHECK(log->count == expected_count);
    if (expected_count > previous_count) {
        CHECK(expected_count == previous_count + 2);
        check_frame(
            log, previous_count, channel->ctx, MODBUS_FRAME_TX, query, query_length);
        check_frame(log,
                    previous_count + 1,
                    channel->ctx,
                    MODBUS_FRAME_RX,
                    response,
                    response_length);
    }
}

static void test_callback_switch(int backend)
{
    channel_t channel = open_channel(backend);
    channel_t other = open_channel(backend);
    frame_log_t log = {0};
    frame_log_t other_log = {0};
    frame_log_t replacement = {0};

    CHECK(modbus_set_frame_callback_enabled(NULL, FALSE) == -1);
    CHECK(errno == EINVAL);
    CHECK(modbus_set_frame_callback_enabled(NULL, TRUE) == -1);
    CHECK(errno == EINVAL);

    /* 注册前设置的暂停状态仍应生效；另一实例继续正常通知。 */
    errno = EDOM;
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, FALSE) == 0);
    CHECK(errno == EDOM);
    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    CHECK(modbus_set_frame_callback(other.ctx, record_frame, &other_log) == 0);
    check_switched_io(&channel, backend, &log, 0);
    check_switched_io(&other, backend, &other_log, 2);

    /* 无需重新注册即可恢复，任意非零值均表示开启。 */
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, 7) == 0);
    check_switched_io(&channel, backend, &log, 2);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, FALSE) == 0);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, FALSE) == 0);
    CHECK(modbus_set_debug(channel.ctx, TRUE) == 0);
    check_switched_io(&channel, backend, &log, 2);
    CHECK(channel.ctx->debug == TRUE);
    CHECK(modbus_set_debug(channel.ctx, FALSE) == 0);

    /* 暂停期间替换回调不会自动开启；恢复后应使用新的用户数据。 */
    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &replacement) == 0);
    check_switched_io(&channel, backend, &replacement, 0);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, -1) == 0);
    check_switched_io(&channel, backend, &replacement, 2);
    CHECK(log.count == 2 && other_log.count == 2);

    /* 注销不会留下旧回调；开启没有注册回调的实例也应安全。 */
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, FALSE) == 0);
    CHECK(modbus_set_frame_callback(channel.ctx, NULL, NULL) == 0);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, TRUE) == 0);
    check_switched_io(&channel, backend, &replacement, 2);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, FALSE) == 0);
    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    check_switched_io(&channel, backend, &log, 2);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, TRUE) == 0);
    CHECK(modbus_set_frame_callback_enabled(channel.ctx, TRUE) == 0);
    check_switched_io(&channel, backend, &log, 4);

    if (backend == RTU) {
        const uint8_t bad_crc[] = {1, 3, 4, 0, 100, 0, 200, 0xBA, 0x7B};
        uint8_t received[MODBUS_MAX_ADU_LENGTH];
        CHECK(modbus_set_frame_callback_enabled(channel.ctx, FALSE) == 0);
        write_frame(channel.peer, bad_crc, sizeof(bad_crc));
        CHECK(modbus_receive_confirmation(channel.ctx, received) == -1);
        CHECK(errno == EMBBADCRC && log.count == 4);
    }
    close_channel(&channel);
    close_channel(&other);
}

static void test_rtu_integrity(void)
{
    channel_t channel = open_channel(RTU);
    frame_log_t log = {0};
    uint8_t received[MODBUS_MAX_ADU_LENGTH];
    uint8_t bad_crc[] = {1, 3, 4, 0, 100, 0, 200, 0xBA, 0x7B};
    const uint8_t other_slave[] = {2, 3, 4, 0, 100, 0, 200, 0x89, 0x7A};

    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    write_frame(channel.peer, rtu_response, sizeof(rtu_response));
    errno = EDOM;
    CHECK(modbus_receive_confirmation(channel.ctx, received) == 9);
    CHECK(errno == EDOM && log.count == 1);
    check_frame(&log, 0, channel.ctx, MODBUS_FRAME_RX, rtu_response, 9);

    CHECK(modbus_set_error_recovery(channel.ctx, MODBUS_ERROR_RECOVERY_PROTOCOL) == 0);
    write_frame(channel.peer, bad_crc, sizeof(bad_crc));
    CHECK(modbus_receive_confirmation(channel.ctx, received) == -1);
    CHECK(errno == EMBBADCRC && log.count == 2);
    CHECK(log.frames[1].error == EMBBADCRC);
    check_frame(&log, 1, channel.ctx, MODBUS_FRAME_RX, bad_crc, 9);

    write_frame(channel.peer, other_slave, sizeof(other_slave));
    CHECK(modbus_receive_confirmation(channel.ctx, received) == 0);
    CHECK(log.count == 3);
    check_frame(&log, 2, channel.ctx, MODBUS_FRAME_RX, other_slave, 9);
    CHECK(modbus_set_frame_callback(channel.ctx, NULL, NULL) == 0);
    write_frame(channel.peer, rtu_response, sizeof(rtu_response));
    CHECK(modbus_receive_confirmation(channel.ctx, received) == 9);
    CHECK(log.count == 3);
    close_channel(&channel);
}

static void test_tcp_transactions(void)
{
    channel_t channel = open_channel(TCP);
    frame_log_t log = {0};
    uint16_t values[2];
    const uint8_t wrong_tid[] = {0, 99, 0, 0, 0, 7, 1, 3, 4, 0, 100, 0, 200};
    const uint8_t exception[] = {0, 3, 0, 0, 0, 3, 1, 0x83, 2};
    uint8_t expected_request[sizeof(tcp_request)];

    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    write_frame(channel.peer, tcp_response, sizeof(tcp_response));
    CHECK(modbus_read_registers(channel.ctx, 0, 2, values) == 2);
    CHECK(values[0] == 100 && values[1] == 200 && log.count == 2);
    check_frame(&log, 0, channel.ctx, MODBUS_FRAME_TX, tcp_request, 12);
    check_frame(&log, 1, channel.ctx, MODBUS_FRAME_RX, tcp_response, 13);
    check_wire(channel.peer, tcp_request, 12);

    memcpy(expected_request, tcp_request, sizeof(tcp_request));
    expected_request[1] = 2;
    write_frame(channel.peer, wrong_tid, sizeof(wrong_tid));
    CHECK(modbus_read_registers(channel.ctx, 0, 2, values) == -1);
    CHECK(errno == EMBBADDATA && log.count == 4);
    check_frame(&log, 3, channel.ctx, MODBUS_FRAME_RX, wrong_tid, 13);
    check_wire(channel.peer, expected_request, 12);

    expected_request[1] = 3;
    write_frame(channel.peer, exception, sizeof(exception));
    CHECK(modbus_read_registers(channel.ctx, 0, 2, values) == -1);
    CHECK(errno == EMBXILADD && log.count == 6);
    check_frame(&log, 5, channel.ctx, MODBUS_FRAME_RX, exception, 9);
    check_wire(channel.peer, expected_request, 12);
    close_channel(&channel);
}

static void test_server_reply(int backend)
{
    channel_t channel = open_channel(backend);
    frame_log_t log = {0};
    modbus_mapping_t *mapping = modbus_mapping_new(0, 0, 2, 0);
    uint8_t request[MODBUS_MAX_ADU_LENGTH];
    const uint8_t *query = backend == RTU ? rtu_request : tcp_request;
    const uint8_t *response = backend == RTU ? rtu_response : tcp_response;
    int query_length = backend == RTU ? sizeof(rtu_request) : sizeof(tcp_request);
    int response_length = backend == RTU ? sizeof(rtu_response) : sizeof(tcp_response);

    CHECK(mapping != NULL);
    mapping->tab_registers[0] = 100;
    mapping->tab_registers[1] = 200;
    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    write_frame(channel.peer, query, query_length);
    CHECK(modbus_receive(channel.ctx, request) == query_length);
    CHECK(modbus_reply(channel.ctx, request, query_length, mapping) == response_length);
    CHECK(log.count == 2);
    check_frame(&log, 0, channel.ctx, MODBUS_FRAME_RX, query, query_length);
    check_frame(&log, 1, channel.ctx, MODBUS_FRAME_TX, response, response_length);
    check_wire(channel.peer, response, response_length);
    modbus_mapping_free(mapping);
    close_channel(&channel);
}

static void test_receive_boundaries(int backend)
{
    channel_t channel = open_channel(backend);
    frame_log_t log = {0};
    uint8_t received[MODBUS_MAX_ADU_LENGTH];
    const uint8_t *response = backend == RTU ? rtu_response : tcp_response;
    int length = backend == RTU ? sizeof(rtu_response) : sizeof(tcp_response);
    pid_t child;
    int status;

    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    child = fork();
    CHECK(child != -1);
    if (child == 0) {
        int i;
        for (i = 0; i < length; i++) {
            write_frame(channel.peer, response + i, 1);
            usleep(2000);
        }
        _exit(EXIT_SUCCESS);
    }
    CHECK(modbus_receive_confirmation(channel.ctx, received) == length);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    CHECK(log.count == 1);
    check_frame(&log, 0, channel.ctx, MODBUS_FRAME_RX, response, length);

    CHECK(modbus_set_response_timeout(channel.ctx, 0, 20000) == 0);
    CHECK(modbus_set_byte_timeout(channel.ctx, 0, 20000) == 0);
    CHECK(modbus_receive_confirmation(channel.ctx, received) == -1);
    CHECK(errno == ETIMEDOUT && log.count == 1);
    write_frame(channel.peer, response, 3);
    CHECK(modbus_receive_confirmation(channel.ctx, received) == -1);
    CHECK(errno == ETIMEDOUT && log.count == 1);
    /* A completed TX is still observable when the response times out. */
    {
        uint16_t values[2];
        CHECK(modbus_read_registers(channel.ctx, 0, 2, values) == -1);
        CHECK(errno == ETIMEDOUT && log.count == 2);
        check_frame(&log,
                    1,
                    channel.ctx,
                    MODBUS_FRAME_TX,
                    backend == RTU ? rtu_request : tcp_request,
                    backend == RTU ? sizeof(rtu_request) : sizeof(tcp_request));
        check_wire(channel.peer,
                   backend == RTU ? rtu_request : tcp_request,
                   backend == RTU ? sizeof(rtu_request) : sizeof(tcp_request));
    }
    CHECK(shutdown(channel.peer, SHUT_WR) == 0);
    CHECK(modbus_receive_confirmation(channel.ctx, received) == -1);
    CHECK(errno == ECONNRESET && log.count == 2);
    close_channel(&channel);
}

/* Only send outcomes that are hard to force with small ADUs need a test double.
   All framing, receive, integrity and successful send paths use real backends. */
static int send_attempts;
static int send_mode;
static ssize_t (*real_send)(modbus_t *, const uint8_t *, int);

static ssize_t fault_send(modbus_t *ctx, const uint8_t *data, int length)
{
    send_attempts++;
    if (send_mode == 0)
        return length - 1;
    if (send_mode == 1 || send_attempts == 1) {
        errno = EIO;
        return -1;
    }
    return real_send(ctx, data, length);
}

static void test_send_failures(void)
{
    channel_t channel = open_channel(TCP);
    frame_log_t log = {0};
    const modbus_backend_t *original = channel.ctx->backend;
    modbus_backend_t backend = *original;

    real_send = original->send;
    backend.send = fault_send;
    channel.ctx->backend = &backend;
    CHECK(modbus_set_frame_callback(channel.ctx, record_frame, &log) == 0);
    send_attempts = 0;
    send_mode = 0;
    CHECK(modbus_send_raw_request(channel.ctx, raw_request, 6) == -1);
    CHECK(errno == EMBBADDATA && log.count == 0 && send_attempts == 1);
    send_mode = 1;
    CHECK(modbus_send_raw_request(channel.ctx, raw_request, 6) == -1);
    CHECK(errno == EIO && log.count == 0);

    send_mode = 2;
    send_attempts = 0;
    CHECK(modbus_set_response_timeout(channel.ctx, 0, 1000) == 0);
    CHECK(modbus_set_error_recovery(channel.ctx, MODBUS_ERROR_RECOVERY_LINK) == 0);
    CHECK(modbus_send_raw_request_tid(channel.ctx, raw_request, 6, 1) == 12);
    CHECK(errno == EIO && log.count == 1 && send_attempts == 2);
    check_frame(&log, 0, channel.ctx, MODBUS_FRAME_TX, tcp_request, 12);
    check_wire(channel.peer, tcp_request, 12);
    channel.ctx->backend = original;
    close_channel(&channel);
}

int main(void)
{
    test_registration_and_instances();
    test_callback_switch(RTU);
    test_callback_switch(TCP);
    test_callback_switch(TCP_PI);
    test_rtu_integrity();
    test_tcp_transactions();
    test_server_reply(RTU);
    test_server_reply(TCP);
    test_receive_boundaries(RTU);
    test_receive_boundaries(TCP);
    test_send_failures();
    puts("Frame callback tests passed");
    return EXIT_SUCCESS;
}
