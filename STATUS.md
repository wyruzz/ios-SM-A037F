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

## The remaining SpringBoard respawn

SpringBoard starts and runs, but exits with `exit(0)` and launchd starts it
again. The cycle is `sb_delay=8` (the injected library sleeps 8 s in its
constructor) plus ~3.5 s of work, so ~11.5 s per cycle. Things already ruled out:

- **Jetsam**: with 1.75 GB there are no jetsam kills; `memory: free` stays
  around 20-30 MB free but `available` is ~1.2 GB, and the kill thresholds are
  170/85 MB.
- **Backboardd shell watchdog**: the setter function at `backboardd+0x1fb68`
  is patched to `ret`, and `[SystemShell] START WATCHDOG` no longer appears.
- **LaunchServices seeding wait**: the branch at `CoreServices+0x257fc` is
  patched to skip the wait, and both synchronous server-store wrappers
  (`CoreServices+0xdcf24` and `+0xdcf3c`) return `nil`.

The last stack samples show the main thread in `QuartzCore`/`mach_msg` and then
a clean `exit(0)`. The next step is to capture a sample at ~9-10 s (just before
the exit) and to enable `os_log` for SpringBoard, `backboardd` and `lsd` in the
launchd jobs, to see the scene/`UIApplicationMain` path that decides to exit.

## Not done

- Touchscreen (the A03s controller has not been identified yet).
- GPU/Metal (CoreAnimation runs in software; `Simulating crash!` from QuartzCore
  is expected).
- Cellular, Wi-Fi, audio, camera, sensors.
- Multi-range DRAM.
