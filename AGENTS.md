# libmodbus development

- Mac is the source of truth. Edit this local working tree only.
- Source synchronization: Mutagen session `libmodbus-code`, Mac to Ubuntu.
- Do not compile locally or edit `/root/mbp_sync/libmodbus_3.2.0` remotely.
- Default target is RK3568 AArch64 with the owner's GCC 9.2 toolchain.
- Build and return artifacts: `./scripts/remote-build.sh`
- Run Ubuntu native tests: `./scripts/remote-build.sh native check`
- Always run the RK3568 build after changing C sources or build inputs.
- After behavior changes, also run the native tests. Cross-compiled executables
  cannot be executed on the Ubuntu x86_64 host; board tests are a separate step.
- Read `.build-logs/` and `artifacts/<target>/build-status.txt` for results.
- A successful build requires exit code 0 from the complete local build command.
- Report build, tests, synchronization, and target-board verification separately.
- Preserve the existing Autotools build and warning flags. Do not change the
  selected toolchain, disable tests, or change synchronization direction merely
  to make a failure disappear.
- Git metadata, `.build-logs/`, and `artifacts/` stay outside source sync.
- Artifacts are generated copies. Edit source/configuration rather than artifacts.
