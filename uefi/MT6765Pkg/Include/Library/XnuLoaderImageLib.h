/** @file
  Embedded HackiOS XNU loader image.

  The image is a freestanding AArch64 UEFI application built from
  hackios-xnu-kernel/xnuloader.  It is linked at 0xA1000000 so that it
  stays outside the memory window XNU will use.

  Copyright (c) 2026, the A03s UEFI project. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef XNU_LOADER_IMAGE_LIB_H_
#define XNU_LOADER_IMAGE_LIB_H_

extern CONST UINT8  gXnuLoaderEfi[];
extern CONST UINTN  gXnuLoaderEfiSize;

#endif
