#!/usr/bin/env bash
# Run on Mac. Bash 3.2 compatible.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
TARGET=${1:-rk3568}
ACTION=${2:-build}
usage() {
  echo "Usage: $0 [rk3568|native] [build|check]" >&2
  echo 'check requires native; ARM tests must run on the target board.' >&2
  exit 2
}
[ "$#" -le 2 ] || usage
case "$TARGET" in rk3568|native) ;; *) usage ;; esac
case "$ACTION" in build|check) ;; *) usage ;; esac
if [ "$ACTION" = check ] && [ "$TARGET" != native ]; then usage; fi
for tool in mutagen ssh tee; do
  command -v "$tool" >/dev/null || { echo "Missing command: $tool" >&2; exit 127; }
done
cd "$ROOT"
if ! mutagen project list >/dev/null 2>&1; then
  mutagen project start --no-global-configuration
fi
mkdir -p "$ROOT/.build-logs"
LOG="$ROOT/.build-logs/$(date +%Y%m%d-%H%M%S)-$TARGET-$ACTION-$$.log"
echo "Waiting for source synchronization..."
mutagen sync flush libmodbus-code
# tee must not hide the exit code returned by SSH.
set +e
ssh -o BatchMode=yes -o ConnectTimeout=10 root@192.168.9.60 \
  "bash /root/mbp_sync/libmodbus_3.2.0/scripts/remote-build-ubuntu.sh $TARGET $ACTION" \
  2>&1 | tee "$LOG"
PIPE_RESULTS=("${PIPESTATUS[@]}")
set -e
BUILD_STATUS=${PIPE_RESULTS[0]}
if [ "$BUILD_STATUS" -eq 0 ] && [ "${PIPE_RESULTS[1]}" -ne 0 ]; then
  BUILD_STATUS=${PIPE_RESULTS[1]}
fi
# Return logs/status even when compilation fails; never mask its failure.
RETURN_STATUS=0
mutagen sync flush libmodbus-results || RETURN_STATUS=$?
echo "Local log: $LOG"
echo "Returned results: $ROOT/artifacts/$TARGET"
if [ "$BUILD_STATUS" -ne 0 ]; then
  echo "Remote build failed (exit $BUILD_STATUS)." >&2
  exit "$BUILD_STATUS"
fi
if [ "$RETURN_STATUS" -ne 0 ]; then
  echo 'Build completed, but result synchronization failed.' >&2
  exit "$RETURN_STATUS"
fi
echo "BUILD AND RESULT SYNC SUCCESS: $TARGET ($ACTION)"
