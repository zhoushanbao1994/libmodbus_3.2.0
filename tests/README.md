# Tests

## License

Test programs of this directory are provided under BSD license (see associated
LICENSE file).

## Compilation

After installation, you can use pkg-config to compile these tests.
For example, to compile random-test-server run:

`gcc random-test-server.c -o random-test-server $(pkg-config --libs --cflags libmodbus)`

- `random-test-server` is necessary to launch a server before running
random-test-client. By default, it receives and replies to Modbus query on the
localhost and port 1502.

- `random-test-client` sends many different queries to a large range of
addresses and values to test the communication between the client and the
server.

- `unit-test-server` and `unit-test-client` run a full unit test suite. These
programs are essential to test the Modbus protocol implementation and libmodbus
behavior.

- `frame-callback-selftest` exercises per-context frame callbacks using POSIX socket
pairs and the real RTU/TCP backends, without serial hardware. It covers complete
ADUs, instance isolation, registration/replacement/removal, CRC failures,
filtered frames, fragmented and partial receives, timeout/disconnect behavior,
server replies, send failures, short writes, recovery and errno preservation.
Callback-switch tests cover paused TX/RX, resuming saved registrations, per-context
isolation, replacement while paused, nonzero enable values and debug independence
across RTU, TCP and TCP PI contexts.
It runs with `make check` on POSIX systems. From the Mac source tree, use
`./scripts/remote-build.sh native check`; RK3568 binaries require the target board.

- `bandwidth-server-one`, `bandwidth-server-many-up` and `bandwidth-client`
 return very useful information about the performance of transfer rate between
 the server and the client. `bandwidth-server-one` can only handles one
 connection at once with a client whereas `bandwidth-server-many-up` opens a
 connection for each new clients (with a limit).

## 帧回调设备测试程序（TCP / RTU）

`frame-callback-test.c` 是实际设备轮询工具，使用公开 libmodbus 接口，代码包含
详细中文注释。原 socket pair 回归程序保留为 `frame-callback-selftest.c`。
`make check` 执行原单元测试、帧回调自测和命令行集成测试，不连接实际设备。
命令行自动测试额外需要 Python 3 标准库；设备工具本身不需要 Python。

各源文件职责如下：

| 文件 | 作用 | 设备运行是否需要 |
| --- | --- | --- |
| `frame-callback-test.c` | TCP/RTU 设备轮询工具，读取四类点位、写入并回读校验、显示报文 | 需要编译后的程序 |
| `frame-callback-selftest.c` | 无需设备的 C 回归测试，验证 CRC 错误帧、超时、实例隔离和回调开关等 | 不需要，仅开发验证 |
| `frame-callback-cli-tests.py` | 用临时 TCP 从站和 PTY 验证命令行、读写回读及相邻 lib 加载 | 不需要，仅开发验证 |

原 `frame-callback-cli-tests.sh` 只有一条 Python 转发命令，已删除；
`make check` 通过 Automake 的 Python 测试支持直接执行 `.py`，保留全部测试覆盖。

### 编译及运行位置

在 Mac 源码目录运行以下脚本，由 Ubuntu 负责构建并回传产物：

```sh
./scripts/remote-build.sh native check
./scripts/remote-build.sh
```

RK3568 程序位于 `artifacts/rk3568/bin/frame-callback-test`，对应库位于
`artifacts/rk3568/bin/lib/`，即**可执行文件所在目录下的 lib 子目录**。
ELF 包含 `$ORIGIN/lib` 运行搜索路径，可将整个 `bin/` 复制到板上后直接运行，
无需手动设置 `LD_LIBRARY_PATH`，也不依赖启动时的工作目录。

```text
应用目录/
  frame-callback-test
  frame-callback-selftest
  lib/
    libmodbus.so -> libmodbus.so.5.1.0
    libmodbus.so.5 -> libmodbus.so.5.1.0
    libmodbus.so.5.1.0
```

```sh
# 进入复制后的 bin/ 或自己的应用目录后直接执行。
./frame-callback-test tcp --cycles 1
./frame-callback-test rtu --device /dev/ttyS1 --cycles 1
```

复制时保留 SO 符号链接，例如使用 `cp -a` 或 `rsync -a`。运行时至少需要
`libmodbus.so.5` 名称及其指向的实际 SO 文件；仅复制不带版本号的 `.so` 名称不够。
库和可执行文件必须使用同一目标架构，系统的 libc/动态加载器仍由目标系统提供。
`install/usr/lib` 保留完整 SDK 安装产物，运行程序不再依赖它。

也可将整个 `artifacts/rk3568/` 目录复制到板上，在根目录继续使用下列示例中的
`./run-frame-callback-test.sh`；兼容脚本会使用同一 `bin/lib/`。
RTU 的串口属于运行测试程序的主机，例如板上的 `/dev/ttyS1`，不是 Mac 的串口。
native 产物同理用于 Ubuntu x86_64，不能在 Mac 或 RK3568 上执行。

在 Ubuntu 构建目录也可直接使用 libtool 生成的 `tests/frame-callback-test` 包装入口。

### TCP 示例

```sh
# 默认对方 192.168.9.60、端口 502、站地址 1，每秒持续读保持寄存器 0。
./run-frame-callback-test.sh tcp

# 配置全部四类读区，并循环写线圈/保持寄存器，每个写区都立即回读比对。
./run-frame-callback-test.sh tcp --ip 192.168.9.60 --port 502 --slave 1 \
  --read-coils 0:8 --read-discrete 0:8 \
  --read-holding 0:10 --read-input 0:10 \
  --write-coils 20:1,0,1,0 --write-registers 100:123,456,65535 \
  --interval 1000 --cycles 10
```

### RTU 示例

```sh
# 默认 9600 波特率、8 数据位、1 停止位、N 无校验、站地址 1。
./run-frame-callback-test.sh rtu --device /dev/ttyS1 \
  --read-holding 0:10 --write-registers 100:123,456

# 显式设置串口参数，并配置全部四类点位。
./run-frame-callback-test.sh rtu --device /dev/ttyUSB0 \
  --baud 19200 --data-bits 8 --stop-bits 1 --parity E --slave 2 \
  --read-coils 0:8 --read-discrete 0:8 \
  --read-holding 10:4 --read-input 20:4 \
  --write-coils 30:1,0 --write-registers 100:1000,2000 \
  --interval 500 --timeout 1000 --cycles 20
```

### 点位和轮询规则

- 所有地址是从 **0 开始的协议地址**。设备手册的 40001 通常对应保持寄存器
  协议地址 0；具体换算以设备手册为准，不要直接把 40001 当作地址 0。
- 读区格式 `地址:数量`；写区格式 `地址:值,值,...`。参数可重复以配置不连续区间，
  最多合计 64 区；地址、数量和值可用十进制或 `0x` 十六进制。
- 线圈、离散输入每次最多读 2000 点，保持/输入寄存器最多读 125 点；
  每次最多写 1968 个线圈或 123 个保持寄存器，区间不能越过地址 65535。
- 线圈写值只允许 0/1；保持寄存器写值为 0..65535。输入类点位按协议只读。
- 每轮先读取全部读区，再逐区写入并立即回读比对。输出包含完整 TX/RX HEX、
  点位值、写入值、回读值及校验结果。写值每轮固定为命令行指定值。
- 不指定读区时默认读保持寄存器 `0:1`；不指定写区时不写入。写操作会持续
  改变所配置点位，请使用要测试的设备地址和值。
- `--interval` 是每轮全部操作结束后等待的毫秒数，默认 1000；
  `--timeout` 设置响应和字节超时，默认 1000 毫秒。
- `--cycles 0` 默认持续轮询，Ctrl+C/SIGTERM 退出；正数表示有限轮询。
  某区通讯或比对失败会记录错误并继续其他配置区；不启用无限链路重试或自动重连。
  信号可立即打断轮询间隔；若正处于收发中，需等待当前 libmodbus API 返回后
  再关闭连接。响应/字节超时由 `--timeout` 控制，设置较长超时会延长停止等待。
- `--frame-output 0` 关闭完整帧显示，`1` 开启，默认开启；点位读写和结果输出继续。
  本工具不启用标准 debug，避免与回调输出重复。
- 站地址范围为 1..247，默认 1，不使用不能回读的广播地址 0。
- 退出码：0 为全部完成且无错误或无错误时主动停止，1 为连接/通讯/回读校验失败，
  2 为参数错误。`--help` 查看全部参数；参数会在连接实际设备前校验。
