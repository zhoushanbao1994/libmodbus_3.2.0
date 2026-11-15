/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * 帧回调设备测试工具：使用公开 libmodbus API 连接真实 TCP 或 RTU 从站。
 * 原来的无硬件自动回归保留在 frame-callback-selftest.c。
 *
 * 数据模型遵循 Modbus 四类点位：线圈/保持寄存器可读写，离散输入/输入
 * 寄存器只读。所有地址使用从 0 开始的协议地址，不自动换算 40001 等编号。
 * 写操作必须显式给出地址和值，每轮写入后立即回读同一范围并逐点校验。
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "modbus.h"

/* 限制配置条目总数，避免错误命令导致无界内存分配；每条仍按协议限制数量。 */
#define MAX_POINTS 64

typedef enum {
    READ_COILS,
    READ_DISCRETE,
    READ_HOLDING,
    READ_INPUT,
    WRITE_COILS,
    WRITE_REGISTERS
} point_type_t;

typedef struct {
    point_type_t type;
    int address;
    int count;
    /* 只给写区分配数组，保存本轮及后续各轮要写入的固定值。 */
    uint16_t *values;
} point_t;

typedef struct {
    int rtu;
    const char *ip;
    const char *device;
    int port;
    int baud;
    int data_bits;
    int stop_bits;
    char parity;
    int slave;
    int interval_ms;
    int timeout_ms;
    int cycles;
    int frame_output;
    int point_count;
    int read_count;
    point_t points[MAX_POINTS];
} options_t;

/* 信号处理函数仅修改原子标志，不在异步信号上下文中打印、关闭连接或释放内存。 */
static volatile sig_atomic_t stop_requested;

static void request_stop(int signum)
{
    (void) signum;
    stop_requested = 1;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "用法：\n"
            "  %s tcp [参数]\n"
            "  %s rtu --device /dev/ttyS1 [参数]\n"
            "TCP 参数：\n"
            "  --ip IP                对方 IPv4 地址，默认 192.168.9.60\n"
            "  --port N               对方端口，默认 502，范围 1..65535\n"
            "RTU 参数：\n"
            "  --device PATH          运行本程序的设备上的串口路径，必须指定\n"
            "  --baud N               波特率，默认 9600\n"
            "  --data-bits N          数据位，默认 8，范围 5..8\n"
            "  --stop-bits N          停止位，默认 1，可选 1/2\n"
            "  --parity N|E|O         无/偶/奇校验，默认 N，大小写均可\n"
            "公共参数：\n"
            "  --slave N              站地址，默认 1，范围 1..247\n"
            "  --interval MS          每轮结束后的等待毫秒数，默认 1000，可为 0\n"
            "  --timeout MS           响应及字节超时，默认 1000 毫秒\n"
            "  --cycles N             轮询次数，默认 0 表示持续运行，Ctrl+C 退出\n"
            "  --frame-output 0|1     是否显示 TX/RX 原始帧，默认 1\n"
            "点位参数（均可重复，最多合计 64 个区）：\n"
            "  --read-coils A:N        读取线圈\n"
            "  --read-discrete A:N     读取离散输入（只读）\n"
            "  --read-holding A:N      读取保持寄存器\n"
            "  --read-input A:N        读取输入寄存器（只读）\n"
            "  --write-coils A:V,...   循环写线圈并回读，值只能为 0/1\n"
            "  --write-registers A:V,... 循环写保持寄存器并回读，值为 0..65535\n"
            "  --help                 显示帮助\n"
            "A 为 0 基地址，N 为点数；地址、点数、值可使用十进制或 0x 十六进制。\n"
            "未设置读区时默认 --read-holding 0:1；未设置写区时不写入。\n"
            "每轮先读取全部读区，再写入各写区并立即回读校验；暂停帧输出不影响读写。\n"
            "退出码：0=正常，1=连接/通讯/校验失败，2=参数错误。\n",
            program,
            program);
}

/* 严格解析整数：不接受符号、空格、尾随字符及溢出；前导 0 仍按十进制。
 * 在调用 libmodbus 前验证，防止将截断后的地址、数量或写值发送到设备。 */
static int parse_number(const char *text, int minimum, int maximum, int *value)
{
    char *end;
    unsigned long number;
    int base;

    if (text == NULL || !isdigit((unsigned char) text[0]))
        return -1;
    base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
    errno = 0;
    number = strtoul(text, &end, base);
    if (errno == ERANGE || end == text || *end != '\0' ||
        number < (unsigned long) minimum || number > (unsigned long) maximum)
        return -1;
    *value = (int) number;
    return 0;
}

static void free_options(options_t *options)
{
    int i;
    for (i = 0; i < options->point_count; i++)
        free(options->points[i].values);
}

/* 解析 A:N 或 A:V,V,...；只有全部验证通过后才将该区加入有效配置。
 * 读数量与写数量分别遵循对应功能码限制，且最后一点不能超过地址 65535。 */
static int add_point(options_t *options, point_type_t type, const char *argument)
{
    point_t point = {0};
    char *copy = strdup(argument);
    char *cursor;
    char *colon;
    int maximum;
    int result = -1;

    if (copy == NULL || options->point_count == MAX_POINTS)
        goto done;
    point.type = type;
    colon = strchr(copy, ':');
    if (colon == NULL)
        goto done;
    *colon = '\0';
    cursor = colon + 1;
    if (parse_number(copy, 0, 65535, &point.address) == -1)
        goto done;
    if (type < WRITE_COILS) {
        maximum =
            type <= READ_DISCRETE ? MODBUS_MAX_READ_BITS : MODBUS_MAX_READ_REGISTERS;
        if (parse_number(cursor, 1, maximum, &point.count) == -1)
            goto done;
    } else {
        maximum =
            type == WRITE_COILS ? MODBUS_MAX_WRITE_BITS : MODBUS_MAX_WRITE_REGISTERS;
        point.values = calloc(maximum, sizeof(*point.values));
        if (point.values == NULL)
            goto done;
        /* 手动切分而非 strtok，确保 1,,2 和尾部逗号不会被静默忽略。 */
        for (;;) {
            char *comma = strchr(cursor, ',');
            int value;
            if (comma != NULL)
                *comma = '\0';
            if (point.count == maximum ||
                parse_number(cursor, 0, type == WRITE_COILS ? 1 : 65535, &value) == -1)
                goto done;
            point.values[point.count++] = (uint16_t) value;
            if (comma == NULL)
                break;
            cursor = comma + 1;
        }
    }
    if (point.address + point.count > 65536)
        goto done;
    options->points[options->point_count++] = point;
    if (type < WRITE_COILS)
        options->read_count++;
    point.values = NULL; /* 所有权已转交 options，统一在退出时释放。 */
    result = 0;

done:
    free(point.values);
    free(copy);
    return result;
}

/* 所有参数在建立连接前解析。禁止把串口参数用于 TCP 或把 TCP 参数用于 RTU，
 * 避免用户误以为某项连接设置已经生效。返回 1 表示请求帮助，-1 表示参数错误。 */
static int parse_options(int argc, char **argv, options_t *options)
{
    static const char *point_flags[] = {"--read-coils",
                                        "--read-discrete",
                                        "--read-holding",
                                        "--read-input",
                                        "--write-coils",
                                        "--write-registers"};
    int tcp_parameters = 0;
    int rtu_parameters = 0;
    int i;
    struct in_addr ip_address;

    if (argc == 2 && strcmp(argv[1], "--help") == 0)
        return 1;
    if (argc < 2 || (strcasecmp(argv[1], "tcp") != 0 && strcasecmp(argv[1], "rtu") != 0))
        return -1;
    options->rtu = strcasecmp(argv[1], "rtu") == 0;
    options->ip = "192.168.9.60";
    options->port = 502;
    options->baud = 9600;
    options->data_bits = 8;
    options->stop_bits = 1;
    options->parity = 'N';
    options->slave = 1;
    options->interval_ms = 1000;
    options->timeout_ms = 1000;
    options->frame_output = TRUE;

    for (i = 2; i < argc; i++) {
        const char *flag = argv[i];
        const char *argument;
        int *destination = NULL;
        int minimum = 0;
        int maximum = INT_MAX;
        int j;

        if (strcmp(flag, "--help") == 0)
            return 1;
        if (++i >= argc) {
            fprintf(stderr, "参数缺少值：%s\n", flag);
            return -1;
        }
        argument = argv[i];
        for (j = READ_COILS; j <= WRITE_REGISTERS; j++) {
            if (strcmp(flag, point_flags[j]) == 0)
                break;
        }
        if (j <= WRITE_REGISTERS) {
            if (add_point(options, (point_type_t) j, argument) == -1)
                goto invalid;
            continue;
        }
        if (strcmp(flag, "--ip") == 0) {
            options->ip = argument;
            tcp_parameters = 1;
            continue;
        }
        if (strcmp(flag, "--device") == 0) {
            options->device = argument;
            rtu_parameters = 1;
            continue;
        }
        if (strcmp(flag, "--parity") == 0) {
            if (strlen(argument) != 1 || strchr("NEOneo", argument[0]) == NULL)
                goto invalid;
            options->parity = (char) toupper((unsigned char) argument[0]);
            rtu_parameters = 1;
            continue;
        }
        if (strcmp(flag, "--port") == 0) {
            destination = &options->port;
            minimum = 1;
            maximum = 65535;
            tcp_parameters = 1;
        } else if (strcmp(flag, "--baud") == 0) {
            destination = &options->baud;
            minimum = 1;
            rtu_parameters = 1;
        } else if (strcmp(flag, "--data-bits") == 0) {
            destination = &options->data_bits;
            minimum = 5;
            maximum = 8;
            rtu_parameters = 1;
        } else if (strcmp(flag, "--stop-bits") == 0) {
            destination = &options->stop_bits;
            minimum = 1;
            maximum = 2;
            rtu_parameters = 1;
        } else if (strcmp(flag, "--slave") == 0) {
            destination = &options->slave;
            minimum = 1;
            maximum = 247;
        } else if (strcmp(flag, "--interval") == 0) {
            destination = &options->interval_ms;
        } else if (strcmp(flag, "--timeout") == 0) {
            destination = &options->timeout_ms;
            minimum = 1;
            maximum = 600000;
        } else if (strcmp(flag, "--cycles") == 0) {
            destination = &options->cycles;
        } else if (strcmp(flag, "--frame-output") == 0) {
            destination = &options->frame_output;
            maximum = 1;
        }
        if (destination == NULL ||
            parse_number(argument, minimum, maximum, destination) == -1)
            goto invalid;
        continue;

    invalid:
        fprintf(stderr, "参数无效：%s %s\n", flag, argument);
        return -1;
    }
    if ((options->rtu &&
         (tcp_parameters || options->device == NULL || options->device[0] == '\0')) ||
        (!options->rtu &&
         (rtu_parameters || inet_pton(AF_INET, options->ip, &ip_address) != 1))) {
        fprintf(stderr,
                "连接参数错误：TCP 需要有效 IPv4；RTU 必须指定串口且不能混用参数。\n");
        return -1;
    }
    /* 即便只指定写区，仍提供一个明确的默认读区，方便观察独立的轮询读操作。 */
    if (options->read_count == 0 && add_point(options, READ_HOLDING, "0:1") == -1)
        return -1;
    return 0;
}

/* 同步回调只在当前调用中读取 data，不保存临时缓冲区指针。
 * 输出完整 ADU（RTU 包含 CRC，TCP 包含 MBAP），不依赖 stdout debug 拦截。 */
static void print_frame(modbus_t *ctx,
                        modbus_frame_direction_t direction,
                        const uint8_t *data,
                        int length,
                        void *user_data)
{
    const options_t *options = user_data;
    int i;
    (void) ctx;
    printf("[%s %s][站%d][%s] ",
           options->rtu ? "RTU" : "TCP",
           options->rtu ? options->device : options->ip,
           options->slave,
           direction == MODBUS_FRAME_TX ? "TX" : "RX");
    for (i = 0; i < length; i++)
        printf("%02X ", data[i]);
    printf("\n");
}

static const char *point_name(point_type_t type)
{
    static const char *names[] = {
        "线圈", "离散输入", "保持寄存器", "输入寄存器", "线圈", "保持寄存器"};
    return names[type];
}

static int report_io_error(const point_t *point, const char *operation)
{
    int saved_errno = errno;
    fprintf(stderr,
            "错误：%s%s 地址=%d 数量=%d：%s (errno=%d)\n",
            operation,
            point_name(point->type),
            point->address,
            point->count,
            modbus_strerror(saved_errno),
            saved_errno);
    return -1;
}

/* 运行一个读区或写区。写区使用 0x0F/0x10 批量写入，随后使用 0x01/0x03
 * 回读同一区域；写成功只说明从站确认了请求，只有比对一致才算本次校验通过。 */
static int run_point(modbus_t *ctx, const point_t *point)
{
    uint8_t bits[MODBUS_MAX_READ_BITS];
    uint16_t registers[MODBUS_MAX_READ_REGISTERS];
    int writing = point->type >= WRITE_COILS;
    int bit_data = point->type <= READ_DISCRETE || point->type == WRITE_COILS;
    int rc;
    int i;

    if (stop_requested)
        return 0;
    if (writing) {
        if (point->type == WRITE_COILS) {
            for (i = 0; i < point->count; i++)
                bits[i] = (uint8_t) point->values[i];
            rc = modbus_write_bits(ctx, point->address, point->count, bits);
        } else {
            rc = modbus_write_registers(ctx, point->address, point->count, point->values);
        }
        if (stop_requested)
            return 0;
        if (rc != point->count)
            return report_io_error(point, "写入");
        printf("[写入]%s 地址=%d 数量=%d 值=",
               point_name(point->type),
               point->address,
               point->count);
        for (i = 0; i < point->count; i++)
            printf("%s%u", i == 0 ? "" : ",", (unsigned int) point->values[i]);
        printf("\n");
    }
    switch (point->type) {
    case READ_COILS:
    case WRITE_COILS:
        rc = modbus_read_bits(ctx, point->address, point->count, bits);
        break;
    case READ_DISCRETE:
        rc = modbus_read_input_bits(ctx, point->address, point->count, bits);
        break;
    case READ_HOLDING:
    case WRITE_REGISTERS:
        rc = modbus_read_registers(ctx, point->address, point->count, registers);
        break;
    case READ_INPUT:
        rc = modbus_read_input_registers(ctx, point->address, point->count, registers);
        break;
    default:
        return -1;
    }
    if (stop_requested)
        return 0;
    if (rc != point->count)
        return report_io_error(point, writing ? "回读" : "读取");
    printf("[%s]%s 地址=%d 数量=%d 值=",
           writing ? "回读" : "读取",
           point_name(point->type),
           point->address,
           point->count);
    for (i = 0; i < point->count; i++)
        printf("%s%u",
               i == 0 ? "" : ",",
               (unsigned int) (bit_data ? bits[i] : registers[i]));
    printf("\n");
    if (writing) {
        for (i = 0; i < point->count; i++) {
            uint16_t actual = bit_data ? bits[i] : registers[i];
            if (actual != point->values[i]) {
                fprintf(stderr,
                        "错误：%s回读不一致 地址=%d 期望=%u 实际=%u\n",
                        point_name(point->type),
                        point->address + i,
                        (unsigned int) point->values[i],
                        (unsigned int) actual);
                return -1;
            }
        }
        printf("[校验]%s 地址=%d 数量=%d 全部一致\n",
               point_name(point->type),
               point->address,
               point->count);
    }
    return 0;
}

/* 每轮等待采用 nanosleep；信号打断后检查退出标志，不会在长轮询间隔中卡住。 */
static void wait_interval(int milliseconds)
{
    struct timespec remaining;
    remaining.tv_sec = milliseconds / 1000;
    remaining.tv_nsec = (long) (milliseconds % 1000) * 1000000L;
    while (!stop_requested && nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {
    }
}

int main(int argc, char **argv)
{
    options_t options = {0};
    struct sigaction action;
    modbus_t *ctx = NULL;
    unsigned long rounds = 0;
    unsigned long errors = 0;
    int connected = 0;
    int result = EXIT_FAILURE;
    int parsed;

    /* 行缓冲使重定向日志和终端中都能及时看到每条帧及每次点位结果。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    parsed = parse_options(argc, argv, &options);
    if (parsed != 0) {
        print_usage(parsed > 0 ? stdout : stderr, argv[0]);
        free_options(&options);
        return parsed > 0 ? EXIT_SUCCESS : 2;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("设置信号处理失败");
        goto done;
    }
    if (options.rtu) {
        printf("连接 RTU：%s 波特率=%d 数据位=%d 停止位=%d 校验=%c 站地址=%d\n",
               options.device,
               options.baud,
               options.data_bits,
               options.stop_bits,
               options.parity,
               options.slave);
        ctx = modbus_new_rtu(options.device,
                             options.baud,
                             options.parity,
                             options.data_bits,
                             options.stop_bits);
    } else {
        printf("连接 TCP：%s:%d 站地址=%d\n", options.ip, options.port, options.slave);
        ctx = modbus_new_tcp(options.ip, options.port);
    }
    if (ctx == NULL) {
        fprintf(stderr, "创建通讯实例失败：%s\n", modbus_strerror(errno));
        goto done;
    }
    /* 先配置超时再连接：TCP 连接和后续响应都有明确的等待上限。
     * 注册的 options 位于 main 栈上，保持到停止通讯并释放 ctx 后才离开作用域。 */
    if (modbus_set_slave(ctx, options.slave) == -1 ||
        modbus_set_response_timeout(
            ctx, options.timeout_ms / 1000, (options.timeout_ms % 1000) * 1000) == -1 ||
        modbus_set_byte_timeout(
            ctx, options.timeout_ms / 1000, (options.timeout_ms % 1000) * 1000) == -1 ||
        modbus_set_frame_callback(ctx, print_frame, &options) == -1 ||
        modbus_set_frame_callback_enabled(ctx, options.frame_output) == -1) {
        fprintf(stderr, "配置通讯参数失败：%s\n", modbus_strerror(errno));
        goto done;
    }
    /* 不开启标准 debug，避免与自定义完整帧输出重复。 */
    if (modbus_connect(ctx) == -1) {
        fprintf(stderr, "连接失败：%s\n", modbus_strerror(errno));
        goto done;
    }
    connected = 1;
    while (!stop_requested &&
           (options.cycles == 0 || rounds < (unsigned long) options.cycles)) {
        int phase;
        int i;
        rounds++;
        printf("--- 第 %lu 轮 ---\n", rounds);
        /* 不依赖命令行顺序：先读取所有读区，再对各写区逐一写入并回读。 */
        for (phase = 0; phase < 2 && !stop_requested; phase++) {
            for (i = 0; i < options.point_count && !stop_requested; i++) {
                point_t *point = &options.points[i];
                if ((point->type >= WRITE_COILS) != phase)
                    continue;
                if (run_point(ctx, point) == -1)
                    errors++;
            }
        }
        if (!stop_requested &&
            (options.cycles == 0 || rounds < (unsigned long) options.cycles))
            wait_interval(options.interval_ms);
    }
    printf("轮询汇总：轮次=%lu 错误=%lu\n", rounds, errors);
    /* 有限轮询或 Ctrl+C 退出时，不能用最后一次成功掩盖之前出现的错误。 */
    result = errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
    if (ctx != NULL) {
        modbus_set_frame_callback(ctx, NULL, NULL);
        if (connected)
            modbus_close(ctx);
        modbus_free(ctx);
    }
    free_options(&options);
    return result;
}
