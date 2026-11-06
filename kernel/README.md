# A03s XNU changes (description only)

To keep this repository free of Apple's kernel source, the modified XNU files
are **not redistributed here**. This file describes exactly what the A03s port
changes and where, so you can reproduce it on your own machine from Apple's
official XNU source plus the HakiOS patch.

Base: Apple **XNU `8796.121.2`** (the kernel of iOS 16.5), with the HakiOS
`xnu/hakios.patch` and `xnu/files/` applied first. The A03s changes are a
derivative work of XNU and are under the **Apple Public Source License 2.0**
(`../LICENSE.APSL`); keep Apple's notices in every file you modify.

## The files the A03s port changes

Apply the changes below to the corresponding files in your XNU tree.

### Memory

- `osfmk/arm64/arm_vm_init.c` — the single DRAM window. The loader hands XNU
  `physBase=0xD0000000`, `memSize=0x6FFFC000` (1.75 GB), one clean contiguous
  range from the phone's `/proc/iomem`. The port adjusts `gPhysBase`,
  `gPhysSize`, `avail_start`/`avail_end`, the physical map and the bootstrap
  table size checks for that window. No multi-range support yet.
- `osfmk/arm64/machine_routines.c`, `osfmk/arm64/sleh.c`,
  `osfmk/arm64/gic_v3_irq.c` — platform/IRQ glue for the MT6765.
- `osfmk/vm/bsd_vm.c`, `osfmk/vm/vm_fault.c`, `osfmk/vm/vm_pageout.c`,
  `bsd/vm/vnode_pager.c`, `bsd/miscfs/apfs/apfs_lib.c`,
  `bsd/miscfs/apfs/apfs_vnops.c`, `bsd/miscfs/bindfs/bind_vnops.c` — the
  APFS-on-`userdata` path, the memory data volume and the paging fixes the iOS
  user space needs.

### Launch path / code signing

- `bsd/kern/kern_exec.c` — `exec_inject`: the kernel inserts our library into a
  named program at exec. The buffer is 4096 bytes and matching is by basename,
  because launchd passes full paths.
- `bsd/kern/kern_cs.c` — `cs_invalid_page()` keeps `CS_VALID` when
  `cs_process_enforcement_enable` is off, so a page outside the code signature
  does not make every process `NOT_CODESIGNED`.
- `bsd/kern/kern_proc.c` — fake `CS_OPS_STATUS`/entitlement answers for
  SpringBoard (there is no AMFI/Secure Enclave on this port).
- `bsd/kern/kern_persona.c` — default personas at boot (`mobile`,
  `mobile-system`, `mobile-proxy`) for `usermanagerd`, `lsd` and
  `runningboardd`.
- `bsd/kern/kern_minimac.c`, `bsd/kern/kern_exit.c`,
  `bsd/kern/kern_synch.c`, `bsd/kern/bsd_init.c`, `bsd/kern/mach_loader.c`,
  `osfmk/kern/startup.c` — the minimal MACF, exit and bootstrap behaviour.

### Display / input

- `iokit/Kernel/MiniIOMobileFramebuffer.cpp` — the
  `IOMobileFramebuffer` user client, the Apple-logo splash, the framebuffer
  flush and the alpha fix (all colors must be `0xFF…` on this panel).
- `bsd/dev/mediatek_display.c`, `bsd/dev/mediatek_display.h` — MT6765
  display/DSI setup.
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

## How to get the exact diff

The development tree is a full XNU checkout with these changes applied. If you
have access to it, a diff against the HakiOS base produces the patch; it is not
published here. If you are reproducing the port from scratch, the list above is
the complete set of files and the intent of each change.
