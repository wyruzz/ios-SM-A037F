#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/HobLib.h>
#include <Library/SerialPortLib.h>

#include <Resources/font5x12.h>
#include <Resources/FbColor.h>

#include "FrameBufferSerialPortLib.h"

FBCON_POSITION m_Position;
FBCON_POSITION m_MaxPosition;
FBCON_COLOR m_Color;
BOOLEAN m_Initialized = FALSE;

UINTN gWidth = FixedPcdGet32(PcdMipiFrameBufferWidth);
// Reserve half screen for output
UINTN gHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
UINTN gBpp = FixedPcdGet32(PcdMipiFrameBufferPixelBpp);
// Row pitch in pixels.  It is not always the visible width: the A03s
// scans out 720 pixels per line but the line buffer is 736 pixels wide.
UINTN gStride = FixedPcdGet32(PcdMipiFrameBufferStride);

// Module-used internal routine
void FbConPutCharWithFactor
(
	char c,
	int type,
	unsigned scale_factor
);

void FbConDrawglyph
(
	char *pixels,
	unsigned stride,
	unsigned bpp,
	unsigned *glyph,
	unsigned scale_factor
);

void FbConReset(void);
void FbConScrollUp(void);
void FbConFlush(void);

RETURN_STATUS
EFIAPI
SerialPortInitialize
(
	VOID
)
{
	UINTN InterruptState = 0;

	// Prevent dup initialization
	if (m_Initialized) return RETURN_SUCCESS;

	// Interrupt Disable
	InterruptState = ArmGetInterruptState();
	ArmDisableInterrupts();

	// Reset console
	FbConReset();

	// Set flag
	m_Initialized = TRUE;

	if (InterruptState) ArmEnableInterrupts();
	return RETURN_SUCCESS;
}

void ResetFb(void)
{
	// Clear current screen.
	char* Base = (void*)FixedPcdGet32(PcdMipiFrameBufferAddress);
	UINTN BytesPerPixel = gBpp / 8;
	UINTN BgColor = FB_BGRA8888_BLACK;

	// Set to black color.  Clear a full pitched row at a time: the
	// padding pixels to the right of the visible area must be cleared
	// too, otherwise the panel shows stale garbage in the margin.
	for (UINTN i = 0; i < gHeight; i++)
	{
		char* Pixels = Base + i * gStride * BytesPerPixel;
		for (UINTN j = 0; j < gStride; j++)
		{
			BgColor = FB_BGRA8888_BLACK;
			// Set pixel bit
			for (UINTN p = 0; p < BytesPerPixel; p++)
			{
				*Pixels = (unsigned char)BgColor;
				BgColor = BgColor >> 8;
				Pixels++;
			}
		}
	}
}

void FbConReset(void)
{
	// Reset position.
	m_Position.x = 0;
	m_Position.y = 0;

	// Calc max position.
	m_MaxPosition.x = gWidth / (FONT_WIDTH + 1);
	m_MaxPosition.y = (gHeight - 1) / FONT_HEIGHT;

	// Reset color.
	m_Color.Foreground = FB_BGRA8888_WHITE;
	m_Color.Background = FB_BGRA8888_BLACK;
}

void FbConPutCharWithFactor
(
	char c,
	int type,
	unsigned scale_factor
)
{
	char* Pixels;

	if (!m_Initialized) return;

paint:

	if ((unsigned char)c > 127) return;

	if ((unsigned char)c < 32)
	{
		if (c == '\n')
		{
			goto newline;
		}
		else if (c == '\r')
		{
			m_Position.x = 0;
			return;
		}
		else
		{
			return;
		}
	}

	// Save some space
	if (m_Position.x == 0 && (unsigned char)c == ' ' &&
		type != FBCON_SUBTITLE_MSG &&
		type != FBCON_TITLE_MSG)
		return;

	BOOLEAN intstate = ArmGetInterruptState();
	ArmDisableInterrupts();

	Pixels = (void*)FixedPcdGet32(PcdMipiFrameBufferAddress);
	Pixels += m_Position.y * ((gBpp / 8) * FONT_HEIGHT * gStride);
	Pixels += m_Position.x * scale_factor * ((gBpp / 8) * (FONT_WIDTH + 1));

	FbConDrawglyph(
		Pixels,
		gStride,
		(gBpp / 8),
		font5x12 + (c - 32) * 2,
		scale_factor);

	m_Position.x++;

	if (m_Position.x >= (int)(m_MaxPosition.x / scale_factor)) goto newline;

	if (intstate) ArmEnableInterrupts();
	return;

newline:
	m_Position.y += scale_factor;
	m_Position.x = 0;
	if (m_Position.y >= m_MaxPosition.y - scale_factor)
	{
		ResetFb();
		FbConFlush();
		m_Position.y = 0;

		if (intstate) ArmEnableInterrupts();
		goto paint;
	}
	else
	{
		FbConFlush();
		if (intstate) ArmEnableInterrupts();
	}

}

void FbConDrawglyph
(
	char *pixels,
	unsigned stride,
	unsigned bpp,
	unsigned *glyph,
	unsigned scale_factor
)
{
	char *bg_pixels = pixels;
	unsigned x, y, i, j, k;
	unsigned data, temp;
	unsigned int fg_color = m_Color.Foreground;
	unsigned int bg_color = m_Color.Background;
	stride -= FONT_WIDTH * scale_factor;

	for (y = 0; y < FONT_HEIGHT / 2; ++y)
	{
		for (i = 0; i < scale_factor; i++)
		{
			for (x = 0; x < FONT_WIDTH; ++x)
			{
				for (j = 0; j < scale_factor; j++)
				{
					bg_color = m_Color.Background;
					for (k = 0; k < bpp; k++)
					{
						*bg_pixels = (unsigned char)bg_color;
						bg_color = bg_color >> 8;
						bg_pixels++;
					}
				}
			}
			bg_pixels += (stride * bpp);
		}
	}

	for (y = 0; y < FONT_HEIGHT / 2; ++y)
	{
		for (i = 0; i < scale_factor; i++)
		{
			for (x = 0; x < FONT_WIDTH; ++x)
			{
				for (j = 0; j < scale_factor; j++)
				{
					bg_color = m_Color.Background;
					for (k = 0; k < bpp; k++)
					{
						*bg_pixels = (unsigned char)bg_color;
						bg_color = bg_color >> 8;
						bg_pixels++;
					}
				}
			}
			bg_pixels += (stride * bpp);
		}
	}

	data = glyph[0];
	for (y = 0; y < FONT_HEIGHT / 2; ++y)
	{
		temp = data;
		for (i = 0; i < scale_factor; i++)
		{
			data = temp;
			for (x = 0; x < FONT_WIDTH; ++x)
			{
				if (data & 1)
				{
					for (j = 0; j < scale_factor; j++)
					{
						fg_color = m_Color.Foreground;
						for (k = 0; k < bpp; k++)
						{
							*pixels = (unsigned char)fg_color;
							fg_color = fg_color >> 8;
							pixels++;
						}
					}
				}
				else
				{
					for (j = 0; j < scale_factor; j++)
					{
						pixels = pixels + bpp;
					}
				}
				data >>= 1;
			}
			pixels += (stride * bpp);
		}
	}

	data = glyph[1];
	for (y = 0; y < FONT_HEIGHT / 2; ++y)
	{
		temp = data;
		for (i = 0; i < scale_factor; i++)
		{
			data = temp;
			for (x = 0; x < FONT_WIDTH; ++x)
			{
				if (data & 1)
				{
					for (j = 0; j < scale_factor; j++)
					{
						fg_color = m_Color.Foreground;
						for (k = 0; k < bpp; k++)
						{
							*pixels = (unsigned char)fg_color;
							fg_color = fg_color >> 8;
							pixels++;
						}
					}
				}
				else
				{
					for (j = 0; j < scale_factor; j++)
					{
						pixels = pixels + bpp;
					}
				}
				data >>= 1;
			}
			pixels += (stride * bpp);
		}
	}
}

void FbConScrollUp(void)
{
	unsigned char *dst = (void*)FixedPcdGet32(PcdMipiFrameBufferAddress);
	UINTN bytes_per_row = gStride * (gBpp / 8);
	unsigned char *src = dst + (bytes_per_row * FONT_HEIGHT);
	UINTN count = bytes_per_row * (gHeight - FONT_HEIGHT);

	// Shift whole pitched rows up: the padding bytes have to move with
	// the visible pixels or every subsequent row lands askew.
	while (count--)
	{
		*dst++ = *src++;
	}

	count = bytes_per_row * FONT_HEIGHT;
	while (count--)
	{
		*dst++ = 0;
	}

	FbConFlush();
}

void FbConFlush(void)
{
	unsigned total_x, total_y;
	unsigned bytes_per_bpp;

	total_x = gStride;
	total_y = gHeight;
	bytes_per_bpp = (gBpp / 8);

	WriteBackInvalidateDataCacheRange(
		(void*)FixedPcdGet32(PcdMipiFrameBufferAddress),
		(total_x * total_y * bytes_per_bpp)
	);
}

//
// TerminalDxe drives a VT100/VT-UTF8 state machine and emits colour,
// cursor-placement and erase sequences.  This panel has no terminal
// emulator, so drawing the bytes literally put "[1m[33m[40m" on screen.
// The parser below consumes the escapes and implements the handful of
// motions the firmware actually emits: clear screen (ESC[2J), absolute
// cursor placement (ESC[row;colH), erase line (ESC[K) and cursor
// forward/backward (ESC[nC / ESC[nD).  Everything else is swallowed.
//
#define FBCON_ANSI_MAX_PARAMS  4

STATIC UINTN   mAnsiState      = 0;
STATIC UINTN   mAnsiParam[FBCON_ANSI_MAX_PARAMS];
STATIC UINTN   mAnsiParamCount = 0;
STATIC UINTN   mAnsiValue      = 0;
STATIC BOOLEAN mAnsiHaveValue  = FALSE;

STATIC
VOID
FbConAnsiResetParams (
	VOID
	)
{
	mAnsiParamCount = 0;
	mAnsiValue      = 0;
	mAnsiHaveValue  = FALSE;
}

STATIC
VOID
FbConAnsiPushParam (
	VOID
	)
{
	if (mAnsiParamCount < FBCON_ANSI_MAX_PARAMS) {
		mAnsiParam[mAnsiParamCount] = mAnsiHaveValue ? mAnsiValue : 0;
		mAnsiParamCount++;
	}
	mAnsiValue     = 0;
	mAnsiHaveValue = FALSE;
}

STATIC
UINTN
FbConAnsiParam (
	IN UINTN Index,
	IN UINTN Default
	)
{
	if (Index >= mAnsiParamCount) {
		return Default;
	}
	return mAnsiParam[Index] == 0 ? Default : mAnsiParam[Index];
}

STATIC
UINTN
FbConColumns (
	VOID
	)
{
	UINTN Columns = m_MaxPosition.x / SCALE_FACTOR;
	return Columns ? Columns : 1;
}

STATIC
UINTN
FbConRows (
	VOID
	)
{
	UINTN Rows = m_MaxPosition.y / SCALE_FACTOR;
	return Rows ? Rows : 1;
}

//
// Set the cursor from 1-based terminal coordinates.  m_Position.x counts
// character cells, m_Position.y counts text lines in FONT_HEIGHT units
// (one line advances it by SCALE_FACTOR).
//
STATIC
VOID
FbConSetCursor (
	IN UINTN Column,
	IN UINTN Row
	)
{
	UINTN Columns = FbConColumns ();
	UINTN Rows    = FbConRows ();

	if (Row < 1)        Row = 1;
	if (Row > Rows)     Row = Rows;
	if (Column < 1)     Column = 1;
	if (Column > Columns) Column = Columns;

	m_Position.x = (INTN)(Column - 1);
	m_Position.y = (INTN)((Row - 1) * SCALE_FACTOR);
}

STATIC
VOID
FbConClearScreen (
	VOID
	)
{
	ResetFb ();
	FbConFlush ();
	m_Position.x = 0;
	m_Position.y = 0;
}

//
// Erase part of the current line: 0 = from the cursor to the end,
// 1 = from the start to the cursor, 2 = the whole line.
//
STATIC
VOID
FbConEraseLine (
	IN UINTN Mode
	)
{
	UINTN Columns = FbConColumns ();
	UINTN From    = 0;
	UINTN To      = Columns;

	if (m_Position.x < 0) {
		m_Position.x = 0;
	}
	if ((UINTN)m_Position.x >= Columns) {
		return;
	}

	if (Mode == 0) {
		From = (UINTN)m_Position.x;
	} else if (Mode == 1) {
		To = (UINTN)m_Position.x + 1;
	}

	if (From >= To) {
		return;
	}

	UINTN BytesPerPixel = gBpp / 8;
	UINTN PitchBytes    = gStride * BytesPerPixel;
	UINTN CellWidth     = (FONT_WIDTH + 1) * SCALE_FACTOR;
	UINTN CellHeight    = FONT_HEIGHT * SCALE_FACTOR;
	UINTN Top           = (UINTN)m_Position.y * FONT_HEIGHT;
	UINTN Left          = From * CellWidth;
	UINTN Width         = (To - From) * CellWidth;
	UINT8 *Base         = (UINT8*)FixedPcdGet32(PcdMipiFrameBufferAddress);
	UINTN x, y, k;

	if (Left >= gWidth) {
		return;
	}
	if (Left + Width > gWidth) {
		Width = gWidth - Left;
	}
	if (Top >= gHeight) {
		return;
	}
	if (Top + CellHeight > gHeight) {
		CellHeight = gHeight - Top;
	}

	for (y = 0; y < CellHeight; y++) {
		UINT8 *Pixel = Base + (Top + y) * PitchBytes + Left * BytesPerPixel;
		for (x = 0; x < Width; x++) {
			UINTN Color = FB_BGRA8888_BLACK;
			for (k = 0; k < BytesPerPixel; k++) {
				*Pixel++ = (UINT8)Color;
				Color >>= 8;
			}
		}
	}

	FbConFlush ();
}

STATIC
VOID
FbConAnsiDispatch (
	IN UINT8 Final
	)
{
	UINTN Columns = FbConColumns ();
	UINTN Amount;

	switch (Final) {
	case 'H':
	case 'f':
		FbConSetCursor (FbConAnsiParam (1, 1), FbConAnsiParam (0, 1));
		break;

	case 'J':
		if (FbConAnsiParam (0, 0) == 2) {
			FbConClearScreen ();
		}
		break;

	case 'K':
		FbConEraseLine (FbConAnsiParam (0, 0));
		break;

	case 'C':
		Amount = FbConAnsiParam (0, 1);
		m_Position.x += (INTN)Amount;
		if ((UINTN)m_Position.x >= Columns) {
			m_Position.x = (INTN)(Columns - 1);
		}
		break;

	case 'D':
		Amount = FbConAnsiParam (0, 1);
		if ((UINTN)Amount > (UINTN)m_Position.x) {
			m_Position.x = 0;
		} else {
			m_Position.x -= (INTN)Amount;
		}
		break;

	default:
		break;
	}
}

STATIC
BOOLEAN
FbConShouldDraw (
	IN UINT8 Byte
	)
{
	switch (mAnsiState) {
	case 0:
		if (Byte == 0x1B) {
			mAnsiState = 1;
			return FALSE;
		}
		return TRUE;

	case 1:
		switch (Byte) {
		case '[':
			FbConAnsiResetParams ();
			mAnsiState = 2;
			break;
		case ']':
			mAnsiState = 3;
			break;
		case '(': case ')': case '*': case '+':
		case '-': case '.': case '/': case '#':
		case '%':
			mAnsiState = 5;
			break;
		default:
			mAnsiState = 0;
			break;
		}
		return FALSE;

	case 2:
		// CSI: parameter bytes are digits and ';', the final byte is
		// in the 0x40..0x7E range.  Other parameter/intermediate
		// bytes ('?', '=', ...) are ignored.
		if ((Byte >= '0') && (Byte <= '9')) {
			mAnsiValue = mAnsiValue * 10 + (Byte - '0');
			mAnsiHaveValue = TRUE;
		} else if (Byte == ';') {
			FbConAnsiPushParam ();
		} else if ((Byte >= 0x40) && (Byte <= 0x7E)) {
			FbConAnsiPushParam ();
			FbConAnsiDispatch (Byte);
			FbConAnsiResetParams ();
			mAnsiState = 0;
		}
		return FALSE;

	case 3:
		// OSC: ends with BEL, or ST (ESC \).
		if (Byte == 0x07) {
			mAnsiState = 0;
		} else if (Byte == 0x1B) {
			mAnsiState = 4;
		}
		return FALSE;

	case 4:
		mAnsiState = (Byte == '\\') ? 0 : 3;
		return FALSE;

	case 5:
		// Two-byte escape (charset selection and friends).
		mAnsiState = 0;
		return FALSE;
	}

	mAnsiState = 0;
	return TRUE;
}

UINTN
EFIAPI
SerialPortWrite
(
	IN UINT8     *Buffer,
	IN UINTN     NumberOfBytes
)
{
	UINT8* CONST Final = &Buffer[NumberOfBytes];
	UINTN  InterruptState = ArmGetInterruptState();
	ArmDisableInterrupts();

	while (Buffer < Final)
	{
		UINT8 Byte = *Buffer++;
		if (FbConShouldDraw(Byte)) {
			FbConPutCharWithFactor(Byte, FBCON_COMMON_MSG, SCALE_FACTOR);
		}
	}

	if (InterruptState) ArmEnableInterrupts();
	return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortWriteCritical
(
	IN UINT8     *Buffer,
	IN UINTN     NumberOfBytes
)
{
	UINT8* CONST Final = &Buffer[NumberOfBytes];
	UINTN  CurrentForeground = m_Color.Foreground;
	UINTN  InterruptState = ArmGetInterruptState();

	ArmDisableInterrupts();
	m_Color.Foreground = FB_BGRA8888_YELLOW;

	while (Buffer < Final)
	{
		UINT8 Byte = *Buffer++;
		if (FbConShouldDraw(Byte)) {
			FbConPutCharWithFactor(Byte, FBCON_COMMON_MSG, SCALE_FACTOR);
		}
	}

	m_Color.Foreground = CurrentForeground;

	if (InterruptState) ArmEnableInterrupts();
	return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead
(
	OUT UINT8     *Buffer,
	IN  UINTN     NumberOfBytes
)
{
	return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll
(
	VOID
)
{
	return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl
(
	IN UINT32 Control
)
{
	return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl
(
	OUT UINT32 *Control
)
{
	return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes
(
	IN OUT UINT64             *BaudRate,
	IN OUT UINT32             *ReceiveFifoDepth,
	IN OUT UINT32             *Timeout,
	IN OUT EFI_PARITY_TYPE    *Parity,
	IN OUT UINT8              *DataBits,
	IN OUT EFI_STOP_BITS_TYPE *StopBits
)
{
	return RETURN_UNSUPPORTED;
}

UINTN SerialPortFlush(VOID)
{
	return 0;
}

VOID
EnableSynchronousSerialPortIO(VOID)
{
	// Already synchronous
}
