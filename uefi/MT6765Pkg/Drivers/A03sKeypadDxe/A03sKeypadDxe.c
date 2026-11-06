/** @file
  Keypad driver for the Samsung Galaxy A03s (SM-A037F, MediaTek MT6765).

  The phone has no USB keyboard and its volume keys are wired to the MediaTek
  keypad controller at 0x10010000.  LK already leaves that controller running
  (it uses KP_MEM1..KP_MEM5 to detect recovery/fastboot keys), so the same
  registers can be sampled from UEFI without touching the PMIC wrap.

  The power key is not part of the keypad matrix: it is a PMIC input, read
  through the PMIC wrapper at 0x1000D000 (MT6357_TOPSTATUS bit 1).  The wrapper
  is initialised by LK and left enabled.  If a read fails, power-key input is
  simply disabled; volume keys keep working.

  The driver publishes EFI_SIMPLE_TEXT_INPUT_PROTOCOL so that BDS, the UEFI
  Shell and UiApp can all use the keys, plus the project's
  gEFIDroidKeypadDeviceProtocolGuid marker so PlatformBootManagerLib adds the
  device path to ConIn.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>

//
// MediaTek keypad controller.  KP_MEM1..KP_MEM4 are active low: a zero bit
// means the key with that hardware code is pressed.
//
#define KP_BASE             0x10010000U
#define KP_STA              (KP_BASE + 0x00U)
#define KP_MEM1             (KP_BASE + 0x04U)
#define KP_MEM2             (KP_BASE + 0x08U)
#define KP_MEM3             (KP_BASE + 0x0CU)
#define KP_MEM4             (KP_BASE + 0x10U)
#define KP_MEM5             (KP_BASE + 0x14U)
#define KP_DEBOUNCE         (KP_BASE + 0x18U)
#define KP_SCAN_TIMING      (KP_BASE + 0x1CU)
#define KP_SEL              (KP_BASE + 0x20U)
#define KP_EN               (KP_BASE + 0x24U)
#define KP_MEM_COUNT        4U
#define KP_TOTAL_BITS       64U

//
// Hardware key codes used by the A03s keypad map.  LK's cust_key.h uses 17
// for volume up (the recovery key) and 0 for volume down (the factory key).
//
#define A03S_KEY_VOLUME_UP    17U
#define A03S_KEY_VOLUME_DOWN  0U

//
// PMIC wrapper (MT6357).  WACS2 is the command/data channel LK uses.
//
#define PWRAP_BASE          0x1000D000U
#define PWRAP_WACS2_CMD     (PWRAP_BASE + 0xC20U)
#define PWRAP_WACS2_RDATA   (PWRAP_BASE + 0xC24U)
#define PWRAP_WACS2_VLDCLR  (PWRAP_BASE + 0xC28U)
#define PWRAP_WACS_FSM_IDLE      0x0U
#define PWRAP_WACS_FSM_WFVLDCLR  0x6U
#define PWRAP_INIT_DONE2         0x00400000U
#define PWRAP_TIMEOUT            100000U

#define PMIC_TOPSTATUS       0x24U
#define PMIC_PWRKEY_BIT      0x0002U   // bit 1: 1 = released, 0 = pressed
#define PMIC_HOMEKEY_BIT     0x0008U   // bit 3: 1 = released, 0 = pressed

#define A03S_KEYPAD_QUEUE_SIZE  16U

#define A03S_KEYPAD_PRIVATE_SIGNATURE  SIGNATURE_32 ('A', '0', '3', 'K')

typedef struct {
  UINTN                                 Signature;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL        SimpleTextIn;
  EFI_EVENT                             TimerEvent;
  EFI_EVENT                             WaitForKey;
  EFI_INPUT_KEY                         Queue[A03S_KEYPAD_QUEUE_SIZE];
  UINTN                                 QueueHead;
  UINTN                                 QueueTail;

  //
  // Keypad debounce state.  Raw is the last sample, Stable is the debounced
  // state and Reported is the state at the last edge report.
  //
  UINT64                                Raw;
  UINT64                                Stable;
  UINT64                                Reported;
  UINT32                                StableCount;
  UINT32                                ColumnIndex;

  //
  // Power key debounce state.
  //
  BOOLEAN                               PowerRaw;
  BOOLEAN                               PowerStable;
  BOOLEAN                               PowerReported;
  UINT32                                PowerCount;
  UINT32                                PowerRetry;
  BOOLEAN                               PowerValid;

  //
  // PMIC home key (Volume Down on the A03s) debounce state.
  //
  BOOLEAN                               HomeRaw;
  BOOLEAN                               HomeStable;
  BOOLEAN                               HomeReported;
  UINT32                                HomeCount;

  //
  // Non-NULL marker installed for PlatformBootManagerLib.
  //
  UINT32                                KeypadProtocol;
} A03S_KEYPAD_PRIVATE;

#define A03S_KEYPAD_FROM_THIS(a) \
  CR (a, A03S_KEYPAD_PRIVATE, SimpleTextIn, A03S_KEYPAD_PRIVATE_SIGNATURE)

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} A03S_KEYPAD_DEVICE_PATH;
#pragma pack ()

STATIC CONST A03S_KEYPAD_DEVICE_PATH  mKeypadDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) }
    },
    {
      0xD7F58A0E, 0xBED2, 0x4B5A,
      { 0xBB, 0x43, 0x8A, 0xB2, 0x3D, 0xD0, 0xE2, 0xB0 }
    }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL) >> 8) }
  }
};

/**
  Read the raw keypad matrix.  Bit N corresponds to hardware key code N.

  Only KP_MEM1..KP_MEM4 are read: they cover hardware codes 0..63, which
  includes both volume keys (17 and 0).  KP_MEM5 holds codes 64..71 and does
  not fit in the 64-bit mask.

  @return  A 64-bit mask with a set bit for every key currently pressed.
**/
STATIC
UINT64
A03sKeypadReadRaw (
  VOID
  )
{
  UINT64  Bits;
  UINTN   Index;
  UINT16  Value;

  //
  // Read the matrix exactly as the working diagnostic did: no writes to
  // KP_SEL/KP_EN, just sample KP_MEM1..KP_MEM4.  The controller is left
  // running by the boot chain; touching it made the keys disappear.
  //
  // A single physical Volume Up press on this unit lights up a whole ghost
  // pattern of bits (1, 10, 19, 28, ...).  The caller must treat any
  // non-zero mask as ONE key event, not one event per bit.
  //
  Bits = 0;
  for (Index = 0; Index < KP_MEM_COUNT; Index++) {
    Value = MmioRead16 (KP_MEM1 + (Index * 4));
    Bits |= ((UINT64)(UINT16)~Value) << (Index * 16);
  }

  return Bits;
}

/**
  Read one 16-bit PMIC register through the WACS2 channel.

  The wrapper is left configured by LK.  This function only polls the FSM and
  issues a read command; it never resets or reconfigures the wrapper, so a
  failure leaves the PMIC exactly as it was.

  @param[in]  Address  PMIC register address.
  @param[out] Value    Register value.

  @retval TRUE   The register was read.
  @retval FALSE  The wrapper did not answer in time.
**/
STATIC
BOOLEAN
A03sPmicRead (
  IN  UINT16  Address,
  OUT UINT16  *Value
  )
{
  UINT32  Data;
  UINTN   Timeout;

  Timeout = PWRAP_TIMEOUT;
  do {
    Data = MmioRead32 (PWRAP_WACS2_RDATA);
    if ((Data & PWRAP_INIT_DONE2) == 0) {
      return FALSE;
    }
    if ((Data & 0x70000U) == (PWRAP_WACS_FSM_IDLE << 16)) {
      break;
    }
  } while (--Timeout != 0);
  if (Timeout == 0) {
    return FALSE;
  }

  MmioWrite32 (PWRAP_WACS2_CMD, ((UINT32)(Address >> 1) << 16));

  Timeout = PWRAP_TIMEOUT;
  do {
    Data = MmioRead32 (PWRAP_WACS2_RDATA);
    if ((Data & PWRAP_INIT_DONE2) == 0) {
      return FALSE;
    }
    if (((Data >> 16) & 0x7U) == PWRAP_WACS_FSM_WFVLDCLR) {
      break;
    }
  } while (--Timeout != 0);
  if (Timeout == 0) {
    return FALSE;
  }

  *Value = (UINT16)(Data & 0xFFFFU);
  MmioWrite32 (PWRAP_WACS2_VLDCLR, 1);

  return TRUE;
}

/**
  Return whether the PMIC power and home keys are currently down.

  On the A03s the PMIC "home" key is wired to the Volume Down button
  (DTB: mt6357keys/home -> linux,keycodes = 114 = KEY_VOLUMEDOWN).

  @param[out] PowerPressed  Receives the power-key state.
  @param[out] HomePressed   Receives the home/volume-down state.
  @param[out] Valid         Receives whether the PMIC answered.

  @retval TRUE   The state is valid.
  @retval FALSE  The PMIC could not be read.
**/
STATIC
BOOLEAN
A03sPmicKeysPressed (
  OUT BOOLEAN  *PowerPressed,
  OUT BOOLEAN  *HomePressed
  )
{
  UINT16  Status;

  *PowerPressed = FALSE;
  *HomePressed  = FALSE;
  if (!A03sPmicRead (PMIC_TOPSTATUS, &Status)) {
    return FALSE;
  }

  //
  // MT6357_TOPSTATUS bit 1 = power key, bit 3 = home key.
  // Both are 1 while released.
  //
  *PowerPressed = ((Status & PMIC_PWRKEY_BIT) == 0);
  *HomePressed  = ((Status & PMIC_HOMEKEY_BIT) == 0);
  return TRUE;
}

/**
  Add one key to the driver queue and wake anyone waiting on WaitForKey.

  @param[in] Private  The driver instance.
  @param[in] Key      The key to queue.
**/
STATIC
VOID
A03sKeypadEnqueue (
  IN A03S_KEYPAD_PRIVATE  *Private,
  IN CONST EFI_INPUT_KEY  *Key
  )
{
  UINTN  Next;

  Next = (Private->QueueTail + 1) % A03S_KEYPAD_QUEUE_SIZE;
  if (Next == Private->QueueHead) {
    return;
  }

  Private->Queue[Private->QueueTail] = *Key;
  Private->QueueTail                 = Next;
  gBS->SignalEvent (Private->WaitForKey);
}

/**
  Translate a hardware key code to an EFI key and queue it.

  Volume up/down are the two keys the A03s boot menu needs.  The power key is
  delivered as Enter.  Any other key from the matrix is treated as "down" so
  the menu remains usable if a particular unit maps the volume keys to
  different hardware codes.

  @param[in] Private  The driver instance.
  @param[in] Code     Hardware key code (0..71).
**/
STATIC
VOID
A03sKeypadReportCode (
  IN A03S_KEYPAD_PRIVATE  *Private,
  IN UINTN                Code
  )
{
  EFI_INPUT_KEY  Key;

  Key.ScanCode    = SCAN_NULL;
  Key.UnicodeChar = 0;

  if (Code == A03S_KEY_VOLUME_UP) {
    Key.ScanCode = SCAN_UP;
  } else if (Code == A03S_KEY_VOLUME_DOWN) {
    Key.ScanCode = SCAN_DOWN;
  } else {
    //
    // Unknown matrix key: use it as "next item" rather than dropping it.
    //
    Key.ScanCode = SCAN_DOWN;
  }

  A03sKeypadEnqueue (Private, &Key);
}

/**
  Report the power key as Enter.

  @param[in] Private  The driver instance.
**/
STATIC
VOID
A03sKeypadReportPower (
  IN A03S_KEYPAD_PRIVATE  *Private
  )
{
  EFI_INPUT_KEY  Key;

  Key.ScanCode    = SCAN_NULL;
  Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
  A03sKeypadEnqueue (Private, &Key);
}

/**
  Report the PMIC home key (Volume Down on the A03s) as Down.

  @param[in] Private  The driver instance.
**/
STATIC
VOID
A03sKeypadReportHome (
  IN A03S_KEYPAD_PRIVATE  *Private
  )
{
  EFI_INPUT_KEY  Key;

  Key.ScanCode    = SCAN_DOWN;
  Key.UnicodeChar = 0;
  A03sKeypadEnqueue (Private, &Key);
}

/**
  Sample the keypad and PMIC, debounce the results and queue new presses.

  This runs from a 10 ms periodic timer.  A key must be stable for two
  samples before it is accepted.  A key that is already down when the driver
  starts is recorded as "reported" and is ignored until it is released and
  pressed again, so holding power to switch the phone on cannot select a menu
  entry by itself.

  @param[in] Private  The driver instance.
**/
STATIC
VOID
A03sKeypadPoll (
  IN A03S_KEYPAD_PRIVATE  *Private
  )
{
  UINT64   Current;
  UINT64   NewPresses;
  BOOLEAN  PowerPressed;
  BOOLEAN  HomePressed;
  BOOLEAN  PmicValid;

  Current = A03sKeypadReadRaw ();
  if (Current == Private->Raw) {
    if (Private->StableCount < 4) {
      Private->StableCount++;
    }
  } else {
    Private->Raw         = Current;
    Private->StableCount = 0;
  }

  if (Private->StableCount >= 2) {
    Private->Stable = Private->Raw;
    NewPresses      = Private->Stable & ~Private->Reported;
    if (NewPresses != 0) {
      //
      // On the A03s the keypad matrix carries Volume Up (Volume Down is a
      // PMIC home-key input).  One physical press lights up a ghost pattern
      // of several bits, so report exactly ONE event per press edge.
      // Reporting one event per bit made eight "down" presses at once,
      // which cycled the four-item menu back to where it started.
      //
      A03sKeypadReportCode (Private, A03S_KEY_VOLUME_UP);
    }
    Private->Reported = Private->Stable;
  }

  if (Private->PowerValid) {
    PmicValid = A03sPmicKeysPressed (&PowerPressed, &HomePressed);
    if (PmicValid) {
      //
      // Power key debounce.
      //
      if (PowerPressed == Private->PowerRaw) {
        if (Private->PowerCount < 4) {
          Private->PowerCount++;
        }
      } else {
        Private->PowerRaw   = PowerPressed;
        Private->PowerCount = 0;
      }

      if (Private->PowerCount >= 2) {
        Private->PowerStable = Private->PowerRaw;
        if (Private->PowerStable && !Private->PowerReported) {
          A03sKeypadReportPower (Private);
        }
        Private->PowerReported = Private->PowerStable;
      }

      //
      // Home key (Volume Down) debounce.
      //
      if (HomePressed == Private->HomeRaw) {
        if (Private->HomeCount < 4) {
          Private->HomeCount++;
        }
      } else {
        Private->HomeRaw   = HomePressed;
        Private->HomeCount = 0;
      }

      if (Private->HomeCount >= 2) {
        Private->HomeStable = Private->HomeRaw;
        if (Private->HomeStable && !Private->HomeReported) {
          A03sKeypadReportHome (Private);
        }
        Private->HomeReported = Private->HomeStable;
      }
    }
  } else {
    //
    // The PMIC wrapper was busy when the driver started.  Retry once a
    // second: if it answers now, take the current key state as the baseline
    // so a key already held is not reported as a fresh press.
    //
    if (++Private->PowerRetry >= 100) {
      Private->PowerRetry = 0;
      if (A03sPmicKeysPressed (&PowerPressed, &HomePressed)) {
        Private->PowerValid    = TRUE;
        Private->PowerRaw      = PowerPressed;
        Private->PowerStable   = PowerPressed;
        Private->PowerReported = PowerPressed;
        Private->PowerCount    = 0;
        Private->HomeRaw       = HomePressed;
        Private->HomeStable    = HomePressed;
        Private->HomeReported  = HomePressed;
        Private->HomeCount     = 0;
      }
    }
  }
}

/**
  Timer callback.
**/
STATIC
VOID
EFIAPI
A03sKeypadTimerNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A03sKeypadPoll ((A03S_KEYPAD_PRIVATE *)Context);
}

/**
  Event notification for WaitForKey.  Signals the event when a key is queued.
**/
STATIC
VOID
EFIAPI
A03sKeypadWaitForKeyNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A03S_KEYPAD_PRIVATE  *Private;

  Private = (A03S_KEYPAD_PRIVATE *)Context;
  if (Private->QueueHead != Private->QueueTail) {
    gBS->SignalEvent (Event);
  }
}

/**
  Reset the input device.
**/
STATIC
EFI_STATUS
EFIAPI
A03sKeypadReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  A03S_KEYPAD_PRIVATE  *Private;
  BOOLEAN              PowerPressed;
  BOOLEAN              HomePressed;

  Private = A03S_KEYPAD_FROM_THIS (This);

  Private->QueueHead   = 0;
  Private->QueueTail   = 0;
  Private->Raw         = A03sKeypadReadRaw ();
  Private->Stable      = Private->Raw;
  Private->Reported    = Private->Raw;
  Private->StableCount = 0;

  if (Private->PowerValid && A03sPmicKeysPressed (&PowerPressed, &HomePressed)) {
    Private->PowerRaw      = PowerPressed;
    Private->PowerStable   = PowerPressed;
    Private->PowerReported = PowerPressed;
    Private->PowerCount    = 0;
    Private->HomeRaw       = HomePressed;
    Private->HomeStable    = HomePressed;
    Private->HomeReported  = HomePressed;
    Private->HomeCount     = 0;
  }

  return EFI_SUCCESS;
}

/**
  Read one key from the queue.
**/
STATIC
EFI_STATUS
EFIAPI
A03sKeypadReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  A03S_KEYPAD_PRIVATE  *Private;

  Private = A03S_KEYPAD_FROM_THIS (This);

  if (Private->QueueHead == Private->QueueTail) {
    return EFI_NOT_READY;
  }

  *Key              = Private->Queue[Private->QueueHead];
  Private->QueueHead = (Private->QueueHead + 1) % A03S_KEYPAD_QUEUE_SIZE;

  return EFI_SUCCESS;
}

/**
  Entry point.
**/
EFI_STATUS
EFIAPI
A03sKeypadInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  A03S_KEYPAD_PRIVATE  *Private;
  A03S_KEYPAD_DEVICE_PATH  *DevicePath;
  BOOLEAN              PowerPressed;
  BOOLEAN              HomePressed;

  Private = AllocateZeroPool (sizeof (A03S_KEYPAD_PRIVATE));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Private->Signature                      = A03S_KEYPAD_PRIVATE_SIGNATURE;
  Private->SimpleTextIn.Reset             = A03sKeypadReset;
  Private->SimpleTextIn.ReadKeyStroke     = A03sKeypadReadKeyStroke;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  A03sKeypadWaitForKeyNotify,
                  Private,
                  &Private->WaitForKey
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Private);
    return Status;
  }

  Private->SimpleTextIn.WaitForKey = Private->WaitForKey;

  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_NOTIFY,
                  A03sKeypadTimerNotify,
                  Private,
                  &Private->TimerEvent
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Private->WaitForKey);
    FreePool (Private);
    return Status;
  }

  //
  // Initial state: a key already held at boot (the power key used to switch
  // the phone on, for example) must be released before it can be reported.
  //
  Private->Raw         = A03sKeypadReadRaw ();
  Private->Stable      = Private->Raw;
  Private->Reported    = Private->Raw;
  Private->StableCount = 0;

  Private->PowerValid = A03sPmicKeysPressed (&PowerPressed, &HomePressed);
  if (Private->PowerValid) {
    Private->PowerRaw      = PowerPressed;
    Private->PowerStable   = PowerPressed;
    Private->PowerReported = PowerPressed;
    Private->PowerCount    = 0;
    Private->HomeRaw       = HomePressed;
    Private->HomeStable    = HomePressed;
    Private->HomeReported  = HomePressed;
    Private->HomeCount     = 0;
  }

  Status = gBS->SetTimer (Private->TimerEvent, TimerPeriodic, 100000);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Private->TimerEvent);
    gBS->CloseEvent (Private->WaitForKey);
    FreePool (Private);
    return Status;
  }

  DevicePath = AllocateCopyPool (sizeof (mKeypadDevicePath), &mKeypadDevicePath);
  if (DevicePath == NULL) {
    gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
    gBS->CloseEvent (Private->TimerEvent);
    gBS->CloseEvent (Private->WaitForKey);
    FreePool (Private);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Install on a fresh handle.  Using ImageHandle can fail because the DXE
  // image handle may already carry a device path, and a failed install means
  // BDS never sees any keypad input at all.
  //
  {
    EFI_HANDLE  KeypadHandle;

    KeypadHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &KeypadHandle,
                    &gEfiDevicePathProtocolGuid,
                    DevicePath,
                    &gEfiSimpleTextInProtocolGuid,
                    &Private->SimpleTextIn,
                    &gEFIDroidKeypadDeviceProtocolGuid,
                    &Private->KeypadProtocol,
                    NULL
                    );
  }
  if (EFI_ERROR (Status)) {
    gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
    gBS->CloseEvent (Private->TimerEvent);
    gBS->CloseEvent (Private->WaitForKey);
    FreePool (DevicePath);
    FreePool (Private);
  }

  return Status;
}
