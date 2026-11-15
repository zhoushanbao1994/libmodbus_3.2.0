# 可执行文件相邻 lib 运行目录

目标：RK3568 和 native 测试 ELF 从自身目录下的 `lib/` 加载 libmodbus，
直接执行也可使用，不依赖工作目录、安装路径或用户设置的 LD_LIBRARY_PATH。

部署布局保持现有回传位置：`bin/frame-callback-test`、
`bin/frame-callback-selftest`、`bin/lib/libmodbus.so*`。
用户可整体复制 `bin/`，也可将其内容放入自己的应用目录。
现有根目录启动脚本继续兼容，改用 `bin/lib`。

- [x] 增加搬移 ELF 与 SO 至含空格临时目录、清空加载环境、从其他目录执行的测试，
      检查实际加载的 SO 路径并完成一次真实回环 TCP 操作；先验证失败。
- [x] Linux 测试 ELF 通过 Autotools 设置 `$ORIGIN/lib` 运行搜索路径。
- [x] 远程构建脚本回传运行 SO 及其 SONAME 链接至 bin/lib，保持 SDK 安装产物。
- [x] 启动脚本和中文部署文档改用相邻 lib，避免把 SO 路径误解为当前工作目录。
- [x] native 完整测试与 RK3568 构建均退出 0；检查 ELF 动态段和回传状态。
- [x] 使用回传 native 包再次验证搬移后直接启动；保留目标板未实测的说明。

遵循 AGENTS.md：只编辑 Mac 源码，通过指定远程脚本构建测试；不改变工具链、
warning 标志或同步方向，不直接修改生成产物。


验证记录：
- RED：20261116-000431-native-check-99954.log，完整命令退出 2；搬移后 libmodbus.so.5 not found。
- native GREEN：20261116-000543-native-check-213.log，完整命令退出 0；3/3 测试组通过，CLI 8 项通过且无跳过。
- RK3568：20261116-000625-rk3568-build-367.log，完整命令退出 0；运行库、SONAME 链接及 ELF 回传成功。
- 直接以回传 native/bin/frame-callback-test 为入口再次执行全部 8 项 CLI 测试，退出 0；包含清空环境并搬移运行测试。
- 独立只读审查无实质问题；目标板仍未实测。
