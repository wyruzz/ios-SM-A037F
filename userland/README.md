# A03s user space

This directory is the A03s side of the HakiOS user space: the injected
libraries, the init, the launchd job list and the small tests. The full system
image (`system.apfs`) is not here; it is built from your own iOS 16.5 IPSW by
the HakiOS userland tooling.

## What matters for the A03s port

- `src/inject.c` — the library injected into `backboardd`,
  `containermanagerd`, `containermanagerd_system`, `lsd`, `cfprefsd`,
  `runningboardd` (and, in some builds, `iconservicesagent`, `profiled`,
  `datamigrator`). It contains:
  - the `backboardd` display/framebuffer hooks and the software-renderer
    workaround,
  - the `runningboardd`/RBS assertion workarounds,
  - the `lsd` persona lookup patch (`InstalledContentLibrary+0xd070` → nil),
  - the backboardd shell-watchdog setter patch (`backboardd+0x1fb68` → ret),
  - the input thread and the touch probe.
- `src/inject_sb.c` — the SpringBoard library: display info
  (`1125x2436@3`), `ControlCenterServices` module block, `RBSAssertion`
  answers, setup/activation answers, battery/cellular status-bar stand-ins,
  the LaunchServices seeding/branching patches, and the stack sampler.
- `src/url_fix.c` — `NSURL`/`NSFileManager` nil-path fixes for
  `iconservicesagent`, `profiled` and `lsd`.
- `src/hold_exit.c` — keeps `datamigrator` alive long enough for launchd's
  activation reply.
- `etc/inject.conf` — the `exec_inject` list (`library program ...`).
- `etc/rc`, `etc/rc.jobs` — the init script and the `com.hakios.demo` job.
- `etc/var.skel` — the directories init creates in the memory data volume.

## Build

The A03s userland is built with the HakiOS `userland/build.sh` flow, e.g.:

```sh
INJECT="
/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system lsd cfprefsd runningboardd
/redwood/lib/libhakios.sb.dylib SpringBoard
/redwood/lib/libhakios.media.dylib mediaserverd
/redwood/lib/libhakios.app.dylib Sileo
/redwood/lib/libhakios.tweaks.dylib Preferences
/redwood/lib/libhakios.url.dylib iconservicesagent profiled lsd
/redwood/lib/libhakios.hold.dylib datamigrator /usr/libexec/datamigrator /System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator" \
CRYPTEXES=flash FLASHBOOT=1 REST=1 ./build.sh
```

The output is `flashboot.apfs` (4 MiB boot volume) and `root.apfs` (4 MiB RAM
image). The loader takes them as `md0` / `md1` with `rdsplit=0x400000`.

`mkjobs.py` generates the ~414 launchd job plists from your iOS root volume;
those Apple plists are not shipped here.

## Notes

- Every injected library is built with `-framework Security` on this tree; the
  kernel needs `SecTaskCopyValueForEntitlement` in `libhakios.dylib`.
- The `umask` of the build shell matters: launchd refuses a plist whose group
  may write (`bad ownership/permissions`). `build.sh` sets `umask 022`.
- `libhakios.um.dylib` and the `u2f_test`/`store_test` files belong to other
  HakiOS targets; they are not required by the A03s boot.
