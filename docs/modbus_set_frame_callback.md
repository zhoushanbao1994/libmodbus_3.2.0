# modbus_set_frame_callback

## Name

modbus_set_frame_callback - register a per-context raw Modbus frame callback

## Synopsis

```c
typedef enum {
    MODBUS_FRAME_TX = 0,
    MODBUS_FRAME_RX = 1
} modbus_frame_direction_t;

typedef void (*modbus_frame_callback_t)(modbus_t *ctx,
                                        modbus_frame_direction_t direction,
                                        const uint8_t *data,
                                        int length,
                                        void *user_data);

int modbus_set_frame_callback(modbus_t *ctx,
                              modbus_frame_callback_t callback,
                              void *user_data);

int modbus_set_frame_callback_enabled(modbus_t *ctx, int enabled);
```

## Description

The *modbus_set_frame_callback()* function registers or replaces the callback
and user pointer of `ctx`. Each context has one independent callback, disabled
by default. Passing `NULL` as `callback` disables it and clears the stored user
pointer. The same callback can serve multiple contexts, with separate
`user_data` values identifying their channels. Multiple subscribers can be
implemented by an application-level dispatcher.

Use [modbus_set_frame_callback_enabled](modbus_set_frame_callback_enabled.md) to
pause or resume TX/RX notifications while retaining the callback and user pointer.
The independent switch defaults to enabled; registration, replacement and removal
do not change it. Registering a callback while paused keeps notifications paused.

The callback receives the complete raw ADU, including the RTU CRC or TCP MBAP
header. It is independent of [modbus_set_debug](modbus_set_debug.md).

- `MODBUS_FRAME_TX` is emitted after the transport reports a complete successful
  send. Failed or short sends do not emit TX. A successful retry emits TX once;
  a later receive timeout does not undo TX. Success means the transport accepted
  the data, without guaranteeing delivery to the peer.
- `MODBUS_FRAME_RX` is emitted once a frame has been assembled according to the
  library's framing rules and its integrity has been checked. Complete frames
  rejected for CRC or filtered by slave address still emit RX. The callback
  length is the assembled byte count, even when the receive API returns 0 or -1.
  RX precedes transaction/response validation, so an exception response or wrong
  transaction ID can emit RX while the transaction fails. Incomplete frames,
  receive timeouts and early receive errors do not emit RX.

The callback runs synchronously in the thread performing I/O. `data` is borrowed,
read-only memory valid only during the callback; copy it before queuing it for
asynchronous work. Keep callbacks short. Callback changes to `errno` are restored
before libmodbus continues, including after integrity failures.

Registration, replacement, removal and switch changes must be serialized with
communications on the same context. Keep `user_data` alive until removal and any current callback
have completed. Do not free the context or perform reentrant I/O on it from the
callback. The context itself does not support concurrent transactions. C++
callbacks must catch exceptions before returning through the C API boundary.

## Return value

The function returns 0 on success. If `ctx` is `NULL`, it returns -1 and sets
`errno` to `EINVAL`.

## Example

```c
#include <modbus/modbus.h>
#include <stdio.h>

static void log_frame(modbus_t *ctx,
                      modbus_frame_direction_t direction,
                      const uint8_t *data,
                      int length,
                      void *user_data)
{
    const char *channel = user_data;
    int i;
    (void) ctx;
    printf("[%s][%s] ", channel, direction == MODBUS_FRAME_TX ? "TX" : "RX");
    for (i = 0; i < length; i++)
        printf("%02X ", data[i]);
    printf("\n");
}

/* After checking that ctx1 and ctx2 were successfully created: */
/* modbus_set_frame_callback(ctx1, log_frame, "RS485-1"); */
/* modbus_set_frame_callback(ctx2, log_frame, "TCP-PLC"); */
/* Pause and resume without losing the callback or its user pointer: */
/* modbus_set_frame_callback_enabled(ctx1, FALSE); */
/* modbus_set_frame_callback_enabled(ctx1, TRUE); */
/* Disable a channel before releasing its user context: */
/* modbus_set_frame_callback(ctx1, NULL, NULL); */
```

Applications must use the modified header together with the modified library;
the new registration symbol is absent from an unmodified libmodbus 3.2.0.
