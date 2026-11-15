# Mac 编辑、Ubuntu 编译

本机源码：`/Users/zhoushanbao/Code/libmodbus_3.2.0`

Ubuntu：`root@192.168.9.60`（现有 SSH 密钥连接）

## 日常使用

在工程目录执行：

```sh
# 默认 RK3568 AArch64 构建，等待源码同步、远端编译并等待结果回传
./scripts/remote-build.sh

# Ubuntu x86_64 原生编译和 make check
./scripts/remote-build.sh native check

# 同步状态
mutagen project list

# 暂停 / 恢复本工程的两个同步会话
mutagen project pause
mutagen project resume

# 停止本工程的同步（保留两端文件）
mutagen project terminate

# 重新启动，使用工程配置且忽略用户全局默认同步配置
mutagen project start --no-global-configuration
```

`remote-build.sh` 会在工程尚未启动时自动启动 Mutagen 项目。已暂停时请先恢复；主机离线时同步会重试，需取消等待可按 Ctrl+C。修改 `mutagen.yml` 后需 terminate 再 start，已存在会话不会自动采用新配置。不要在有构建进行时重建会话。

## 目录与同步方向

| 用途 | 目录 | 方向 |
| --- | --- | --- |
| 主源码 | 本机工程目录 | 本机 → Ubuntu |
| 源码镜像 | `/root/mbp_sync/libmodbus_3.2.0` | 仅接收本机源码 |
| 工作目录 | `/root/mbp_build/libmodbus_3.2.0/<target>` | 只在 Ubuntu 使用 |
| 结果目录 | `/root/mbp_results/libmodbus_3.2.0/<target>` | Ubuntu → 本机 |
| 返回结果 | 本机 `artifacts/<target>` | 只接收远端生成文件 |
| 实时构建日志 | 本机 `.build-logs/` | SSH 实时输出并保存 |

两条 Mutagen 会话均使用 `one-way-replica`。源码以本机为准，远端源码副本内的额外文件会被移除或覆盖；结果以 Ubuntu 为准，本机 artifacts 的手工修改也会被覆盖。Git、artifacts、日志及 Mutagen 锁文件不进入源码镜像。

远端生成的 configure、Makefile 和对象文件保存在独立工作目录，不写入同步源码。构建保留缓存；修改 Autotools 输入或目标配置会重新生成配置。同一目标已有构建时新命令以 75 退出，避免共享缓存被并发写入。

## 产物

```text
artifacts/rk3568/
  build-status.txt              # 本次状态、退出码、开始和结束时间
  toolchain.txt                 # 编译器版本、目标架构、库文件类型
  bin/frame-callback-test       # TCP/RTU 设备测试 ELF，可直接运行
  bin/frame-callback-selftest   # 无硬件自动回归 ELF
  bin/lib/                     # ELF 所在目录的 lib 子目录，包含 libmodbus.so*
  run-frame-callback-test.sh    # 兼容启动入口，使用 bin/lib
  install/usr/include/modbus/   # 对外头文件
  install/usr/lib/              # libmodbus.so*、libmodbus.a、pkgconfig/
  logs/config.log               # configure 详细日志
```

`native check` 的测试日志位于 `artifacts/native/logs/tests/`。

设备运行可只复制 `bin/` 的全部内容。两个 Linux ELF 均设置 `$ORIGIN/lib`
运行搜索路径，会从自身目录下的 `lib/` 查找 SO，而非当前工作目录下的 `lib/`。
保留 SO 实体和 SONAME 符号链接；`install/usr/lib` 仍作为完整 SDK 安装产物回传。

构建失败时会回传失败状态和可用诊断日志，完整输出保存在 `.build-logs/`；此前成功产物可能仍然保留，使用产物前先确认 `build-status.txt` 的本次时间和 `status=success`。若在连接或同步阶段失败，本地命令退出码和本地日志为准，不能将旧状态视为本次成功。

只执行暂存安装 `make DESTDIR=... install`，不会将交叉编译库安装进 Ubuntu 系统 `/usr`。返回的 Linux ELF 库用于 Linux/目标设备，不能在 macOS 加载。

## 工具链和验证范围

默认配置在 `config/remote-build/rk3568.env`：

```text
/opt/rk3568/gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
```

`native.env` 使用 Ubuntu 自带 GCC，仅用于主机测试。没有选择其他已安装的工具链。

默认遵循指定工具链的 sysroot 和 ABI。编译通过与 ELF 架构检查不能替代 RK3568 设备上的运行验证；需要结合实际固件检查动态加载器和 glibc 兼容性。

本机安装 Mutagen；Ubuntu 需要自动部署的同步代理以及 GCC、Make、Autoconf、Automake、Libtool、rsync、flock。
执行 `native check` 还需要 Python 3 标准库，用于 TCP/RTU 命令行自动测试；目标板运行设备测试程序不需要 Python。
Mutagen 守护进程与已保存会话负责后台同步；登录自启通过 `mutagen daemon register` 管理。暂停的会话仍保持暂停状态。

参考：[Mutagen 单向同步模式](https://mutagen.io/documentation/synchronization/)、[工程会话管理](https://mutagen.io/documentation/orchestration/projects/)。
