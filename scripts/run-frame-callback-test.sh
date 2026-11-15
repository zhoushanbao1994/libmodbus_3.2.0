#!/bin/sh
# 构建流程会把本脚本复制到 artifacts/<target>/ 根目录。
# 相对定位可执行文件及其相邻 lib/，兼容原有从产物根目录启动的方式。
# ELF 自身已包含 ORIGIN/lib 搜索路径，也可以直接运行 bin/frame-callback-test。
PACKAGE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -x "$PACKAGE_ROOT/bin/frame-callback-test" ]; then
    echo '请从 artifacts/<target>/run-frame-callback-test.sh 运行回传的设备测试程序。' >&2
    exit 1
fi
LD_LIBRARY_PATH="$PACKAGE_ROOT/bin/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
exec "$PACKAGE_ROOT/bin/frame-callback-test" "$@"
