# modbus_set_frame_callback_enabled

## 名称

modbus_set_frame_callback_enabled — 暂停或恢复单个实例的报文回调输出。

## 声明

```c
int modbus_set_frame_callback_enabled(modbus_t *ctx, int enabled);
```

## 说明

`enabled` 为 `FALSE`（0）时，停止该实例的完整 TX/RX 报文回调；为 `TRUE`
或其他非零值时，恢复回调。每个 `modbus_t` 独立保存开关状态。

关闭开关会保留通过
[modbus_set_frame_callback](modbus_set_frame_callback.md) 注册的函数和
`user_data`，重新开启无需再次注册。关闭期间的报文不会缓存，也不会在恢复后
补发。实际 Modbus 收发、完整性检查、错误码和连接状态继续正常工作。

为兼容原有代码，新实例的开关默认开启，但回调函数默认未注册，因此不会输出。
注册、替换、注销回调均不改变开关状态：先关闭再注册仍保持暂停；无回调时开启
也不会输出。需要彻底注销时使用 `modbus_set_frame_callback(ctx, NULL, NULL)`。

此接口仅控制回调路径。原有 stdout/stderr 调试输出仍由
[modbus_set_debug](modbus_set_debug.md) 控制。如需同时停止两种输出，分别调用
这两个接口。

调用须与同一实例的通讯操作串行执行，内部不提供线程同步。暂停不等于注销，
不能因此释放仍被注册的用户上下文。应用已复制并入队的日志不会由本接口清除。
完整的数据生命周期及回调约束参见注册接口文档。

## 返回值

成功返回 0。`ctx == NULL` 时返回 -1，并设置 `errno = EINVAL`。

## 用法

```c
/* 检查 ctx 已创建成功后，注册函数及所属通道。 */
modbus_set_frame_callback(ctx, frame_cb, channel);

/* 暂停回调输出，通讯照常进行。 */
modbus_set_frame_callback_enabled(ctx, FALSE);

/* 恢复输出，继续使用原 frame_cb 和 channel。 */
modbus_set_frame_callback_enabled(ctx, TRUE);

/* 标准 debug 输出独立控制。 */
modbus_set_debug(ctx, FALSE);

/* 不再需要回调时先注销，再安全释放 channel。 */
modbus_set_frame_callback(ctx, NULL, NULL);
```

实际应用应检查接口返回值，并使用本次构建的头文件与库文件共同编译、链接。
