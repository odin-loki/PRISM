Cross-check of the PL031 device model against QEMU (docs/DEVICE_MODEL.md section 7). Not part of the
build. `run.sh` generates register traces, runs each on QEMU `-M virt` (bare-metal AArch64 guest) and
replays the observed values against the generated model.
