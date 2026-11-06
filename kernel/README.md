# A03s XNU changes

These are the XNU source files the A03s port modified or added, relative to the
HakiOS XNU tree (`xnu-8796.121.2` + HakiOS `xnu/hakios.patch` + `xnu/files/`).
They are licensed under the **Apple Public Source License 2.0**
(`../LICENSE.APSL`), as a derivative work of Apple's XNU.

Apply them over a tree prepared as described in `../docs/building.md`:

```sh
cp -a files/. /path/to/xnu/
```

## What is here and why

### Memory

- `osfmk/arm64/arm_vm_init.c` — the single DRAM window (`gPhysBase`,
  `gPhysSize`, `avail_start/avail_end`) and the physical map. The A03s loader
  hands XNU `physBase=0xD0000000`, `memSize=0x6FFFC000` (1.75 GB), one clean
  contiguous range from the phone's `/proc/iomem`.
- `osfmk/arm64/machine_routines.c`, `osfmk/arm64/sleh.c`,
  `osfmk/arm64/gic_v3_irq.c` — platform/IRQ glue.
- `osfmk/vm/*`, `bsd/vm/vnode_pager.c`, `bsd/miscfs/apfs/*`,
  `bsd/miscfs/bindfs/*` — APFS-on-`userdata`, the memory data volume and the
  paging fixes needed by the iOS user space.

### Launch path / code signing

- `bsd/kern/kern_exec.c` — `exec_inject` (the kernel inserts our library into a
  named program at exec; the buffer is 4096 bytes and matching is by basename,
  because launchd passes full paths).
- `bsd/kern/kern_cs.c` — `cs_invalid_page()` keeps `CS_VALID` when
  `cs_process_enforcement_enable` is off, so a page that is not in the code
  signature does not make every process `NOT_CODESIGNED`.
- `bsd/kern/kern_proc.c` — the fake `CS_OPS_STATUS`/entitlement answers for
  SpringBoard (there is no AMFI/Secure Enclave on this port).
- `bsd/kern/kern_persona.c` — the default personas created at boot
  (`mobile`, `mobile-system`, `mobile-proxy`) that `usermanagerd`, `lsd` and
  `runningboardd` expect.
- `bsd/kern/kern_minimac.c`, `bsd/kern/kern_exit.c`,
  `bsd/kern/kern_synch.c`, `bsd/kern/bsd_init.c`, `bsd/kern/mach_loader.c`,
  `bsd/kern/startup.c` — the minimal MACF, exit and bootstrap behaviour of the
  port.

### Display / input

- `iokit/Kernel/MiniIOMobileFramebuffer.cpp` — the `IOMobileFramebuffer` user
  client, the Apple-logo splash and the framebuffer flush. All colors must have
  alpha `0xFF` on this panel.
- `bsd/dev/mediatek_display.c`, `bsd/dev/mediatek_display.h` — the MT6765
  display/DSI setup used by the loader and the kernel.
- `iokit/Kernel/IOService.cpp`, `iokit/bsddev/IOKitBSDInit.cpp` — IOKit
  bootstrap for the port.

### Log

- `osfmk/console/video_console.c`, `osfmk/kern/printf.c`, `bsd/sys/msgbuf.h`,
  `bsd/conf/files` — the kernel console and the shared log window.
- `osfmk/arm64/loader_return.c` — `loader_return_after=...` keeps the kernel
  (and its log) alive instead of returning to the loader.

### Other

- `bsd/sys/cdefs.h`, `bsd/kern/mach_loader.c`, `bsd/kern/kern_exit.c` — build
  and exit-path fixes required by the iOS 16.5 user space.

## Not a complete tree

This is a set of modified files, not a full XNU checkout. Get the base XNU at
tag `xnu-8796.121.2`, apply the HakiOS patch/files, then overlay this directory.
