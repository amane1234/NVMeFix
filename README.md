NVMeFix
=======

[![Build Status](https://github.com/acidanthera/NVMeFix/actions/workflows/main.yml/badge.svg?branch=master)](https://github.com/acidanthera/NVMeFix/actions) [![Scan Status](https://scan.coverity.com/projects/22192/badge.svg?flat=1)](https://scan.coverity.com/projects/22192)

***NOTE***: NVMeFix supports up to macOS 26, some features might not be available on newer versions as of now, like the timeout panic fix.

NVMeFix is a set of patches for the Apple NVMe storage driver, IONVMeFamily.
Its goal is to improve compatibility with non-Apple SSDs. It may be used both on Apple and non-Apple
computers.

The following features are implemented:

- Autonomous Power State Transition to reduce idle power consumption of the controller.
- Host-driver active power state management.
- Workaround for timeout panics on certain controllers (VMware, Samsung PM981).

Other incompatibilities with third-party SSDs may be addressed provided enough information is
submitted to our [bugtracker](https://github.com/acidanthera/bugtracker).

Unfortunately, some issues cannot be fixed purely by a kernel-side driver. For example, MacBookPro
11,1 EFI includes an old version of NVMHCI DXE driver that causes a hang when resuming from
hibernation with full disk encryption on.

Installation
------------

NVMeFix requires at least Lilu 1.4.1 and at least 10.14 system version. It may be compatible with
older systems, but has not been tested.

It may be installed to `/Library/Extensions` on 10.15 and earlier, or injected by the bootloader on all versions.

Configuration
-------------

`-nvmefdbg` enables detailed logging for `DEBUG` build.

`-nvmefoff` disables the kext.

`-nvmefaspm` forces ASPM L1 on all the devices. This argument is recommended exclusively for testing purposes,
as for daily usage one could inject `pci-aspm-default` device property with `<02 00 00 00>` value into the SSD devices and bridge devices they are connected to onboard.
Updated values will be visible as `pci-aspm-custom` in the affected devices.

ASPM L1.2 (L1 PM Substates) is opt-in. Many laptops support L1.2 on both the SSD and its root port, but
the firmware leaves it off. For daily use, inject an `nvme-aspm-l12` device property (any value, e.g.
`<01 00 00 00>`) into the SSD's PCI device (the same device as `ps-max-latency-us`); `-nvmefl1ss` enables
it for every SSD and is meant for a first test. NVMeFix then enables LTR on both
ends and programs the L1.2 timing the way Linux does: the larger Common_Mode_Restore_Time and T_POWER_ON
of both ports, and an LTR_L1.2_THRESHOLD of the time to enter and leave L1.2. ASPM L1 must already be on
(firmware, `pci-aspm-default`, or `-nvmefaspm`). The setting is applied again after each power transition
of the controller. The board must route the SSD's CLKREQ# signal: if it does not, the SSD may stop
answering once it enters L1.2, so keep a bootable EFI without this argument at hand. Every register is
read back, and a mismatch or a device that stops answering restores the previous configuration.
`nvmef-l1ss-status` on the controller reports the outcome, `nvmef-l12-threshold-us` and
`nvmef-l12-power-on-us` the programmed timing. The link only enters L1.2 when the SSD reports, via LTR,
that it tolerates more latency than the threshold: `nvmef-ltr` (bit 0 supported, bit 1 enabled) and
`nvmef-ltr-max-latency` (Max Snoop latency in bits 15:0, Max No-Snoop in bits 31:16, 0 if the firmware
left them unset) show what the SSD may report.

Some SSDs misbehave when APST is on. NVMeFix attempts to detect broken motherboard and SSD
combinations and work around them. Motherboard is detected via IORegistry keys injected by Clover,
or NVRAM variables provided by OpenCore.

APST table entries specify minimum idle latency for the transition to occur. Maximum acceptable
latency is 100000 microseconds, and may be overriden via little-endian 8-byte property
`ps-max-latency-us` of parent PCI device (e.g.
`IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/RP06@1C,5/IOPP/SSD0@0`). If set to 0, APST
will be disabled completely.

`ps-max-latency-us-ac` and `ps-max-latency-us-battery` properties (same format, on the same PCI device)
replace `ps-max-latency-us` while the Mac runs on AC power or on battery, for example a lower limit on
AC for responsiveness and a higher one on battery for deeper idle states. APST is reconfigured when the
power source changes. Without them NVMeFix does not watch the power source.

Diagnostics
-----------

`RELEASE` build will only log high-level information about failures.

`DEBUG` build will additionally log used power states, detailed error messages, and attempt to
fetch APST status and table from the controller.

APST enable status, as read back from the controller, is posted to the IONVMeController IORegistry entry
`apst` key, and the requested status to `apst-requested`. If they differ, or the configuration could not
be confirmed, NVMeFix retries after 1, 5 and 30 seconds, and again after the next power transition.
`reset-generation` counts the times the controller lost its power settings without a power transition.

The SSD and its upstream bridge get `nvmef-aspm-supported` and `nvmef-aspm-enabled` (bit 0 L0s, bit 1 L1)
and, when they have the L1 PM Substates capability, `nvmef-l1ss-supported` and `nvmef-l1ss-enabled`
(bit 0 PCI-PM L1.2, bit 1 PCI-PM L1.1, bit 2 ASPM L1.2, bit 3 ASPM L1.1). L1 substates are enabled only
by firmware or the PCI driver; without `nvme-aspm-l12` or `-nvmefl1ss` NVMeFix only reads them. After a
wake NVMeFix only republishes them when it had to program the link again.

If active power management initialisation is successful, an `NVMePMProxy` entry will be created
in the IOPower IORegistry plane with IOPowerManagement dictionary.

Information about power states supported by the controller may be obtained e.g. using `smartmontools`.
For example, in the following output the controller reports 5 states, where the former three
high-power states will be used by NVMeFix for active power management, and the latter two may be
used for APST depending on `ps-max-latency-us`.

    Supported Power States
    St Op     Max   Active     Idle   RL RT WL WT  Ent_Lat  Ex_Lat
     0 +     9.00W       -        -    0  0  0  0        0       0
     1 +     4.60W       -        -    1  1  1  1        0       0
     2 +     3.80W       -        -    2  2  2  2        0       0
     3 -   0.0450W       -        -    3  3  3  3     2000    2000
     4 -   0.0040W       -        -    4  4  4  4     6000    8000

IONVMeFamily supports the following debug flag bitfield, which are passed either via `nvme` bootarg
or `debug.NVMe` sysctl:

    1: Log some events via kprintf
    2: Detailed event trace via kernel_debug with 0x61500xx debugid
    4: PRP-related event trace via kernel_debug with 0x61540xx debugid
    8: Force disable LPSR for Apple controllers
    16: Perform only PCI initialisation of NVMe controller
    32: Ignore initialisation errors
    128: Disable LPSR for Apple controllers
    512: Disable Unmap feature for IONVMeBlockStorageDevice

IONVMeFamily supports the following additional bootargs:

    nand-io-timeoutms: Timeout for NVMe requests in ms, 35 s by default
    enable-IO-log: Issue CORE_DEBUG_ENABLE_IOLOG ASP command (for Apple controllers)
