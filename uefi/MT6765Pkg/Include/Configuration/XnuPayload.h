/** @file
  Location of the XNU payload that the firmware hands to the HackiOS loader.

  The loader is a separate UEFI application and cannot see the firmware's
  build-time symbols, so the address is duplicated there as PAYLOAD_BASE in
  xnuloader/loader.c.  Keep the two in sync.

  Copyright (c) 2026, the A03s UEFI project. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef XNU_PAYLOAD_H_
#define XNU_PAYLOAD_H_

//
// Low DRAM bank, between the UEFI image at 0x50000000 and the display at
// 0x7C250000.  Mapped as reserved in DeviceMemoryMap.h; BootShim copies the
// XNUP container here from the boot image and the loader probes it for the
// "XNUP" magic.
//
// The container is at most 32 MB (it sits below the decompression buffer):
//   0x60000000..0x62000000  XNUP container (`kernel.lz4`)
//   0x62000000..0x68000000  decompressed kernelcache
//
#define XNU_PAYLOAD_BASE      0x60000000ULL
#define XNU_PAYLOAD_MAX_SIZE  0x08000000ULL

#endif
