NVMeFix Changelog
=================
#### v1.1.4
- Validate the private IONVMeFamily layout: disassembled member offsets must use a positive displacement, stay aligned and distinct, and fit the loaded `AppleNVMeRequest` and `IONVMeController` classes; requests must be `AppleNVMeRequest` objects. An unexpected layout disables NVMe commands (or only the ANS2 workaround) instead of writing to the wrong field
- Locate `activityTickle` in IOService's vtable instead of a fixed slot and only route it when IONVMeController's slot holds the expected function; without the hook only active power management is disabled
- Finish a `setPowerState` that finds the controller busy on a worker and acknowledge it afterwards, instead of reporting a power state that was never set; replay activity tickles that found the controller busy
- Read APST back in release builds and follow the reported state; retry a failed Set Features once at once, then after 1, 5 and 30 s
- Track a reset generation: a controller power transition or a power state changed without the host (controller reset) reapplies the power policy and drops stale deferred work (`reset-generation`)
- Use a 100 ms minimum APST idle time, so non-operational states that report no entry/exit latency neither thrash nor go unused
- Skip power management cleanly when the PM root domain is unavailable
- Restore both link partners when `-nvmefaspm` cannot enable ASPM L1 on both ends
- Added `ps-max-latency-us-ac` and `ps-max-latency-us-battery` properties to use a different APST latency limit on AC power and on battery
- Report ASPM and PCIe L1 PM Substates support and state of the SSD and its bridge (`nvmef-aspm-*`, `nvmef-l1ss-*`), and LTR state and maximum latencies (`nvmef-ltr*`)
- Added opt-in ASPM L1.2 (`nvme-aspm-l12` SSD property, or `-nvmefl1ss` for all SSDs) with Linux' L1.2 timing and LTR, read back with rollback, and reapplied after power transitions without republishing an unchanged link
- Added APST quirks from Linux: Kingston SKC2000, ZHITAI TiPro5000/TiPro7000, Solidigm P44 Pro and Samsung Portable SSD X5 (no deepest power state), Kioxia CD6-V KCD6XVUL6T40 (no APST)

#### v1.1.3
- Added constants for macOS 26 support

#### v1.1.2
- Added constants for macOS 15 support
  
#### v1.1.1
- Added constants for macOS 14 support
- Fixed macOS 14 compatibility

#### v1.1.0
- Added constants for macOS 13 support

#### v1.0.9
- Added constants for macOS 12 support
- Fixed macOS 12 compatibility

#### v1.0.8
- Fixed applying quirks based on the disk name and serial
- Make Kingston A2000 quirk specific to S5Z42105

#### v1.0.7
- Fixed symbol solving on macOS 11.3
- Added `-nvmefaspm` boot argument to force ASPM L1 on all NVMe SSDs

#### v1.0.6
- Added APST workaround for Kingston A2000

#### v1.0.5
- Fixed quirks enabling per controller
- Fixed initialisation on 10.15+

#### v1.0.4
- Added MacKernelSDK with Xcode 12 compatibility

#### v1.0.3
- Fix re-enabling APST after sleep (1.0.2 regression)
- Added constants for 11.0 support (no full compatibility provided)

#### v1.0.2
- Prevent timeout panic on certain controllers (VMware, Samsung PM981)
- Only enable active NVMe power management for controllers that do not support APST

#### v1.0.1
- Add OpenCore support for quirk autodetection

#### v1.0.0
- Initial release
