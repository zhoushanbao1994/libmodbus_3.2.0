# Frame Callback Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Keep edits in the existing Mac working tree as required by AGENTS.md.

**Goal:** Add independent full-ADU TX/RX callbacks to each libmodbus context.

**Architecture:** Implement the supplied design in the common send/receive paths. Each context stores one callback and user pointer; application code owns any multi-subscriber dispatcher. Keep all transport, recovery and debug behavior.

**Tech Stack:** C, existing Autotools, Mutagen and remote Ubuntu builds.

**Spec:** `libmodbus 增加多实例报文回调的完整修改方案.md`

## Global Constraints

- Edit this local working tree only; no local compilation.
- Preserve Mac → Ubuntu source synchronization and separate returned artifacts.
- Build RK3568 AArch64 with the owner's GCC 9.2 via `./scripts/remote-build.sh`.
- Run `./scripts/remote-build.sh native check` for behavioral changes.
- A successful build requires exit code 0 from the complete local build command.
- Preserve existing Autotools and warning flags.

## Review Focus

- Complete invalid or filtered RTU frames still emit RX without changing the integrity result or errno.
- Partial frames and timeouts emit no RX; fragmented complete frames emit once.
- Failed and short sends emit no TX; successful recovery emits once.
- Replacing/disabling one instance's callback leaves other instances and debug settings independent.
- Callbacks execute synchronously: document data lifetime, serialization, no reentrant I/O/free, and C++ exception boundaries.

### Task 1: Per-context callback API and common TX/RX integration

**Files:**
- Modify: `src/modbus.h`, `src/modbus-private.h`, `src/modbus.c`.
- Create: `tests/frame-callback-test.c`, `docs/modbus_set_frame_callback.md`.
- Modify: `tests/Makefile.am`, `tests/README.md`, `docs/index.md`.

**Interfaces:**
- Consumes: common `send_msg()`, `_modbus_receive_msg()`, `_modbus_init_common()` and existing transport backends.
- Produces: `MODBUS_FRAME_TX = 0`, `MODBUS_FRAME_RX = 1`, `modbus_frame_direction_t`, `modbus_frame_callback_t` and `int modbus_set_frame_callback(modbus_t *ctx, modbus_frame_callback_t callback, void *user_data)`.

- [x] Add a POSIX socket-pair regression executable to `make check`, using real RTU/TCP backends and literal frame fixtures. Test registration/null context, disabled defaults (including TCP PI), instance routing, callback replacement/removal, complete TX/RX bytes, CRC rejection/filtering, errno preservation, partial receive/timeout/disconnect, server replies and transport send faults.
- [x] Run `./scripts/remote-build.sh native check`: expect missing callback API diagnostics before implementation.
- [x] Add public types/API, context fields, setter and initialization. Run native checks: expect runtime failure because TX/RX hooks are absent.
- [x] Add an errno-preserving emitter; emit TX only on `rc == msg_length` inside the existing send loop; emit RX after a single integrity check and return that check's original result.
- [x] Document API semantics and a C usage example; add links to the reference index and test documentation.
- [x] Run `./scripts/remote-build.sh native check`: expect both original and callback suites PASS and complete local exit 0.
- [x] Run `./scripts/remote-build.sh`: expect RK3568 build and artifact return success, complete local exit 0.
- [x] Review the final diff and returned logs/status/toolchain metadata. Report build, native tests, synchronization and target-board verification separately. Leave changes uncommitted for user review.


## Verification record

- RED API: `.build-logs/20261115-225044-native-check-86131.log`, exit 2; new API/types missing as expected.
- RED hooks: `.build-logs/20261115-225138-native-check-86274.log`, exit 2; original suite PASS, callback test failed at the missing TX notification.
- GREEN native: `.build-logs/20261115-225218-native-check-86374.log`, complete local command exit 0; both suites PASS (2/2), no skips.
- RK3568: `.build-logs/20261115-225254-rk3568-build-86644.log`, complete local command exit 0; GCC 9.2.1, aarch64-none-linux-gnu, returned ELF AArch64 shared/static libraries and matching public header.
- Both build-status files report success/exit_code=0; both scripts report source flush and successful result synchronization.
- Fresh read-only review found no actionable correctness issues. Source changes remain uncommitted in the requested Mac working tree; no local compilation or remote source edits.
- Target-board hardware verification has not been performed.
