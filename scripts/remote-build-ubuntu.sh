#!/usr/bin/env bash
# Ubuntu build driver. Generated files never enter the Mutagen source mirror.
set -euo pipefail
TARGET=${1:-rk3568}
ACTION=${2:-build}
[ "$#" -le 2 ] || exit 2
case "$TARGET" in rk3568|native) ;; *) exit 2 ;; esac
case "$ACTION" in build|check) ;; *) exit 2 ;; esac
if [ "$ACTION" = check ] && [ "$TARGET" != native ]; then
  echo 'Cross-compiled tests require a target board.' >&2
  exit 2
fi
SYNC=/root/mbp_sync/libmodbus_3.2.0
WORK=/root/mbp_build/libmodbus_3.2.0/$TARGET
RESULT=/root/mbp_results/libmodbus_3.2.0/$TARGET
mkdir -p "$WORK" "$RESULT"
# Serializes same-target builds, including configure/install and test execution.
exec 9>"$WORK/build.lock"
flock -n 9 || { echo "Another $TARGET build is running." >&2; exit 75; }
STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
printf 'status=building\ntarget=%s\naction=%s\nstarted=%s\n' "$TARGET" "$ACTION" "$STARTED" > "$RESULT/build-status.txt"
finish() {
  local status=$?
  trap - EXIT
  # Logs from failed configure/tests are also made available on the Mac.
  for path in config.log tests/*.log; do
    if [ -f "$WORK/build/$path" ]; then
      mkdir -p "$RESULT/logs/$(dirname "$path")"
      cp "$WORK/build/$path" "$RESULT/logs/$path" || true
    fi
  done
  {
    if [ "$status" -eq 0 ]; then echo 'status=success'; else echo 'status=failed'; fi
    printf 'target=%s\naction=%s\nexit_code=%s\nstarted=%s\nfinished=%s\n' \
      "$TARGET" "$ACTION" "$status" "$STARTED" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "$RESULT/build-status.txt"
  exit "$status"
}
trap finish EXIT
for tool in rsync autoreconf libtoolize make flock sha256sum file; do
  command -v "$tool" >/dev/null || { echo "Missing Ubuntu dependency: $tool" >&2; exit 127; }
done
mkdir -p "$WORK/source" "$WORK/build" "$WORK/stage"
# Only this dedicated, reproducible source snapshot is mirrored with deletion.
rsync -a --delete --exclude='/.git/' --exclude='/artifacts/' \
  --exclude='/.build-logs/' --exclude='/mutagen.yml.lock' \
  --exclude='/configure' --exclude='/config.h.in' --exclude='/aclocal.m4' --exclude='/autom4te.cache/' \
  --exclude='/build-aux/' --exclude='Makefile.in' --exclude='/INSTALL' \
  --exclude='/m4/libtool.m4' --exclude='/m4/lt*.m4' "$SYNC/" "$WORK/source/"
cd "$WORK/source"
source "config/remote-build/$TARGET.env"
export CC CXX AR RANLIB STRIP CFLAGS
for tool in "$CC" "$CXX" "$AR" "$RANLIB" "$STRIP"; do
  command -v "$tool" >/dev/null || { echo "Missing toolchain executable: $tool" >&2; exit 127; }
done
if [ -n "$HOST" ] && [ "$("$CC" -dumpmachine)" != "$HOST" ]; then
  echo 'Compiler target differs from configured HOST.' >&2
  exit 2
fi
# Generated Autotools files are retained only in this build-owned snapshot.
INPUT_HASH=$(find . -type f \( -name configure.ac -o -name Makefile.am -o -name autogen.sh -o -name '*.m4' \) -not -path './autom4te.cache/*' -not -name 'libtool.m4' -not -name 'lt*.m4' -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -d ' ' -f 1)
if [ ! -f configure ] || [ ! -f config.h.in ] || [ ! -f "$WORK/bootstrap.sha256" ] || [ "$(cat "$WORK/bootstrap.sha256")" != "$INPUT_HASH" ]; then
  ./autogen.sh
  printf '%s\n' "$INPUT_HASH" > "$WORK/bootstrap.sha256"
fi
cd "$WORK/build"
CONFIG_KEY=$( { cat "$WORK/source/config/remote-build/$TARGET.env"; printf '%s\n' "$INPUT_HASH"; "$CC" --version; } | sha256sum | cut -d ' ' -f 1)
ARGS=(--prefix=/usr --enable-static)
if [ -n "$HOST" ]; then ARGS+=("--host=$HOST"); fi
if [ ! -f Makefile ] || [ ! -f "$WORK/configuration.sha256" ] || [ "$(cat "$WORK/configuration.sha256")" != "$CONFIG_KEY" ]; then
  "$WORK/source/configure" "${ARGS[@]}"
  printf '%s\n' "$CONFIG_KEY" > "$WORK/configuration.sha256"
fi
make -j"${JOBS:-$(nproc)}"
if [ "$ACTION" = check ]; then make check; fi
# Stage installation in a private directory, never in Ubuntu's /usr.
rm -rf -- "$WORK/stage"
mkdir -p "$WORK/stage"
make DESTDIR="$WORK/stage" install
mkdir -p "$RESULT/install"
rsync -a --delete "$WORK/stage/" "$RESULT/install/"
# noinst 测试程序不会由 make install 安装，显式回传真正的 ELF 而非 libtool
# 包装脚本；两个 ELF 使用自身目录下的 lib/，可整体搬移 bin/ 后直接运行。
mkdir -p "$RESULT/bin"
for program in frame-callback-test frame-callback-selftest; do
  executable="$WORK/build/tests/.libs/$program"
  if [ ! -f "$executable" ]; then executable="$WORK/build/tests/$program"; fi
  install -m 755 "$executable" "$RESULT/bin/$program"
done
# 运行库与 ELF 同处 bin 目录树，保留 SO 实体及 SONAME 相对符号链接。
# install/usr/lib 继续作为 SDK 安装产物，设备部署只需复制 bin/ 的内容。
mkdir -p "$RESULT/bin/lib"
rsync -a --delete --include='libmodbus.so*' --exclude='*' \
  "$WORK/stage/usr/lib/" "$RESULT/bin/lib/"
install -m 755 "$WORK/source/scripts/run-frame-callback-test.sh" "$RESULT/run-frame-callback-test.sh"
{
  printf 'target=%s\ncompiler=' "$TARGET"
  "$CC" --version | head -1
  printf 'machine='
  "$CC" -dumpmachine
  file -L "$RESULT/install/usr/lib/libmodbus.so"
} > "$RESULT/toolchain.txt"
echo "REMOTE BUILD SUCCESS: $TARGET ($ACTION)"
