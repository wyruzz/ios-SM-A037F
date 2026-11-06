/** @file
  XNU payload embedded in the firmware.

  The image is an "XNUP" container as consumed by the HackiOS loader:
  a header, named entries (kernel, ramdisk, cmdline, ...) and a terminating
  empty entry.  Until the firmware can read a payload from storage or USB,
  this library is how the payload reaches PAYLOAD_BASE.

  Copyright (c) 2026, the A03s UEFI project. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef XNU_PAYLOAD_LIB_H_
#define XNU_PAYLOAD_LIB_H_

extern CONST UINT8  gXnuPayload[];
extern CONST UINTN  gXnuPayloadSize;

#endif
