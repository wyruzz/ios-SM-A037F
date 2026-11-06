/** @file
  Implementation for PlatformBootManagerLib library class interfaces.

  Copyright (C) 2015-2016, Red Hat, Inc.
  Copyright (c) 2014, ARM Ltd. All rights reserved.
  Copyright (c) 2004 - 2016, Intel Corporation. All rights reserved.
  Copyright (c) 2016, Linaro Ltd. All rights reserved.
  Copyright (c), 2017, Andrei Warkentin <andrey.warkentin@gmail.com>

  This program and the accompanying materials are licensed and made available
  under the terms and conditions of the BSD License which accompanies this
  distribution. The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS, WITHOUT
  WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Library/BaseLib.h>
#include <Library/BootLogoLib.h>
#include <Library/CapsuleLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/XnuLoaderImageLib.h>
#include <Library/XnuPayloadLib.h>
#include <Configuration/XnuPayload.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EsrtManagement.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Guid/EventGroup.h>
#include <Guid/TtyTerm.h>
#include <Configuration/BootDevices.h>

#include "PlatformBm.h"

#define DP_NODE_LEN(Type) { (UINT8)sizeof (Type), (UINT8)(sizeof (Type) >> 8) }

//
// LK does not clear the whole scanout buffer before it draws the Samsung
// logo, so whatever UEFI leaves in DRAM ghosts through on the next boot.
//
// The old workaround held the loader log for A03S_LOADER_LOG_HOLD_SECONDS,
// continued into the UEFI Shell and then blacked the framebuffer after
// A03S_BLANK_DELAY_SECONDS.  It is kept only for A03S_BOOT_MENU=0: with the
// menu and keypad enabled the Shell is actually usable and must not be
// erased while the user is reading it.
//
// Set A03S_BOOT_MENU to 0 to get the old behaviour back: start XNU as soon
// as the console is ready, without asking.  With the menu enabled the phone
// shows a HakiOS boot menu for A03S_MENU_TIMEOUT_SECONDS and then starts XNU
// unless the user picked another entry.
//
#define A03S_BOOT_MENU                 1
#define A03S_MENU_TIMEOUT_SECONDS      8
#define A03S_KEYPAD_DIAG               0

#if A03S_BOOT_MENU
//
// With a working keypad the Shell is usable, so the "blank the screen after
// 60 seconds" workaround is no longer wanted: it would erase the Shell while
// the user is reading it.
//
#define A03S_BLANK_SCREEN_AFTER_SHELL  0
#else
#define A03S_BLANK_SCREEN_AFTER_SHELL  1
#endif
#define A03S_LOADER_LOG_HOLD_SECONDS   30
#define A03S_BLANK_DELAY_SECONDS       60

//
// 0: the XNUP payload is appended to the boot image; BootShim copies it to
//    XNU_PAYLOAD_BASE before this firmware starts (see build_a03s.sh).
// 1: copy the XNUP image embedded by XnuPayloadLib to XNU_PAYLOAD_BASE.
//
#define A03S_COPY_XNU_PAYLOAD  0

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH         SerialDxe;
  UART_DEVICE_PATH           Uart;
  VENDOR_DEFINED_DEVICE_PATH TermType;
  EFI_DEVICE_PATH_PROTOCOL   End;
} PLATFORM_SERIAL_CONSOLE;
#pragma pack ()

typedef struct {
  VENDOR_DEVICE_PATH            Custom;
  USB_DEVICE_PATH               Hub;
  USB_DEVICE_PATH               Dev;
  EFI_DEVICE_PATH_PROTOCOL      EndDevicePath;
} PLATFORM_USB_DEV;

#define DW_USB_DXE_FILE_GUID { \
          0x4bf1704c, 0x03f4, 0x46d5, \
          { 0xbc, 0xa6, 0x82, 0xfa, 0x58, 0x0b, 0xad, 0xfd } \
          }

STATIC PLATFORM_USB_DEV mUsbHubPort = {
  //
  // VENDOR_DEVICE_PATH DwUsbHostDxe
  //
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, DP_NODE_LEN (VENDOR_DEVICE_PATH) },
    DW_USB_DXE_FILE_GUID
  },

  //
  // USB_DEVICE_PATH Hub
  //
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_DP, DP_NODE_LEN (USB_DEVICE_PATH) },
    0, 0
  },

  //
  // USB_DEVICE_PATH Dev
  //
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_DP, DP_NODE_LEN (USB_DEVICE_PATH) },
    1, 0
  },

  //
  // EFI_DEVICE_PATH_PROTOCOL End
  //
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};

#define SERIAL_DXE_FILE_GUID { \
          0xD3987D4B, 0x971A, 0x435F, \
          { 0x8C, 0xAF, 0x49, 0x67, 0xEB, 0x62, 0x72, 0x41 } \
          }

STATIC PLATFORM_SERIAL_CONSOLE mSerialConsole = {
  //
  // VENDOR_DEVICE_PATH SerialDxe
  //
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, DP_NODE_LEN (VENDOR_DEVICE_PATH) },
    SERIAL_DXE_FILE_GUID
  },

  //
  // UART_DEVICE_PATH Uart
  //
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP, DP_NODE_LEN (UART_DEVICE_PATH) },
    0,                                      // Reserved
    FixedPcdGet64 (PcdUartDefaultBaudRate), // BaudRate
    FixedPcdGet8 (PcdUartDefaultDataBits),  // DataBits
    FixedPcdGet8 (PcdUartDefaultParity),    // Parity
    FixedPcdGet8 (PcdUartDefaultStopBits)   // StopBits
  },

  //
  // VENDOR_DEFINED_DEVICE_PATH TermType
  //
  {
    {
      MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,
      DP_NODE_LEN (VENDOR_DEFINED_DEVICE_PATH)
    }
    //
    // Guid to be filled in dynamically
    //
  },

  //
  // EFI_DEVICE_PATH_PROTOCOL End
  //
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};


#pragma pack (1)
typedef struct {
  USB_CLASS_DEVICE_PATH    Keyboard;
  EFI_DEVICE_PATH_PROTOCOL End;
} PLATFORM_USB_KEYBOARD;
#pragma pack ()

STATIC PLATFORM_USB_KEYBOARD mUsbKeyboard = {
  //
  // USB_CLASS_DEVICE_PATH Keyboard
  //
  {
    {
      MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
      DP_NODE_LEN (USB_CLASS_DEVICE_PATH)
    },
    0xFFFF, // VendorId: any
    0xFFFF, // ProductId: any
    3,      // DeviceClass: HID
    1,      // DeviceSubClass: boot
    1       // DeviceProtocol: keyboard
  },

  //
  // EFI_DEVICE_PATH_PROTOCOL End
  //
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};


/**
  Check if the handle satisfies a particular condition.

  @param[in] Handle      The handle to check.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.

  @retval TRUE   The condition is satisfied.
  @retval FALSE  Otherwise. This includes the case when the condition could not
                 be fully evaluated due to an error.
**/
typedef
BOOLEAN
(EFIAPI *FILTER_FUNCTION) (
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  );


/**
  Process a handle.

  @param[in] Handle      The handle to process.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.
**/
typedef
VOID
(EFIAPI *CALLBACK_FUNCTION)  (
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  );

/**
  Locate all handles that carry the specified protocol, filter them with a
  callback function, and pass each handle that passes the filter to another
  callback.

  @param[in] ProtocolGuid  The protocol to look for.

  @param[in] Filter        The filter function to pass each handle to. If this
                           parameter is NULL, then all handles are processed.

  @param[in] Process       The callback function to pass each handle to that
                           clears the filter.
**/
STATIC
VOID
FilterAndProcess (
  IN EFI_GUID          *ProtocolGuid,
  IN FILTER_FUNCTION   Filter         OPTIONAL,
  IN CALLBACK_FUNCTION Process
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN      NoHandles;
  UINTN      Idx;

  Status = gBS->LocateHandleBuffer (ByProtocol, ProtocolGuid,
                  NULL /* SearchKey */, &NoHandles, &Handles);
  if (EFI_ERROR (Status)) {
    //
    // This is not an error, just an informative condition.
    //
    DEBUG ((EFI_D_VERBOSE, "%a: %g: %r\n", __FUNCTION__, ProtocolGuid,
      Status));
    return;
  }

  ASSERT (NoHandles > 0);
  for (Idx = 0; Idx < NoHandles; ++Idx) {
    CHAR16        *DevicePathText;
    STATIC CHAR16 Fallback[] = L"<device path unavailable>";

    //
    // The ConvertDevicePathToText() function handles NULL input transparently.
    //
    DevicePathText = ConvertDevicePathToText (
                       DevicePathFromHandle (Handles[Idx]),
                       FALSE, // DisplayOnly
                       FALSE  // AllowShortcuts
                       );
    if (DevicePathText == NULL) {
      DevicePathText = Fallback;
    }

    if (Filter == NULL || Filter (Handles[Idx], DevicePathText)) {
      Process (Handles[Idx], DevicePathText);
    }

    if (DevicePathText != Fallback) {
      FreePool (DevicePathText);
    }
  }
  gBS->FreePool (Handles);
}

/**
  This CALLBACK_FUNCTION retrieves the EFI_DEVICE_PATH_PROTOCOL from the
  handle, and adds it to ConOut and ErrOut.
**/
STATIC
VOID
EFIAPI
AddOutput (
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  )
{
  EFI_STATUS               Status;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    DEBUG ((EFI_D_ERROR, "%a: %s: handle %p: device path not found\n",
      __FUNCTION__, ReportText, Handle));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: %s: adding to ConOut: %r\n", __FUNCTION__,
      ReportText, Status));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: %s: adding to ErrOut: %r\n", __FUNCTION__,
      ReportText, Status));
    return;
  }

  DEBUG ((EFI_D_VERBOSE, "%a: %s: added to ConOut and ErrOut\n", __FUNCTION__,
    ReportText));
}

STATIC
VOID
EFIAPI
AddInput (
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  )
{
  EFI_STATUS               Status;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    DEBUG ((EFI_D_ERROR, "%a: %s: handle %p: device path not found\n",
      __FUNCTION__, ReportText, Handle));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable (ConIn, DevicePath, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: %s: adding to ConIn: %r\n", __FUNCTION__,
      ReportText, Status));
    return;
  }

  DEBUG ((EFI_D_VERBOSE, "%a: %s: added to ConOut and ErrOut\n", __FUNCTION__,
    ReportText));
}

STATIC
INTN
PlatformRegisterBootOption (
  EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  CHAR16                   *Description,
  UINT32                   Attributes
  )
{
  EFI_STATUS                        Status;
  INTN                              OptionIndex;
  EFI_BOOT_MANAGER_LOAD_OPTION      NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION      *BootOptions;
  UINTN                             BootOptionCount;

  Status = EfiBootManagerInitializeLoadOption (
             &NewOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             Attributes,
             Description,
             DevicePath,
             NULL,
             0
             );
  ASSERT_EFI_ERROR (Status);

  BootOptions = EfiBootManagerGetLoadOptions (
                  &BootOptionCount, LoadOptionTypeBoot
                  );

  OptionIndex = EfiBootManagerFindLoadOption (
                  &NewOption, BootOptions, BootOptionCount
                  );

  if (OptionIndex == -1) {
    Status = EfiBootManagerAddLoadOptionVariable (&NewOption, MAX_UINTN);
    ASSERT_EFI_ERROR (Status);
    OptionIndex = BootOptionCount;
  }

  EfiBootManagerFreeLoadOption (&NewOption);
  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

  return OptionIndex;
}

STATIC
INTN
PlatformRegisterFvBootOption (
  CONST EFI_GUID                   *FileGuid,
  CHAR16                           *Description,
  UINT32                           Attributes
  )
{
  EFI_STATUS                        Status;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FileNode;
  EFI_LOADED_IMAGE_PROTOCOL         *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL          *DevicePath;
  INTN OptionIndex;

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **) &LoadedImage
                  );
  ASSERT_EFI_ERROR (Status);

  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  ASSERT (DevicePath != NULL);
  DevicePath = AppendDevicePathNode (
                 DevicePath,
                 (EFI_DEVICE_PATH_PROTOCOL *) &FileNode
                 );
  ASSERT (DevicePath != NULL);

  OptionIndex = PlatformRegisterBootOption (DevicePath,
                                            Description,
                                            Attributes);
  FreePool (DevicePath);

  return OptionIndex;
}


STATIC
VOID
PlatformRegisterOptionsAndKeys (
  VOID
  )
{
  INTN ShellOption;

  ShellOption = PlatformRegisterFvBootOption(
      &gUefiShellFileGuid, 
      L"UEFI Shell",
      LOAD_OPTION_ACTIVE
  );
}

STATIC
VOID
PlatformRegisterSetupKey(
  VOID
)
{
  EFI_STATUS                   Status;
  EFI_INPUT_KEY                PowerBtn;
  EFI_BOOT_MANAGER_LOAD_OPTION BootOption;

  //
  // Map Power to Boot Manager Menu
  //
  PowerBtn.ScanCode    = SCAN_NULL;
  PowerBtn.UnicodeChar = CHAR_CARRIAGE_RETURN;
  Status = EfiBootManagerGetBootManagerMenu(&BootOption);
  ASSERT_EFI_ERROR(Status);
  Status = EfiBootManagerAddKeyOptionVariable(
      NULL, 
      (UINT16) BootOption.OptionNumber, 
      0, 
      &PowerBtn, 
      NULL
  );
  ASSERT (Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
}


//
// BDS Platform Functions
//
/**
  Do the platform init, can be customized by OEM/IBV
  Possible things that can be done in PlatformBootManagerBeforeConsole:
  > Update console variable: 1. include hot-plug devices;
  >                          2. Clear ConIn and add SOL for AMT
  > Register new Driver#### or Boot####
  > Register new Key####: e.g.: F12
  > Signal ReadyToLock event
  > Authentication action: 1. connect Auth devices;
  >                        2. Identify auto logon user.
**/
VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_STATUS                    Status;
  ESRT_MANAGEMENT_PROTOCOL      *EsrtManagement;

  if (GetBootModeHob() == BOOT_ON_FLASH_UPDATE) {
    DEBUG ((DEBUG_INFO, "ProcessCapsules Before EndOfDxe ......\n"));
    Status = ProcessCapsules ();
    DEBUG ((DEBUG_INFO, "ProcessCapsules returned %r\n", Status));
  } else {
    Status = gBS->LocateProtocol (&gEsrtManagementProtocolGuid, NULL,
                    (VOID **)&EsrtManagement);
    if (!EFI_ERROR (Status)) {
      EsrtManagement->SyncEsrtFmp ();
    }
  }

  //
  // Now add the device path of all handles with GOP on them to ConOut and
  // ErrOut.
  //
  FilterAndProcess (&gEfiGraphicsOutputProtocolGuid, NULL, AddOutput);

  //
  // Add the hardcoded short-form USB keyboard device path to ConIn.
  //
  EfiBootManagerUpdateConsoleVariable (ConIn,
    (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard, NULL);

  //
  // Now add the device path of all handles with QcomKeypadDeviceProtocolGuid
  // on them to ConIn.
  //
  FilterAndProcess (&gEFIDroidKeypadDeviceProtocolGuid, NULL, AddInput);
  // Register setup key then
  PlatformRegisterSetupKey();

  //
  // Add the hardcoded serial console device path to ConIn, ConOut, ErrOut.
  //
  ASSERT (FixedPcdGet8 (PcdDefaultTerminalType) == 4);
  CopyGuid (&mSerialConsole.TermType.Guid, &gEfiTtyTermGuid);

  EfiBootManagerUpdateConsoleVariable (ConIn,
    (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut,
    (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut,
    (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);

  //
  // Signal EndOfDxe PI Event
  //
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  //
  // Dispatch deferred images after EndOfDxe event and ReadyToLock installation.
  //
  EfiBootManagerDispatchDeferredImages ();
}

/**
  Keep the loader log on screen for a while, then return control to BDS so
  that the UEFI Shell can start.  The framebuffer is cleared later, by a
  one-shot timer, while the Shell sits idle at its prompt.
**/
STATIC
VOID
XnuLoaderHoldScreen (
  VOID
  )
{
  //
  // The loader disables the watchdog itself, but do it here too so a
  // loader that failed to start cannot reset the phone while the log is
  // being read.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  Print (
    L"XNU loader: holding this screen for %u seconds...\n",
    (UINT32)A03S_LOADER_LOG_HOLD_SECONDS
    );
#if A03S_BLANK_SCREEN_AFTER_SHELL
  Print (
    L"XNU loader: returning to the UEFI Shell; it will be cleared in %u seconds.\n",
    (UINT32)A03S_BLANK_DELAY_SECONDS
    );
#endif
  gBS->Stall (A03S_LOADER_LOG_HOLD_SECONDS * 1000000);
}

#if A03S_BLANK_SCREEN_AFTER_SHELL

/**
  Fill the visible part of the GOP framebuffer with black.

  This runs from a one-shot timer after the UEFI Shell has started.  The
  Shell has no input device and stays idle at its prompt, so nothing will
  draw over the black screen afterwards.  GOP Blt is used instead of a
  raw memset so that SimpleFbDxe flushes the CPU cache to DRAM: the
  display controller scans DRAM, and the cache is lost on power-off.

  @param[in] Event    The timer event that fired.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
A03sBlankScreenNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black;

  Gop    = NULL;
  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (!EFI_ERROR (Status) && (Gop->Mode != NULL) &&
      (Gop->Mode->FrameBufferBase != 0) && (Gop->Mode->Info != NULL))
  {
    Black.Blue     = 0;
    Black.Green    = 0;
    Black.Red      = 0;
    Black.Reserved = 0;
    Status = Gop->Blt (
                    Gop,
                    &Black,
                    EfiBltVideoFill,
                    0,
                    0,
                    0,
                    0,
                    Gop->Mode->Info->HorizontalResolution,
                    Gop->Mode->Info->VerticalResolution,
                    0
                    );
    DEBUG ((DEBUG_INFO, "A03s: framebuffer blank: %r\n", Status));
  }

  gBS->CloseEvent (Event);
}

/**
  Arm the one-shot timer that clears the framebuffer after the Shell has
  had time to appear.  Called at the end of PlatformBootManagerAfterConsole,
  just before BDS boots the Shell (PcdPlatformBootTimeOut is 0).
**/
STATIC
VOID
A03sScheduleScreenBlank (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Event  = NULL;
  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_CALLBACK,
                  A03sBlankScreenNotify,
                  NULL,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A03s: cannot create blank timer: %r\n", Status));
    return;
  }

  Status = gBS->SetTimer (
                  Event,
                  TimerRelative,
                  A03S_BLANK_DELAY_SECONDS * 10000000ULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A03s: cannot arm blank timer: %r\n", Status));
    gBS->CloseEvent (Event);
  }
}

#endif

/**
  Hand control to the HackiOS XNU loader embedded in this firmware.

  The loader is a freestanding UEFI application linked at 0xA1000000 so
  that it stays outside the memory window XNU will use.  It is started
  here, from BDS, and not from a DXE driver: a driver's entry point can
  run before TerminalDxe has attached itself to ConSplitter, in which
  case the loader's log would be written to a console nobody sees.

  With no payload in RAM the loader returns instead of resetting, so its
  log stays readable.  The log is held for 30 seconds and then control
  returns to BDS, which starts the UEFI Shell; a one-shot timer clears the
  framebuffer 60 seconds later, leaving a black screen for the next
  power-on (LK does not clear the scanout buffer itself).
**/
STATIC
VOID
XnuLoaderStart (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  LoaderHandle;

  Print (
    L"\nXNU loader: handing control to the XNU loader (%u bytes)...\n",
    (UINT32)gXnuLoaderEfiSize
    );

  //
  // The firmware has no USB host or storage driver yet, so a payload has
  // to be embedded in the FV and copied into the RAM area the loader probes.
  //
#if A03S_COPY_XNU_PAYLOAD
  CopyMem ((VOID *)(UINTN)XNU_PAYLOAD_BASE, gXnuPayload, gXnuPayloadSize);
  Print (
    L"XNU loader: payload at 0x%lx (%u bytes)\n",
    (UINT64)XNU_PAYLOAD_BASE,
    (UINT32)gXnuPayloadSize
    );
#else
  //
  // BootShim copied the XNUP payload from the boot image before starting
  // this firmware (and cleared the magic if the image had none).  Do not
  // touch the payload area here: writing to it would destroy the container.
  //
  Print (L"XNU loader: payload copied by BootShim\n");
#endif

  LoaderHandle = NULL;
  Status = gBS->LoadImage (
                  FALSE,
                  gImageHandle,
                  NULL,
                  (VOID *)gXnuLoaderEfi,
                  gXnuLoaderEfiSize,
                  &LoaderHandle
                  );
  if (EFI_ERROR (Status)) {
    Print (L"XNU loader: LoadImage failed: %r\n", Status);
    XnuLoaderHoldScreen ();
    return;
  }

  Status = gBS->StartImage (LoaderHandle, NULL, NULL);
  Print (L"XNU loader: loader returned: %r\n", Status);
  XnuLoaderHoldScreen ();
}

/**
  HakiOS boot menu for the A03s.

  The menu is deliberately built on the VT-UTF8 terminal that already works
  on this panel (TerminalDxe -> SerialDxe -> FrameBufferSerialPortLib), not on
  GraphicsConsoleDxe: the framebuffer console has always drawn correctly,
  while GraphicsConsoleDxe never produced readable text on this display.

  Input comes from A03sKeypadDxe through ConSplitter.  If the keypad driver
  did not load, the countdown simply runs out and XNU starts as before.
**/

typedef enum {
  A03sMenuStartXnu = 0,
  A03sMenuShell,
  A03sMenuBootManager,
  A03sMenuReboot,
  A03sMenuItemCount
} A03S_MENU_ITEM;

STATIC CONST CHAR16 *mMenuItems[A03sMenuItemCount] = {
  L"Start HakiOS (XNU)",
  L"UEFI Shell",
  L"UEFI Boot Manager",
  L"Reboot"
};

//
// The framebuffer terminal is 60x66 cells.  The content block is 20 rows
// tall, so start at row 18 to sit it in the middle of the panel.
//
#define A03S_MENU_TOP        18
#define A03S_MENU_FIRST_ROW  (A03S_MENU_TOP + 13)

STATIC
UINTN
A03sMenuColumns (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINTN                         Columns;

  //
  // TerminalDxe reports its own 80x25 text mode, but the VT-UTF8 backend
  // is the framebuffer console: 720 px / 12 px per cell = 60 columns.
  // Centring against 80 columns would push the right border off-screen.
  //
  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (!EFI_ERROR (Status) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL)) {
    Columns = Gop->Mode->Info->HorizontalResolution / 12;
    if ((Columns >= 40) && (Columns <= 200)) {
      return Columns;
    }
  }

  return 60;
}

STATIC
VOID
A03sMenuPrintAt (
  IN UINTN        Column,
  IN UINTN        Row,
  IN CONST CHAR16 *Text
  )
{
  if (gST->ConOut == NULL) {
    return;
  }

  gST->ConOut->SetCursorPosition (gST->ConOut, Column, Row);
  gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
}

STATIC
VOID
A03sMenuPrintCentered (
  IN UINTN        Row,
  IN CONST CHAR16 *Text
  )
{
  UINTN  Columns;
  UINTN  Length;

  Columns = A03sMenuColumns ();
  Length  = StrLen (Text);
  A03sMenuPrintAt ((Columns > Length) ? (Columns - Length) / 2 : 0, Row, Text);
}

/**
  Print a centred line padded with spaces to the console width.  The padding
  erases the previous contents of the line, which is what the countdown and
  the status line need when their text changes length.
**/
STATIC
VOID
A03sMenuPrintPadded (
  IN UINTN        Row,
  IN CONST CHAR16 *Text
  )
{
  CHAR16  Buffer[96];
  UINTN   Columns;
  UINTN   Width;
  UINTN   Length;
  UINTN   Index;

  Columns = A03sMenuColumns ();
  Width   = (Columns > 4) ? (Columns - 4) : Columns;
  if (Width >= ARRAY_SIZE (Buffer)) {
    Width = ARRAY_SIZE (Buffer) - 1;
  }

  Length = StrLen (Text);
  for (Index = 0; Index < Width; Index++) {
    Buffer[Index] = (Index < Length) ? Text[Index] : L' ';
  }
  Buffer[Width] = L'\0';

  A03sMenuPrintCentered (Row, Buffer);
}

/**
  Draw the small red accent line under the wordmark.  This is the only piece
  of the menu that uses GOP directly; if GOP is missing the text menu is
  still complete.
**/
STATIC
VOID
A03sMenuDrawAccent (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Red;
  UINTN                         Width;
  UINTN                         LineWidth;

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return;
  }

  Red.Blue     = 0;
  Red.Green    = 0;
  Red.Red      = 0xFF;
  Red.Reserved = 0;

  Width     = Gop->Mode->Info->HorizontalResolution;
  LineWidth = (Width > 240) ? 240 : Width;

  //
  // Text row A03S_MENU_TOP+5 occupies y=(A03S_MENU_TOP+5)*24..+23 at the
  // 12x24 cell size used by the framebuffer terminal.  Put the line in the
  // empty row below it.
  //
  Gop->Blt (
         Gop,
         &Red,
         EfiBltVideoFill,
         0,
         0,
         (Width - LineWidth) / 2,
         (A03S_MENU_TOP + 6) * 24 + 8,
         LineWidth,
         4,
         0
         );
}

STATIC
VOID
A03sMenuDrawItem (
  IN UINTN        Row,
  IN BOOLEAN      Selected,
  IN CONST CHAR16 *Text
  )
{
  CHAR16  Line[64];
  UINTN   Index;
  UINTN   Length;
  UINTN   Field;

  //
  // Box is 42 columns wide: '|' + 40 inner columns + '|'.  Inside, the first
  // four columns are "| > " / "|   ", the rest is the label.
  //
  Field = 36;
  Length = StrLen (Text);

  Line[0] = L'|';
  Line[1] = L' ';
  Line[2] = Selected ? L'>' : L' ';
  Line[3] = L' ';
  for (Index = 0; Index < Field; Index++) {
    Line[4 + Index] = (Index < Length) ? Text[Index] : L' ';
  }
  Line[4 + Field] = L' ';
  Line[5 + Field] = L'|';
  Line[6 + Field] = L'\0';

  A03sMenuPrintCentered (Row, Line);
}

STATIC
VOID
A03sMenuDrawItems (
  IN UINTN  Selected
  )
{
  UINTN  Index;

  for (Index = 0; Index < A03sMenuItemCount; Index++) {
    A03sMenuDrawItem (
      A03S_MENU_FIRST_ROW + Index,
      Index == Selected,
      mMenuItems[Index]
      );
  }
}

STATIC
VOID
A03sMenuDrawStatus (
  IN BOOLEAN  KeySeen,
  IN UINTN    Seconds
  )
{
  CHAR16  Text[64];

  if (KeySeen) {
    UnicodeSPrint (Text, sizeof (Text), L"Press Power to start HakiOS");
  } else {
    UnicodeSPrint (Text, sizeof (Text), L"Starting HakiOS in %u s", (UINT32)Seconds);
  }

  A03sMenuPrintPadded (A03S_MENU_TOP + 24, Text);
}

#if A03S_KEYPAD_DIAG
//
// Keypad diagnostic: reads MT6765 keypad controller and PMIC power-key status
// directly and renders them on screen.  The user presses volume / power keys
// and watches which bits change.
//
// KP_BASE  = 0x10010000 (confirmed by DTB /kp@10010000)
// KP_MEM1  = +0x04  codes  0-15  (Vol Down = code 0)
// KP_MEM2  = +0x08  codes 16-31  (Vol Up   = code 17)
// KP_MEM3  = +0x0C  codes 32-47
// KP_MEM4  = +0x10  codes 48-63
// KP_EN    = +0x24  keypad enable
// KP_SEL   = +0x20  scan select
// KP_DEBNC = +0x18  debounce
// KP_SCAN  = +0x1C  scan timing
// PMIC TOPSTATUS = 0x24 via pwrap at 0x1000D000, bit 1 = power key released
//

#define KP_DIAG_BASE        0x10010000U
#define KP_DIAG_STA         (KP_DIAG_BASE + 0x00U)
#define KP_DIAG_MEM1        (KP_DIAG_BASE + 0x04U)
#define KP_DIAG_MEM2        (KP_DIAG_BASE + 0x08U)
#define KP_DIAG_MEM3        (KP_DIAG_BASE + 0x0CU)
#define KP_DIAG_MEM4        (KP_DIAG_BASE + 0x10U)
#define KP_DIAG_MEM5        (KP_DIAG_BASE + 0x14U)
#define KP_DIAG_DEBOUNCE    (KP_DIAG_BASE + 0x18U)
#define KP_DIAG_SCAN_TIMING (KP_DIAG_BASE + 0x1CU)
#define KP_DIAG_SEL         (KP_DIAG_BASE + 0x20U)
#define KP_DIAG_EN          (KP_DIAG_BASE + 0x24U)

#define PWRAP_DIAG_BASE     0x1000D000U
#define PWRAP_DIAG_CMD      (PWRAP_DIAG_BASE + 0xC20U)
#define PWRAP_DIAG_RDATA    (PWRAP_DIAG_BASE + 0xC24U)
#define PWRAP_DIAG_VLDCLR   (PWRAP_DIAG_BASE + 0xC28U)
#define PWRAP_DIAG_INIT_DONE2 0x00400000U
#define PWRAP_DIAG_FSM_IDLE     0x0U
#define PWRAP_DIAG_FSM_WFVLDCLR 0x6U
#define PWRAP_DIAG_TIMEOUT    100000U

#define PMIC_DIAG_TOPSTATUS   0x24U
#define PMIC_DIAG_PWRKEY_BIT  0x0002U

STATIC
BOOLEAN
A03sDiagPmicRead (
  IN  UINT16  Address,
  OUT UINT16  *Value
  )
{
  UINT32  Data;
  UINTN   Timeout;

  Timeout = PWRAP_DIAG_TIMEOUT;
  do {
    Data = MmioRead32 (PWRAP_DIAG_RDATA);
    if ((Data & PWRAP_DIAG_INIT_DONE2) == 0) {
      return FALSE;
    }
    if (((Data >> 16) & 0x7U) == PWRAP_DIAG_FSM_IDLE) {
      break;
    }
  } while (--Timeout != 0);
  if (Timeout == 0) {
    return FALSE;
  }

  MmioWrite32 (PWRAP_DIAG_CMD, ((UINT32)(Address >> 1) << 16));

  Timeout = PWRAP_DIAG_TIMEOUT;
  do {
    Data = MmioRead32 (PWRAP_DIAG_RDATA);
    if ((Data & PWRAP_DIAG_INIT_DONE2) == 0) {
      return FALSE;
    }
    if (((Data >> 16) & 0x7U) == PWRAP_DIAG_FSM_WFVLDCLR) {
      break;
    }
  } while (--Timeout != 0);
  if (Timeout == 0) {
    return FALSE;
  }

  *Value = (UINT16)(Data & 0xFFFFU);
  MmioWrite32 (PWRAP_DIAG_VLDCLR, 1);

  return TRUE;
}

STATIC
VOID
A03sKeypadDiagDraw (
  IN UINTN  Frame
  )
{
  CHAR16        Text[128];
  UINT16        PmicStatus;
  BOOLEAN       PmicOk;
  UINT16        KpSta, KpMem1, KpMem2, KpMem3, KpMem4, KpMem5;
  UINT16        KpEn, KpSel, KpDebounce, KpScanTim;
  UINT64        PressedMask;
  UINTN         Code;
  UINTN         Row;
  BOOLEAN       AnyPressed;
  UINT16        SelValue;
  UINTN         ColIndex;

  if (gST->ConOut == NULL) {
    return;
  }

  //
  // Cycle KP_SEL through single column bits: 0x0001, 0x0002, 0x0004, ...
  // Each column is held for ~3 seconds so the user can press a key while
  // it is active.  This finds which column the volume keys are wired to.
  //
  ColIndex = (Frame / 30) % 8;   // ~3 s per column at ~100 ms frames
  SelValue = (UINT16)(1U << ColIndex);
  MmioWrite16 (0x10010000U + 0x20U, SelValue);
  gBS->Stall (5000);  // let the scan settle

  KpSta        = MmioRead16 (0x10010000U + 0x00U);
  KpMem1       = MmioRead16 (0x10010000U + 0x04U);
  KpMem2       = MmioRead16 (0x10010000U + 0x08U);
  KpMem3       = MmioRead16 (0x10010000U + 0x0CU);
  KpMem4       = MmioRead16 (0x10010000U + 0x10U);
  KpMem5       = MmioRead16 (0x10010000U + 0x14U);
  KpEn         = MmioRead16 (0x10010000U + 0x24U);
  KpSel        = MmioRead16 (0x10010000U + 0x20U);
  KpDebounce   = MmioRead16 (0x10010000U + 0x18U);
  KpScanTim    = MmioRead16 (0x10010000U + 0x1CU);

  PmicOk = A03sDiagPmicRead (0x24U, &PmicStatus);

  PressedMask = 0;
  for (Code = 0; Code < 16;  Code++) { if (((~KpMem1) >> Code) & 1) PressedMask |= (UINT64)1 << Code; }
  for (Code = 0; Code < 16;  Code++) { if (((~KpMem2) >> Code) & 1) PressedMask |= (UINT64)1 << (Code + 16); }
  for (Code = 0; Code < 16;  Code++) { if (((~KpMem3) >> Code) & 1) PressedMask |= (UINT64)1 << (Code + 32); }
  for (Code = 0; Code < 16;  Code++) { if (((~KpMem4) >> Code) & 1) PressedMask |= (UINT64)1 << (Code + 48); }
  for (Code = 0; Code < 8;   Code++) { if (((~KpMem5) >> Code) & 1) PressedMask |= (UINT64)1 << (Code + 64); }

  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);

  A03sMenuPrintCentered (2, L"HakiOS Keypad Diagnostic");
  A03sMenuPrintCentered (3, L"Samsung Galaxy A03s (MT6765)");
  A03sMenuPrintCentered (4, L"KP_SEL cycling per column, ~3 s each");

  Row = 6;
  UnicodeSPrint (Text, sizeof (Text), L"Frame: %u", (UINT32)Frame);
  A03sMenuPrintAt (0, Row++, Text);

  UnicodeSPrint (Text, sizeof (Text), L"KP_SEL    0x10010020 = 0x%04x  (col %u)", KpSel, ColIndex);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_STA    0x10010000 = 0x%04x", KpSta);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_MEM1   0x10010004 = 0x%04x", KpMem1);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_MEM2   0x10010008 = 0x%04x", KpMem2);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_MEM3   0x1001000C = 0x%04x", KpMem3);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_MEM4   0x10010010 = 0x%04x", KpMem4);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_MEM5   0x10010014 = 0x%04x", KpMem5);
  A03sMenuPrintAt (0, Row++, Text);

  UnicodeSPrint (Text, sizeof (Text), L"KP_EN     0x10010024 = 0x%04x", KpEn);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_DEBOUNCE 0x10010018 = 0x%04x", KpDebounce);
  A03sMenuPrintAt (0, Row++, Text);
  UnicodeSPrint (Text, sizeof (Text), L"KP_SCAN_TIM 0x1001001C = 0x%04x", KpScanTim);
  A03sMenuPrintAt (0, Row++, Text);

  Row++;
  if (PmicOk) {
    UnicodeSPrint (Text, sizeof (Text), L"PMIC TOPSTATUS 0x24 = 0x%04x  PWR bit1=%d",
                   PmicStatus, (PmicStatus & 0x0002U) ? 1 : 0);
  } else {
    UnicodeSPrint (Text, sizeof (Text), L"PMIC = NOT READABLE");
  }
  A03sMenuPrintAt (0, Row++, Text);

  Row++;
  AnyPressed = FALSE;
  UnicodeSPrint (Text, sizeof (Text), L"Pressed codes:");
  A03sMenuPrintAt (0, Row++, Text);
  for (Code = 0; Code < 72; Code++) {
    if ((PressedMask & ((UINT64)1 << Code)) != 0) {
      UnicodeSPrint (Text, sizeof (Text), L"  code %2u", (UINT32)Code);
      A03sMenuPrintAt (0, Row++, Text);
      AnyPressed = TRUE;
    }
  }
  if (!AnyPressed) {
    A03sMenuPrintAt (0, Row++, L"  (none)");
  }

  Row++;
  UnicodeSPrint (Text, sizeof (Text),
                 L"VolDown(c0)=%d  VolUp(c17)=%d",
                 ((PressedMask & (UINT64)1 << 0)  != 0) ? 1 : 0,
                 ((PressedMask & (UINT64)1 << 17) != 0) ? 1 : 0);
  A03sMenuPrintAt (0, Row++, Text);

  A03sMenuPrintAt (0, Row + 1, L"Any key to exit diagnostic");

  //
  // Slow down so the screen does not flicker.
  //
  gBS->Stall (1000000);  // 1 second per frame — stable screen, no flicker
}
#endif // A03S_KEYPAD_DIAG

STATIC
VOID
A03sMenuDraw (
  IN UINTN    Selected,
  IN BOOLEAN  KeySeen,
  IN UINTN    Seconds
  )
{
  if (gST->ConOut == NULL) {
    return;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);

  A03sMenuDrawAccent ();

  A03sMenuPrintCentered (A03S_MENU_TOP + 5, L"H a k i O S");
  A03sMenuPrintCentered (A03S_MENU_TOP + 7, L"Samsung Galaxy A03s");
  A03sMenuPrintCentered (A03S_MENU_TOP + 8, L"HakiOS UEFI  |  MediaTek MT6765");

  A03sMenuPrintCentered (A03S_MENU_TOP + 12, L"+----------------------------------------+");
  A03sMenuDrawItems (Selected);
  A03sMenuPrintCentered (A03S_MENU_TOP + 17, L"+----------------------------------------+");

  A03sMenuPrintCentered (A03S_MENU_TOP + 20, L"Volume Up / Down    move");
  A03sMenuPrintCentered (A03S_MENU_TOP + 21, L"Power               select");

  A03sMenuDrawStatus (KeySeen, Seconds);
}

//
// Direct hardware input for the boot menu.
//
// The DXE keypad driver is not reliably reaching ConIn on this port (BDS
// never sees its keys), so the menu samples the same registers that the
// on-screen diagnostic proved work.  This makes the menu usable even if the
// ConIn path is broken; the driver is still kept for the UEFI Shell.
//
#define A03S_MENU_KP_MEM1        0x10010004U
#define A03S_MENU_KP_MEM2        0x10010008U
#define A03S_MENU_KP_MEM3        0x1001000CU
#define A03S_MENU_KP_MEM4        0x10010010U

#define A03S_MENU_PWRAP_BASE     0x1000D000U
#define A03S_MENU_WACS2_CMD      (A03S_MENU_PWRAP_BASE + 0xC20U)
#define A03S_MENU_WACS2_RDATA    (A03S_MENU_PWRAP_BASE + 0xC24U)
#define A03S_MENU_WACS2_VLDCLR   (A03S_MENU_PWRAP_BASE + 0xC28U)
#define A03S_MENU_PWRAP_DONE2    0x00400000U
#define A03S_MENU_PMIC_TOPSTATUS 0x24U

#define A03S_MENU_KEY_VOLUP      0x01U
#define A03S_MENU_KEY_POWER      0x02U
#define A03S_MENU_KEY_VOLDOWN    0x04U

STATIC
BOOLEAN
A03sMenuPmicRead (
  IN  UINT16  Address,
  OUT UINT16  *Value
  )
{
  UINT32  Data;
  UINTN   Timeout;

  Timeout = 100000;
  do {
    Data = MmioRead32 (A03S_MENU_WACS2_RDATA);
    if ((Data & A03S_MENU_PWRAP_DONE2) == 0) {
      return FALSE;
    }
    if (((Data >> 16) & 0x7U) == 0) {
      break;
    }
  } while (--Timeout != 0);
  if (Timeout == 0) {
    return FALSE;
  }

  MmioWrite32 (A03S_MENU_WACS2_CMD, ((UINT32)(Address >> 1) << 16));

  Timeout = 100000;
  do {
    Data = MmioRead32 (A03S_MENU_WACS2_RDATA);
    if ((Data & A03S_MENU_PWRAP_DONE2) == 0) {
      return FALSE;
    }
    if (((Data >> 16) & 0x7U) == 6U) {
      break;
    }
  } while (--Timeout != 0);
  if (Timeout == 0) {
    return FALSE;
  }

  *Value = (UINT16)(Data & 0xFFFFU);
  MmioWrite32 (A03S_MENU_WACS2_VLDCLR, 1);
  return TRUE;
}

/**
  Read the menu keys straight from the hardware.

  @return  Bit mask of A03S_MENU_KEY_* values.
**/
STATIC
UINT32
A03sMenuReadKeys (
  VOID
  )
{
  UINT32  Keys;
  UINT16  Mem1;
  UINT16  Mem2;
  UINT16  Mem3;
  UINT16  Mem4;
  UINT16  Status;

  Keys = 0;

  //
  // Matrix: any non-0xFFFF word means something is pressed.  Volume Up is
  // the only matrix key on this unit, and one press lights up a ghost
  // pattern, so the whole matrix is treated as a single key.
  //
  Mem1 = MmioRead16 (A03S_MENU_KP_MEM1);
  Mem2 = MmioRead16 (A03S_MENU_KP_MEM2);
  Mem3 = MmioRead16 (A03S_MENU_KP_MEM3);
  Mem4 = MmioRead16 (A03S_MENU_KP_MEM4);
  if ((Mem1 != 0xFFFFU) || (Mem2 != 0xFFFFU) ||
      (Mem3 != 0xFFFFU) || (Mem4 != 0xFFFFU)) {
    Keys |= A03S_MENU_KEY_VOLUP;
  }

  //
  // PMIC: bit 1 = power key, bit 3 = home key (Volume Down on this unit).
  // Both are 1 while released.
  //
  if (A03sMenuPmicRead (A03S_MENU_PMIC_TOPSTATUS, &Status)) {
    if ((Status & 0x0002U) == 0) {
      Keys |= A03S_MENU_KEY_POWER;
    }
    if ((Status & 0x0008U) == 0) {
      Keys |= A03S_MENU_KEY_VOLDOWN;
    }
  }

  return Keys;
}

/**
  Handle one key.  Returns TRUE when the key confirms the current selection.
**/
STATIC
BOOLEAN
A03sMenuHandleKey (
  IN     EFI_INPUT_KEY  *Key,
  IN OUT UINTN          *Selected
  )
{
  if (Key->ScanCode == SCAN_UP) {
    *Selected = (*Selected + A03sMenuItemCount - 1) % A03sMenuItemCount;
    A03sMenuDrawItems (*Selected);
    return FALSE;
  }

  if (Key->ScanCode == SCAN_DOWN) {
    *Selected = (*Selected + 1) % A03sMenuItemCount;
    A03sMenuDrawItems (*Selected);
    return FALSE;
  }

  if (Key->UnicodeChar == CHAR_CARRIAGE_RETURN) {
    return TRUE;
  }

  return FALSE;
}

/**
  Show the menu and wait for a selection or the timeout.

  The countdown stops as soon as any key is pressed, so the user is never
  forced to race the timer while reading the entries.
**/
STATIC
A03S_MENU_ITEM
A03sMenuSelect (
  VOID
  )
{
  UINTN         Selected;
  UINTN         ElapsedMs;
  UINTN         Seconds;
  UINTN         LastSeconds;
  BOOLEAN       Countdown;
  EFI_INPUT_KEY Key;
  UINT32        LastKeys;
  UINT32        StableKeys;
  UINTN         SameCount;
  BOOLEAN       KeyLatched;

  Selected    = 0;
  ElapsedMs   = 0;
  LastSeconds = A03S_MENU_TIMEOUT_SECONDS;
  Countdown   = TRUE;
  LastKeys    = 0;
  StableKeys  = 0;
  SameCount   = 0;
  KeyLatched  = FALSE;

#if A03S_KEYPAD_DIAG
  //
  // Diagnostic mode: continuously draw the keypad register dump until any
  // key is pressed, then fall through to the normal menu (which will start
  // XNU since no real key handling is wired in diag mode).
  //
  {
    UINTN        Frame = 0;
    EFI_INPUT_KEY  DiagKey;

    for (;;) {
      A03sKeypadDiagDraw (Frame++);
      if (gST->ConIn != NULL) {
        while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &DiagKey))) {
          break;
        }
      }
      gBS->Stall (100000);  // 100 ms between frames
    }
  }
#endif

  A03sMenuDraw (Selected, FALSE, A03S_MENU_TIMEOUT_SECONDS);

  for (;;) {
    UINT32  Keys;

    //
    // Poll the hardware directly.  This is the path the on-screen
    // diagnostic proved works, and it does not depend on the keypad DXE
    // driver being present in ConIn.
    //
    Keys = A03sMenuReadKeys ();
    if (Keys == LastKeys) {
      if (SameCount < 4) {
        SameCount++;
      }
    } else {
      LastKeys  = Keys;
      SameCount = 0;
    }

    if (SameCount >= 2) {
      StableKeys = LastKeys;
      if (StableKeys == 0) {
        KeyLatched = FALSE;
      } else if (!KeyLatched) {
        KeyLatched = TRUE;

        if (Countdown) {
          Countdown = FALSE;
          A03sMenuDrawStatus (TRUE, 0);
        }

        if ((StableKeys & A03S_MENU_KEY_POWER) != 0) {
          return (A03S_MENU_ITEM)Selected;
        }

        if ((StableKeys & A03S_MENU_KEY_VOLUP) != 0) {
          Selected = (Selected + A03sMenuItemCount - 1) % A03sMenuItemCount;
          A03sMenuDrawItems (Selected);
        } else if ((StableKeys & A03S_MENU_KEY_VOLDOWN) != 0) {
          Selected = (Selected + 1) % A03sMenuItemCount;
          A03sMenuDrawItems (Selected);
        }
      }
    }

    //
    // Still honour ConIn for a USB keyboard or if the driver does work.
    //
    if (gST->ConIn != NULL) {
      while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
        if (Countdown) {
          Countdown = FALSE;
          A03sMenuDrawStatus (TRUE, 0);
        }

        if (A03sMenuHandleKey (&Key, &Selected)) {
          return (A03S_MENU_ITEM)Selected;
        }
      }
    }

    if (!Countdown) {
      gBS->Stall (20000);
      continue;
    }

    gBS->Stall (20000);
    ElapsedMs += 20;
    if (ElapsedMs >= (A03S_MENU_TIMEOUT_SECONDS * 1000)) {
      return A03sMenuStartXnu;
    }

    Seconds = (A03S_MENU_TIMEOUT_SECONDS * 1000 - ElapsedMs + 999) / 1000;
    if (Seconds != LastSeconds) {
      A03sMenuDrawStatus (FALSE, Seconds);
      LastSeconds = Seconds;
    }
  }
}

/**
  Boot the UEFI Shell explicitly.

  Returning to BDS is not enough: the Shell option is appended to BootOrder
  after all auto-enumerated options, so BDS would boot whichever option is
  first.  Find the Shell option by its description and start it directly.
**/
STATIC
VOID
A03sBootShell (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
  UINTN                         BootOptionCount;
  UINTN                         Index;
  BOOLEAN                       Found;

  BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);
  if ((BootOptions == NULL) || (BootOptionCount == 0)) {
    Print (L"No boot options are available\n");
    gBS->Stall (2000000);
    return;
  }

  Found = FALSE;
  for (Index = 0; Index < BootOptionCount; Index++) {
    if (StrCmp (BootOptions[Index].Description, L"UEFI Shell") == 0) {
      Found = TRUE;
      EfiBootManagerBoot (&BootOptions[Index]);
      break;
    }
  }

  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

  if (!Found) {
    Print (L"UEFI Shell boot option not found\n");
    gBS->Stall (2000000);
  }
}

/**
  Run the boot menu.  Returns only when the user selected the UEFI Shell;
  every other entry either starts something that does not return or resets
  the phone.
**/
STATIC
VOID
A03sBootMenu (
  VOID
  )
{
  A03S_MENU_ITEM                  Item;
  EFI_STATUS                      Status;
  EFI_BOOT_MANAGER_LOAD_OPTION    BootManagerMenu;

  for (;;) {
    Item = A03sMenuSelect ();

    if (gST->ConOut != NULL) {
      gST->ConOut->ClearScreen (gST->ConOut);
      gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);
    }

    switch (Item) {
      case A03sMenuStartXnu:
        XnuLoaderStart ();
        //
        // The loader only returns when it could not start XNU.  Redraw the
        // menu instead of falling into the Shell, so the user can try again.
        //
        break;

      case A03sMenuShell:
        A03sBootShell ();
        break;

      case A03sMenuBootManager:
        Status = EfiBootManagerGetBootManagerMenu (&BootManagerMenu);
        if (!EFI_ERROR (Status)) {
          EfiBootManagerBoot (&BootManagerMenu);
        } else {
          Print (L"Boot Manager is not available: %r\n", Status);
          gBS->Stall (2000000);
        }
        break;

      case A03sMenuReboot:
      default:
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
        CpuDeadLoop ();
    }
  }
}

/**
  Do the platform specific action after the console is ready
  Possible things that can be done in PlatformBootManagerAfterConsole:
  > Console post action:
    > Dynamically switch output mode from 100x31 to 80x25 for certain senarino
    > Signal console ready platform customized event
  > Run diagnostics like memory testing
  > Connect certain devices
  > Dispatch aditional option roms
  > Special boot: e.g.: USB boot, enter UI
**/
VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  ESRT_MANAGEMENT_PROTOCOL      *EsrtManagement;
  EFI_STATUS                    Status;

  //
  // LK leaves its Samsung logo (and whatever the previous UEFI session
  // left behind) in the scanout buffer.  Start from a clean screen before
  // the console or the boot logo draws anything.
  //
  if (gST->ConOut != NULL) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }

#if !A03S_BOOT_MENU
  //
  // Show the splash screen.  With the boot menu enabled the menu draws its
  // own HakiOS branding, so the logo would only flash for one frame.
  //
  Status = BootLogoEnableLogo ();
#endif

  //
  // Connect the rest of the devices.
  //
  EfiBootManagerConnectAll ();

  //
  // Register the Shell before the menu runs: A03sBootShell() finds the
  // option by its description and starts it directly.
  //
  EfiBootManagerRefreshAllBootOption ();
  PlatformRegisterOptionsAndKeys ();

#if A03S_BOOT_MENU
  //
  // Ask the user what to start.  This returns only for the UEFI Shell; the
  // XNU entry calls the loader below, and Reboot resets the phone.
  //
  A03sBootMenu ();
#else
  //
  // The terminal console is connected now: give XNU the machine.
  //
  XnuLoaderStart ();
#endif

  Status = gBS->LocateProtocol (&gEsrtManagementProtocolGuid, NULL,
                  (VOID **)&EsrtManagement);
  if (!EFI_ERROR (Status)) {
    EsrtManagement->SyncEsrtFmp ();
  }

  if (GetBootModeHob() == BOOT_ON_FLASH_UPDATE) {
    DEBUG((DEBUG_INFO, "ProcessCapsules After EndOfDxe ......\n"));
    Status = ProcessCapsules ();
    DEBUG((DEBUG_INFO, "ProcessCapsules returned %r\n", Status));
  }

#if A03S_BLANK_SCREEN_AFTER_SHELL
  //
  // The loader has returned and BDS is about to start the UEFI Shell.
  // Arm the timer that leaves the scanout buffer black for the next boot.
  //
  A03sScheduleScreenBlank ();
#endif
}

/**
  This function is called each second during the boot manager waits the
  timeout.

  @param TimeoutRemain  The remaining timeout.
**/
VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16          TimeoutRemain
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION Black;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION White;
  UINT16                              Timeout;
  EFI_STATUS                          Status;

  Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  Black.Raw = 0x00000000;
  White.Raw = 0x00FFFFFF;

  Status = BootLogoUpdateProgress (
             White.Pixel,
             Black.Pixel,
             L"Press Power Button for Setup Utility\n",
             White.Pixel,
             (Timeout - TimeoutRemain) * 100 / Timeout,
             0
             );
  if (EFI_ERROR (Status)) {
    Print (L".");
  }
}

/**
  The function is called when no boot option could be launched,
  including platform recovery options and options pointing to applications
  built into firmware volumes.

  If this function returns, BDS attempts to enter an infinite loop.
**/
VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  EFI_STATUS                   Status;
  EFI_INPUT_KEY                Key;
  EFI_BOOT_MANAGER_LOAD_OPTION BootManagerMenu;
  UINTN                        Index;

  //
  // BootManagerMenu doesn't contain the correct information when return status
  // is EFI_NOT_FOUND.
  //
  Status = EfiBootManagerGetBootManagerMenu (&BootManagerMenu);
  if (EFI_ERROR (Status)) {
    return;
  }
  //
  // Normally BdsDxe does not print anything to the system console, but this is
  // a last resort -- the end-user will likely not see any DEBUG messages
  // logged in this situation.
  //
  // AsciiPrint() will NULL-check gST->ConOut internally. We check gST->ConIn
  // here to see if it makes sense to request and wait for a keypress.
  //
  if (gST->ConIn != NULL) {
    AsciiPrint (
      "%a: No bootable option or device was found.\n"
      "%a: Press any key to enter the Boot Manager Menu.\n",
      gEfiCallerBaseName,
      gEfiCallerBaseName
      );
    Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    ASSERT_EFI_ERROR (Status);
    ASSERT (Index == 0);

    //
    // Drain any queued keys.
    //
    while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      //
      // just throw away Key
      //
    }
  }

  for (;;) {
    EfiBootManagerBoot (&BootManagerMenu);
  }
}
