/** @file
  Simple GOP driver for the Samsung Galaxy A03s framebuffer.

  The archive this file belongs to was missing the original SimpleFbDxe.c,
  but MT6765Pkg.dsc and MT6765Pkg.fdf both reference this driver.  This is a
  small, self-contained replacement: it publishes one GOP mode over the
  framebuffer that LK leaves at PcdMipiFrameBufferAddress and delegates all
  drawing to FrameBufferBltLib, which already knows how to convert the
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL format to the panel's BGRA layout.

  The panel scans DRAM, so every Blt finishes with a cache clean/invalidate
  over the visible framebuffer.  Without it the CPU cache holds the only copy
  of the picture and the display shows stale data.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/GraphicsOutput.h>

//
// FrameBufferBltConfigure() stores a private structure plus one converted
// scan line in the caller supplied buffer, so its size depends on the panel
// width.  The buffer is allocated at runtime with the size the library asks
// for instead of guessing.
//
STATIC FRAME_BUFFER_CONFIGURE                *mFrameBufferConfigure;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mMode;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGraphicsOutput;

/**
  Return information about one of the video modes.

  @param[in]  This           The GOP instance.
  @param[in]  ModeNumber     The mode number to return information about.
  @param[out] SizeOfInfo     Size of the EFI_GRAPHICS_OUTPUT_MODE_INFORMATION.
  @param[out] Info           Information about the requested mode.

  @retval EFI_SUCCESS        The mode information was returned.
  @retval EFI_UNSUPPORTED    ModeNumber is not supported.
**/
STATIC
EFI_STATUS
EFIAPI
A03sGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  *Info       = AllocateCopyPool (*SizeOfInfo, &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

/**
  Set the video mode.  The A03s has exactly one mode: the framebuffer that LK
  configured before starting this firmware.

  @param[in] This            The GOP instance.
  @param[in] ModeNumber      The mode number to activate.

  @retval EFI_SUCCESS        The requested mode is the current mode.
  @retval EFI_UNSUPPORTED    ModeNumber is not supported.
**/
STATIC
EFI_STATUS
EFIAPI
A03sGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  Blt a rectangle to the framebuffer.

  @param[in]     This          The GOP instance.
  @param[in,out] BltBuffer     The source or destination buffer.
  @param[in]     BltOperation  The operation to perform.
  @param[in]     SourceX       Source X coordinate.
  @param[in]     SourceY       Source Y coordinate.
  @param[in]     DestinationX  Destination X coordinate.
  @param[in]     DestinationY  Destination Y coordinate.
  @param[in]     Width         Width of the rectangle.
  @param[in]     Height        Height of the rectangle.
  @param[in]     Delta         Row pitch of BltBuffer, or 0.

  @retval EFI_SUCCESS          The operation completed.
  @retval EFI_DEVICE_ERROR     FrameBufferBltLib rejected the operation.
**/
STATIC
EFI_STATUS
EFIAPI
A03sGopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL         *BltBuffer,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION     BltOperation,
  IN     UINTN                                 SourceX,
  IN     UINTN                                 SourceY,
  IN     UINTN                                 DestinationX,
  IN     UINTN                                 DestinationY,
  IN     UINTN                                 Width,
  IN     UINTN                                 Height,
  IN     UINTN                                 Delta
  )
{
  RETURN_STATUS  Status;

  if (mFrameBufferConfigure == NULL) {
    return EFI_NOT_READY;
  }

  Status = FrameBufferBlt (
             mFrameBufferConfigure,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );
  if (RETURN_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // The display controller reads DRAM directly.  Flush the pixels written by
  // this Blt before returning; the range covers the whole scanout buffer
  // because FrameBufferBlt does not report which part it touched.
  //
  WriteBackInvalidateDataCacheRange (
    (VOID *)(UINTN)mMode.FrameBufferBase,
    (UINTN)mMode.FrameBufferSize
    );

  return EFI_SUCCESS;
}

/**
  Entry point of the simple framebuffer GOP driver.

  @param[in] ImageHandle  The firmware allocated handle for the image.
  @param[in] SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS     The GOP was installed.
  @retval others          The GOP could not be installed.
**/
EFI_STATUS
EFIAPI
SimpleFbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  RETURN_STATUS  Status;
  UINTN          ConfigureSize;
  UINT32         Width;
  UINT32         Height;
  UINT32         Stride;

  Width  = FixedPcdGet32 (PcdMipiFrameBufferWidth);
  Height = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  Stride = FixedPcdGet32 (PcdMipiFrameBufferStride);

  if ((Width == 0) || (Height == 0) || (Stride < Width)) {
    DEBUG ((DEBUG_ERROR, "SimpleFbDxe: invalid framebuffer geometry %ux%u/%u\n",
      Width, Height, Stride));
    return EFI_DEVICE_ERROR;
  }

  mModeInfo.Version               = 0;
  mModeInfo.HorizontalResolution  = Width;
  mModeInfo.VerticalResolution    = Height;
  //
  // FrameBufferSerialPortLib writes FB_BGRA8888_* values, i.e. blue in the
  // lowest byte.  That is PixelBlueGreenRedReserved8BitPerColor.
  //
  mModeInfo.PixelFormat           = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine     = Stride;
  ZeroMem (&mModeInfo.PixelInformation, sizeof (mModeInfo.PixelInformation));

  //
  // Ask FrameBufferBltLib how much room its private configuration needs.
  // The first call intentionally passes no buffer; the library returns
  // RETURN_BUFFER_TOO_SMALL with the required size.
  //
  ConfigureSize = 0;
  Status        = FrameBufferBltConfigure (
                    (VOID *)(UINTN)FixedPcdGet32 (PcdMipiFrameBufferAddress),
                    &mModeInfo,
                    NULL,
                    &ConfigureSize
                    );
  if (Status != RETURN_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "SimpleFbDxe: unexpected configure size result: %r\n", Status));
    return EFI_DEVICE_ERROR;
  }

  mFrameBufferConfigure = AllocatePool (ConfigureSize);
  if (mFrameBufferConfigure == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)FixedPcdGet32 (PcdMipiFrameBufferAddress),
             &mModeInfo,
             mFrameBufferConfigure,
             &ConfigureSize
             );
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SimpleFbDxe: FrameBufferBltConfigure failed: %r\n", Status));
    FreePool (mFrameBufferConfigure);
    mFrameBufferConfigure = NULL;
    return EFI_DEVICE_ERROR;
  }

  mMode.MaxMode       = 1;
  mMode.Mode          = 0;
  mMode.Info          = &mModeInfo;
  mMode.SizeOfInfo    = sizeof (mModeInfo);
  mMode.FrameBufferBase = (EFI_PHYSICAL_ADDRESS)FixedPcdGet32 (PcdMipiFrameBufferAddress);
  mMode.FrameBufferSize = (UINTN)Stride * Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  mGraphicsOutput.QueryMode = A03sGopQueryMode;
  mGraphicsOutput.SetMode   = A03sGopSetMode;
  mGraphicsOutput.Blt       = A03sGopBlt;
  mGraphicsOutput.Mode      = &mMode;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGraphicsOutput,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SimpleFbDxe: install GOP failed: %r\n", Status));
  }

  return Status;
}
