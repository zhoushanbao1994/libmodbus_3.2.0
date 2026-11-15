# libmodbus 增加多实例报文回调的完整修改方案

推荐直接修改 libmodbus 源码，在原有 `modbus_t` 上增加独立的报文回调接口，不改变现有 Modbus RTU/TCP 通讯逻辑。

设计目标：

- 支持多个 `modbus_t` 实例同时通信，例如多个串口、多个 TCP 连接。
- 每个实例可以独立设置报文回调，也可以将多个实例的报文转发到同一个显示接口。
- 同一实例支持多个显示订阅者，例如控制台、文件日志、Qt UI、WebSocket。
- 分别回调完整的 TX、RX 原始 HEX 数据，包含 RTU CRC 或 TCP MBAP。
- 支持开启、关闭报文回调，不影响 `modbus_set_debug()`。
- 接口使用纯 C 实现，能够被 C、C++、Qt 和嵌入式 Linux 应用直接调用。
- 不改变现有 `modbus_read_registers()`、`modbus_write_register()` 等 API 的使用方式。

## 一、整体架构

核心设计：每个 `modbus_t` 绑定一个底层回调，底层回调将报文交给上层分发器，上层分发器再调用多个显示接口。

这样避免直接维护全局日志函数，也不需要在多个线程中拦截 `stdout`。

## 二、libmodbus 源码修改位置

源码：[stephane/libmodbus](https://github.com/stephane/libmodbus?utm_source=chatgpt.com)

只需要修改三个文件：

| 文件                   | 修改内容                                     |
| ---------------------- | -------------------------------------------- |
| `src/modbus.h`         | 增加公开回调类型、回调方向、注册 API         |
| `src/modbus-private.h` | 在 `struct _modbus` 内增加回调指针和用户数据 |
| `src/modbus.c`         | 实现注册函数，增加 TX/RX 回调触发            |

目前主线源码中，`send_msg()` 统一处理协议报文发送，`_modbus_receive_msg()` 负责接收和组装完整协议报文；这两个位置适合集中插入回调，无需分别修改 `modbus-rtu.c`、`modbus-tcp.c`。

GitHub+1



## 三、修改 modbus.h：增加公开 API

在 `src/modbus.h` 中，`modbus_t` 类型定义之后添加：

```
/* Modbus 报文方向 */
typedef enum {
    MODBUS_FRAME_TX = 0,
    MODBUS_FRAME_RX = 1
} modbus_frame_direction_t;

/*
 * 完整 Modbus ADU 报文回调
 *
 * ctx       : 当前 Modbus 实例
 * direction : TX / RX
 * data      : 原始完整报文
 * length    : 报文长度
 * user_data : 用户自定义上下文
 *
 * data 仅在回调执行期间有效。
 * 如需异步处理，必须复制数据。
 */
typedef void (*modbus_frame_callback_t)(
    modbus_t *ctx,
    modbus_frame_direction_t direction,
    const uint8_t *data,
    int length,
    void *user_data
);

/*
 * 注册报文回调
 * callback == NULL 表示关闭回调
 */
MODBUS_API int modbus_set_frame_callback(
    modbus_t *ctx,
    modbus_frame_callback_t callback,
    void *user_data
);
```

接口特点：

- 同一个回调函数可以注册到多个 `modbus_t`。
- 通过 `ctx` 识别报文属于哪个通讯实例。
- 通过 `user_data` 指定报文应该送往哪个上层对象。
- 无需使用全局变量区分不同串口和 TCP 连接。

注册示例：

```
modbus_set_frame_callback(ctx1, frame_cb, logger1);
modbus_set_frame_callback(ctx2, frame_cb, logger2);
modbus_set_frame_callback(ctx3, frame_cb, logger3);
```

三个实例使用相同函数，但是各自拥有独立的 `user_data`。

## 四、修改 modbus-private.h：增加上下文成员

找到：

```
struct _modbus {
    ...
};
```

在结构体末尾加入：

```
/* 自定义完整报文回调 */
modbus_frame_callback_t frame_callback;

/* 回调用户上下文 */
void *frame_user_data;
```

这些字段属于每个 `modbus_t` 自己的内存，不是全局变量，因此多个连接不会相互覆盖。

## 五、修改 modbus.c：实现回调注册

在 `modbus.c` 中增加：

```
int modbus_set_frame_callback(
    modbus_t *ctx,
    modbus_frame_callback_t callback,
    void *user_data)
{
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    ctx->frame_callback = callback;
    ctx->frame_user_data =
        callback ? user_data : NULL;

    return 0;
}

/* 库内部使用的回调触发函数 */
static void _modbus_emit_frame(
    modbus_t *ctx,
    modbus_frame_direction_t direction,
    const uint8_t *data,
    int length)
{
    if (ctx == NULL ||
        ctx->frame_callback == NULL ||
        data == NULL ||
        length <= 0) {
        return;
    }

    int saved_errno = errno;

    ctx->frame_callback(
        ctx,
        direction,
        data,
        length,
        ctx->frame_user_data
    );

    errno = saved_errno;
}
```

其中保存及恢复 `errno` 很重要，因为日志回调内部可能调用 `write()`、文件操作等系统函数，不能让日志系统意外改变 libmodbus 对外报告的错误码。

### 初始化回调字段

找到 `_modbus_init_common()`，添加：

```
ctx->frame_callback = NULL;
ctx->frame_user_data = NULL;
```

保证所有新创建的 RTU/TCP 实例默认不输出报文，保持原有性能和行为。

## 六、修改 TX 发送函数：回调完整发送报文

在 `modbus.c` 找到：

```
static int send_msg(
    modbus_t *ctx,
    uint8_t *msg,
    int msg_length)
```

原函数关键流程：

```
msg_length = ctx->backend->send_msg_pre(
    msg, msg_length
);

// 原有 Debug 输出

do {
    rc = ctx->backend->send(
        ctx, msg, msg_length
    );
    // 原有错误恢复逻辑
} while (...);
```

`send_msg_pre()` 会完成发送前的协议处理，RTU 后端会补齐 CRC，TCP 后端会处理 MBAP 长度字段，因此不能在它之前调用日志接口。

GitHub+1



推荐在原有发送循环内，每次成功发送完整报文之后触发一次回调。

修改方法：

```
/*
 * 修改 send_msg() 内部原有 do 循环。
 * 保留原有错误恢复代码，只增加回调部分。
 */

do {
    rc = ctx->backend->send(ctx, msg, msg_length);

    if (rc == msg_length) {

        /* 成功提交完整报文后触发 TX */
        _modbus_emit_frame(
            ctx,
            MODBUS_FRAME_TX,
            msg,
            msg_length
        );
    }

    /*
     * 这里继续保留 libmodbus 原有的：
     * 发送失败处理
     * 错误恢复
     * 重连和重试
     */
    if (rc == -1) {
        // 原有代码保持不变
    }

} while (
    (ctx->error_recovery & MODBUS_ERROR_RECOVERY_LINK)
    && rc == -1
);
```

以上是插入位置示意，不要把现有的错误恢复逻辑替换成空代码。

这样处理之后：

| 发送情况               | TX 回调          |
| ---------------------- | ---------------- |
| 发送完整报文成功       | 触发一次         |
| 发送失败 `-1`          | 不触发           |
| 发送成功后接收超时     | 已触发 TX        |
| 自动重试并再次成功发送 | 再触发一次       |
| RTU CRC                | 包含在回调数据中 |
| TCP MBAP               | 包含在回调数据中 |

需要说明：`send()` 报告成功表示完整数据已交给相应传输接口，不保证远端设备已经收到。若需要记录失败的发送尝试，可以后续扩展 `TX_ATTEMPT`、`TX_ERROR` 事件。对于 TCP，即使 `send()` 成功，也不代表数据已经到达网络对端。

## 七、修改 RX 接收函数：回调完整接收报文

找到：

```
int _modbus_receive_msg(
    modbus_t *ctx,
    uint8_t *msg,
    msg_type_t msg_type)
```

函数内部通过多次 `recv()` 逐步接收报文，最后进行完整性校验。

原函数末尾：

```
if (ctx->debug)
    printf("\n");

return ctx->backend->check_integrity(
    ctx, msg, msg_length
);
```

推荐替换为：

```
if (ctx->debug)
    printf("\n");

/*
 * 先保留一次完整性检查。
 * 注意：返回值可能为负数、0 或完整报文长度。
 */
int integrity_rc =
    ctx->backend->check_integrity(
        ctx, msg, msg_length
    );

/*
 * 到达此处说明协议接收流程已取得一条
 * 按当前帧长规则组装的完整报文。
 *
 * 即使 CRC 错误，也应提供给调试界面。
 */
_modbus_emit_frame(
    ctx,
    MODBUS_FRAME_RX,
    msg,
    msg_length
);

return integrity_rc;
```

注意区别：

- `msg_length`：实际接收到并组装出的报文长度。
- `integrity_rc`：库进行 CRC、从站地址等完整性检查后的返回值。

例如：

```
RX: 01 03 04 00 64 00 C8 BA 7A
```

如果 CRC 错误，仍然能把这条原始接收帧交给你的调试界面。

为什么不直接在 `recv()` 之后回调？

因为 Modbus TCP 使用流式传输，单次 `recv()` 可能只接收几个字节；Modbus RTU 同样可能需要多次 `read()` 才能收齐完整报文。在 `recv()` 后直接回调会导致调试界面出现很多不完整的数据片段。

另外，这里触发 RX 的时机是在完整性检查之后、最终业务响应校验之前，因此它是“收到完整帧”的事件，不等于“这次主站请求成功”。例如收到错误的事务 ID、Modbus 异常响应或者非法数据长度，仍可能触发 RX 回调。

当前修改方案对接收中途超时、无法组装完成的帧不会触发完整 RX 回调。若需要显示这种不完整数据，建议另行增加 `RX_PARTIAL` 诊断事件，而不要混入正常的 `RX`。

## 八、实现多个 Modbus 实例统一回调

下面是应用层 C++ 示例。它既支持多实例，也支持按照指定接口名称路由报文。

先定义一个接口上下文：

```
#include <modbus/modbus.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct ModbusChannel {
    std::string name;       // 显示名称
    std::string interface;  // ttyS1 / TCP 地址等
    int channelId;          // 唯一通道编号
};

void printModbusFrame(
    modbus_t* ctx,
    modbus_frame_direction_t direction,
    const uint8_t* data,
    int length,
    void* user_data)
{
    (void)ctx;

    auto* channel =
        static_cast<ModbusChannel*>(user_data);

    if (!channel)
        return;

    printf(
        "[%d][%s][%s][%s] ",
        channel->channelId,
        channel->name.c_str(),
        channel->interface.c_str(),
        direction == MODBUS_FRAME_TX ? "TX" : "RX"
    );

    for (int i = 0; i < length; ++i) {
        printf("%02X ", data[i]);
    }

    printf("\n");
}
```

创建三个通讯实例：

```
ModbusChannel channel1 {
    "RS485-1", "/dev/ttyS1", 1
};

ModbusChannel channel2 {
    "RS485-2", "/dev/ttyS2", 2
};

ModbusChannel channel3 {
    "TCP-PLC", "192.168.1.100:502", 3
};

modbus_t* ctx1 = modbus_new_rtu(
    "/dev/ttyS1", 9600, 'N', 8, 1
);

modbus_t* ctx2 = modbus_new_rtu(
    "/dev/ttyS2", 9600, 'N', 8, 1
);

modbus_t* ctx3 = modbus_new_tcp(
    "192.168.1.100", 502
);

// 分别注册到三个实例
modbus_set_frame_callback(
    ctx1, printModbusFrame, &channel1
);

modbus_set_frame_callback(
    ctx2, printModbusFrame, &channel2
);

modbus_set_frame_callback(
    ctx3, printModbusFrame, &channel3
);
```

这里省略了实例创建失败检查、从站地址设置、连接建立和资源释放。实际工程应当检查这些步骤，并保证 `ModbusChannel` 的生存期覆盖所有使用它的回调。

多线程运行时，输出类似：

```
[1][RS485-1][/dev/ttyS1][TX]
01 03 00 00 00 02 C4 0B

[2][RS485-2][/dev/ttyS2][TX]
02 04 00 10 00 02 70 3C

[3][TCP-PLC][192.168.1.100:502][TX]
00 01 00 00 00 06 01 03 00 00 00 02

[1][RS485-1][/dev/ttyS1][RX]
01 03 04 00 64 00 C8 BA 7A
```

每条报文都包含自己的来源，不会因为多个实例共用一个回调函数而丢失通道信息。

## 九、实现同一 Modbus 报文分发到多个指定显示接口

前面的 `modbus_set_frame_callback()` 每个实例只保存一个函数指针。为了支持一条报文同时发送给多个处理接口，推荐在 C++ 应用层增加 `FrameDispatcher`。

例如，同一条 RTU 通讯报文需要同时发送到：

- `Console`：终端显示。
- `FileLogger`：持久化日志文件。
- `QtMonitor`：Qt 通讯监控窗口。
- `WebMonitor`：WebSocket 远程调试客户端。

### 1. 实现多订阅者分发器

```
#pragma once

#include <modbus/modbus.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ModbusFrame {
    int channelId;
    std::string interfaceName;
    modbus_frame_direction_t direction;
    std::vector<uint8_t> data;
};

class FrameDispatcher {
public:
    using Callback =
        std::function<void(const ModbusFrame&)>;

    // 注册一个显示接口，返回订阅 ID
    uint64_t subscribe(Callback cb)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        uint64_t id = nextId_++;
        subscribers_[id] = std::move(cb);
        return id;
    }

    // 取消订阅
    void unsubscribe(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.erase(id);
    }

    // 发布一条报文
    void publish(const ModbusFrame& frame)
    {
        std::vector<Callback> callbacks;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            for (const auto& item : subscribers_) {
                callbacks.push_back(item.second);
            }
        }

        // 不持锁执行用户回调
        for (auto& cb : callbacks) {
            try {
                cb(frame);
            } catch (...) {
                // 生产环境应记录异常或错误计数
                // 不能让异常穿越 libmodbus 的 C 回调边界
            }
        }
    }

private:
    std::mutex mutex_;
    uint64_t nextId_ = 1;
    std::unordered_map<uint64_t, Callback> subscribers_;
};
```

这个实现允许在运行时动态增加和删除显示接口。分发时先复制回调列表，再执行回调，避免在锁内进行文件操作或 UI 更新。

### 2. 将底层 libmodbus 回调接入分发器

```
struct FrameContext {
    FrameDispatcher* dispatcher;
    int channelId;
    std::string interfaceName;
};

void onModbusFrame(
    modbus_t* ctx,
    modbus_frame_direction_t direction,
    const uint8_t* data,
    int length,
    void* user_data)
{
    (void)ctx;

    auto* context = static_cast<FrameContext*>(user_data);

    if (!context ||
        !context->dispatcher ||
        !data ||
        length <= 0) {
        return;
    }

    try {
        ModbusFrame frame;

        frame.channelId = context->channelId;
        frame.interfaceName = context->interfaceName;
        frame.direction = direction;

        // 必须复制：原始指针在回调返回后可能失效
        frame.data.assign(data, data + length);

        context->dispatcher->publish(frame);
    } catch (...) {
        // 不允许异常传回 C 库
    }
}
```

### 3. 注册多个显示接口

```
FrameDispatcher dispatcher;

// 注册控制台显示
auto consoleId = dispatcher.subscribe(
    [](const ModbusFrame& frame) {
        printf(
            "[%s][%s] ",
            frame.interfaceName.c_str(),
            frame.direction == MODBUS_FRAME_TX
                ? "TX" : "RX"
        );

        for (uint8_t byte : frame.data) {
            printf("%02X ", byte);
        }
        printf("\n");
    }
);

// 注册文件日志
auto fileId = dispatcher.subscribe(
    [](const ModbusFrame& frame) {
        // 发送到自定义文件日志模块
        // FileLogger::write(frame);
    }
);

// 注册 Qt 显示窗口
auto qtId = dispatcher.subscribe(
    [](const ModbusFrame& frame) {
        // 将报文投递到 Qt UI 线程
        // QtMonitor::postFrame(frame);
    }
);

// 注册 WebSocket 显示
auto webId = dispatcher.subscribe(
    [](const ModbusFrame& frame) {
        // 发送到 WebSocket 的消息队列
        // WebMonitor::postFrame(frame);
    }
);
```

例如将一个 Modbus 通道注册到该分发器：

```
FrameContext context {
    &dispatcher,
    1,
    "/dev/ttyS1"
};

modbus_set_frame_callback(
    ctx1,
    onModbusFrame,
    &context
);
```

其他实例也可以注册到同一个 `dispatcher`，使用各自的 `FrameContext`。

如果某个接口不再需要显示报文，只要执行：

```
dispatcher.unsubscribe(qtId);
```

不会影响其他接口，更不需要关闭 Modbus 连接。

目前 `publish()` 是同步分发，不能让订阅函数长时间阻塞。用于高频轮询、WebSocket 或 Qt 的生产环境时，应在回调里仅复制并投递报文到有界线程安全队列，由其他线程完成日志写入和界面刷新。

### 4. 支持按指定接口过滤

假设 Qt 窗口只显示 `/dev/ttyS1`：

```
auto qtId = dispatcher.subscribe(
    [](const ModbusFrame& frame) {

        if (frame.interfaceName != "/dev/ttyS1")
            return;

        // postFrameToQt(frame);
    }
);
```

也可以根据 `channelId` 过滤。推荐使用唯一的通道 ID 作为主要过滤条件，避免重命名串口或网络地址后导致日志路由不正确。

## 十、生产环境需要考虑的几个问题

| 事项               | 建议                                            |
| ------------------ | ----------------------------------------------- |
| 多个通讯线程       | 每个连接独立 `modbus_t`，共享线程安全日志分发器 |
| 同一串口下多个从站 | 共用一个 `modbus_t`，在应用层串行调度           |
| 回调阻塞           | 回调只复制数据并入队                            |
| 回调启停           | 注册、注销应与该实例通讯线程串行执行            |
| 内存生命周期       | `user_data` 在注销回调及当前回调结束后才能释放  |
| 串口 CRC 异常      | 保留完整 RX 报文，并增加错误状态事件            |
| 接收超时           | 建议另设错误回调，携带 `errno`                  |
| 日志性能           | 采用有界队列，支持日志等级和超量丢弃统计        |
| 多线程日志排序     | 增加单调时间戳及递增事件序号                    |
| 动态库兼容         | 增加导出符号，应用使用同一版本的头文件及动态库  |

另外，同一个 `modbus_t` 不应被多个线程同时执行读写事务，否则请求和响应可能交叉。多订阅者日志分发并不意味着通讯实例本身支持并发请求。

### 回调与通讯异常状态的关系

建议后续再增加一个可选的事件回调：

```
typedef enum {
    MODBUS_EVENT_TX,
    MODBUS_EVENT_RX,
    MODBUS_EVENT_TIMEOUT,
    MODBUS_EVENT_CRC_ERROR,
    MODBUS_EVENT_IO_ERROR
} modbus_event_type_t;
```

这比单纯输出 HEX 更适合完整通讯调试。需要注意的是，错误事件和完整帧事件不能完全混为一谈：例如超时可能根本没有接收到任何数据。

## 十一、编译和验证

完成源码修改后，在 libmodbus 源码根目录执行：

```
./autogen.sh
./configure --prefix=/opt/libmodbus-custom
make -j$(nproc)
sudo make install
```

如果使用的是发行版源码包且已经存在 `configure`，可以省略 `autogen.sh`。

应用编译：

```
g++ -std=c++17 main.cpp \
  -I/opt/libmodbus-custom/include/modbus \
  -L/opt/libmodbus-custom/lib \
  -lmodbus -pthread \
  -o modbus_master
```

如果应用中使用的是 `<modbus/modbus.h>`，则头文件路径应改为 `-I/opt/libmodbus-custom/include`，避免包含路径不匹配。

运行前还需要确保动态链接器找到自定义库，例如设置对应的 `rpath` 或 `LD_LIBRARY_PATH`，并检查实际加载的不是系统自带旧版本。

建议至少验证四种场景：

1. 两个 RTU 实例和一个 TCP 实例同时轮询，确认报文来源没有混淆。
2. 同一条报文发送到两个显示接口，关闭其中一个订阅，确认另一个继续工作。
3. 模拟 CRC 错误、超时和 TCP 断线，确认报文回调不会影响原有错误处理。
4. 通讯中动态启停日志，反复连接和释放实例，检查内存及并发问题。

## 十二、最终推荐的 API

如果希望长期维护一个自己扩展过的 libmodbus 分支，我建议底层先保持三个最简单的接口：

```
/* 注册或替换报文回调 */
int modbus_set_frame_callback(
    modbus_t *ctx,
    modbus_frame_callback_t callback,
    void *user_data
);

/* 禁止报文回调 */
modbus_set_frame_callback(ctx, NULL, NULL);

/* 原有标准调试接口仍可独立使用 */
modbus_set_debug(ctx, FALSE);
```

最终结构是：

libmodbus 负责完整原始报文回调，应用层 `FrameDispatcher` 负责多订阅者和指定接口路由。

相比直接在 libmodbus 内实现复杂的多回调链表，这样改动更小、库与 UI 耦合更少，也方便后续把 DL/T 645、IEC 104 等协议接入同一套报文调试系统。

上述代码是一套基于已核对源码调用链的修改方案，并非已编译验证的完整补丁。实际落地时，建议基于固定的 libmodbus 版本提交修改并配套单元测试。
