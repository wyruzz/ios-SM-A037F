_Head:
	/* Set _Entry address */
	adr		x1, _Payload

	/* Jump to the real code */
	b		_Start

	/* Fake Linux kernel header */

_StackBase:
	/* Text Offset */
	.quad UEFI_BASE

_StackSize:
	/* Image Size */
	.quad UEFI_SIZE

	/* Flags */
	.quad 0

	/* Reserved */
	.quad 0

	/* Reserved */
	.quad 0

	/* Reserved */
	.quad 0

	/* ARM64 Magic */
	.ascii "ARM\x64"

	/* Reserved */
	.long 0

_Start:
	/*
	 * LK hands control to the kernel through MTK_SIP_KERNEL_BOOT, so we
	 * arrive at EL1 with the MMU off.  The MTK watchdog may still be armed
	 * (it is normally only petted by the Linux driver), and UEFI knows
	 * nothing about it; without this write the board resets a few seconds
	 * into the UEFI boot.  Same sequence as LK's mtk_wdt_disable():
	 * clear ENABLE and write the 0x22000000 key.
	 */
	mov		x7, #0x10000000
	add		x7, x7, #0x7000
	movz		w8, #0x2200, lsl #16
	str		w8, [x7]

	/*
	 * The boot image carries the XNU payload after the firmware volume.
	 * Copy it to XNU_PAYLOAD_BASE before UEFI starts, so the firmware's
	 * heap and stack (which overlap the source) cannot overwrite it.
	 *
	 * No literal pool here: it would land between this code and _Payload,
	 * and _Payload must be exactly the FD that follows the shim.
	 */
	adr		x9, _Payload
	movz		x10, #(UEFI_SIZE & 0xFFFF)
	movk		x10, #((UEFI_SIZE >> 16) & 0xFFFF), lsl #16
	movk		x10, #((UEFI_SIZE >> 32) & 0xFFFF), lsl #32
	movk		x10, #((UEFI_SIZE >> 48) & 0xFFFF), lsl #48
	add		x9, x9, x10
	movz		x11, #(XNU_PAYLOAD_BASE & 0xFFFF)
	movk		x11, #((XNU_PAYLOAD_BASE >> 16) & 0xFFFF), lsl #16
	movk		x11, #((XNU_PAYLOAD_BASE >> 32) & 0xFFFF), lsl #32
	movk		x11, #((XNU_PAYLOAD_BASE >> 48) & 0xFFFF), lsl #48
	movz		x12, #(PAYLOAD_SIZE & 0xFFFF)
	movk		x12, #((PAYLOAD_SIZE >> 16) & 0xFFFF), lsl #16
	movk		x12, #((PAYLOAD_SIZE >> 32) & 0xFFFF), lsl #32
	movk		x12, #((PAYLOAD_SIZE >> 48) & 0xFFFF), lsl #48
	cbz		x12, _NoPayload

_PayloadCopy:
	ldp		x13, x14, [x9], #16
	stp		x13, x14, [x11], #16
	subs	x12, x12, #16
	b.ne	_PayloadCopy
	b		_CopyUefi

_NoPayload:
	/* A warm reset keeps DRAM: make sure no stale payload is picked up. */
	str		wzr, [x11]

_CopyUefi:
	mov		x4, x1
	ldr		x5, _StackBase
	cmp		x4, x5
	beq		_Entry
	ldr		x6,  _StackSize

_CopyLoop:
	ldp		x2, x3, [x4], #16
	stp		x2, x3, [x5], #16
	subs	x6, x6, #16
	b.ne	_CopyLoop
	ldr		x5, _StackBase

_Entry:
	br		x5

_Dead:
	/* We should never get here */
	b		_Dead

.text
.align 4

_Payload:
	/* Your code will get ran right after this binary */
