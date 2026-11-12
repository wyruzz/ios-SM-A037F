# A03s bring-up status

## What was achieved

- **Boot chain**: Android `boot.img` v2 header → `BootShim` → EDK2 UEFI
  (`MT6765Pkg`) → `xnuloader` → XNU `8796.121.2`.
- **Display**: the loader brings up the 720x1600 panel at `0x7C250000`; XNU's
  `MiniIOMobileFramebuffer` presents frames and the Apple logo appears.
- **Persistent log**: the loader writes its live log to LBA `1216720`
  (24 MiB into the 32 MiB boot partition), then XNU keeps appending to it
  through eMMC MSDC0. `scripts/pull_log.sh` extracts the last 8 MiB.
- **iOS 16.5 user space**: `launchd` starts, then `notifyd`, `logd`,
  `cfprefsd`, `runningboardd`, `backboardd`, `lsd`, `SpringBoard`, and the
  rest of the ~400 launchd jobs. `securityd`, `usermanagerd`, `installd`,
  `iconservicesagent` and friends come up too.
- **Touchscreen**: the Chipone ICNL9911C on the A03s, driven by bit-banged SPI
  on GPIO25-28 (`bsd/dev/mediatek_input.c`); its firmware
  (`xl_truly_firmware_nl9911c.bin`, taken from the phone's own vendor
  partition) is written into the controller's SRAM at boot. Touch and the
  on-screen Power / Volume Up / Volume Down buttons reach iOS as HID events.
- **Memory**: `phys window d0000000 size 6fffc000 (forced)` →
  `memory 1791 MB, page size 16384, 1 cpu`. No `pmap_assert_free`, no
  `compressed PTE ... corrupted`, no jetsam kills.

## Memory map that made it possible

From the phone's own `/proc/iomem` (TWRP, Android kernel):

```
40000000-47c7ffff : System RAM      (124 MB)
47dd0000-47ffffff : System RAM
48400000-7abfffff : System RAM      (~840 MB)
7b000000-7c24ffff : System RAM
7fa41000-7fafffff : System RAM
7fbc0000-7fbfffff : System RAM
80000000-8effffff : System RAM      (240 MB)
8f300000-9faeffff : System RAM
a0000000-bdffffff : System RAM      (480 MB)
be440000-beffffff : System RAM
bf300000-bff2ffff : System RAM
bfff0000-bfffefff : System RAM
c0000000-cfffafff : System RAM      (256 MB)
d0000000-13fffefff : System RAM      (~1.75 GB)  <-- our window
```

`0xD0000000..0x13FFFEFFF` is one clean contiguous block; the other regions are
split by TEE/modem/SCP/secure carve-outs. We use `win_size=0x6FFFC000`
(1.75 GB, page-aligned, ending below the last 4 KB hole).

The first attempts used `0x80000000..0xC0000000` (1024 MB) and crashed in
`compressed PTE ... corrupted` because that range contains firmware holes
(`0x8F000000-0x8F2FFFFF`, `0x9FAF0000-0x9FFFFFFF`, ...). The next attempt used
`0xD0000000..0xFE000000` (736 MB) and worked. Widening it to
`0xD0000000..0x140000000` (1.75 GB) also works because the range is contiguous
in `/proc/iomem`.

To go above 1.75 GB, XNU's pmap must learn multiple disjoint DRAM ranges
(`gPhysBase`/`gPhysSize` are single-range today). The remaining clean chunks are
~480 MB at `0xA0000000`, ~256 MB at `0xC0000000`, and ~544 MB above 4 GB; a
multi-range pmap is a substantial change, not a boot-arg.

## How the loader gives the window to XNU

`xnuloader` reads `win_base=` and `win_size=` from the payload command line
before it chooses a window, checks that the range is usable, places the kernel
and the two ramdisks there, and fills `boot_args.physBase` / `memSize` /
`topOfKernelData` accordingly:

```
win_base=0xd0000000 win_size=0x6fffc000
```

The kernel then sees `physBase=0xD0000000`, `memSize=0x6FFFC000`, and
`topOfKernelData` just above the loaded payload.

## The remaining respring (Settings / Siri)

The lock screen, SpringBoard and touch work. Opening Settings or Siri still
brings the shell down: the APFS page-in path returns `EIO` for a handful of
files (`powerd`, `dyld_shared_cache_arm64.1`, `langid.inv`, `mediaremoted`,
`useractivityd`, `healthd`, `remindd`, `searchd`, `MobileSlideShow`,
`contextstored`), so the daemon cannot start and the shell is torn down.

Things already ruled out:

- **Jetsam**: with 1.75 GB there are no jetsam kills; `available` stays around
  1.2 GB and the kill thresholds are 170/85 MB.
- **Fake entitlements**: the kernel now answers `CS_OPS_STATUS` and the
  entitlement queries with real XML/DER blobs, so processes are no longer
  marked `NOT_CODESIGNED` and SpringBoard no longer respawns for that reason.
- **Backboardd shell watchdog** and the **LaunchServices seeding wait** are
  patched out (`backboardd+0x1fb68` → `ret`, `CoreServices+0x257fc`).

The next step is the APFS read path itself: find which node/extent returns
`EIO` for those files and fix the reader (a `apfs_lenient=1` mode that skips a
bad page is the fallback, not the fix).

## Not done

- APFS page-in `EIO` for the daemons above (the cause of the Settings/Siri
  respring).
- GPU/Metal (CoreAnimation runs in software; `Simulating crash!` from QuartzCore
  is expected).
- SMP: the loader can list the second (little) cluster, but XNU still reports
  `hw.ncpu 1`; the secondary cores are not brought up yet.
- CPU frequency scaling: the MT6765 top OPP is not enabled. The loader now
  measures the real core clock with the PMU against the 13 MHz generic timer
  (the earlier `300 MHz` line came from reading the wrong register block).
- Cellular, Wi-Fi, audio, camera, sensors.
- Multi-range DRAM.
