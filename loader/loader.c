/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

/*
 * The payload area must be RAM that the firmware has actually mapped, and it
 * must stay outside the window XNU will be given.  The A03s low DRAM bank
 * ends at 0x9FF00000 (the top 1.5 GB lives above 4 GB), so the upstream
 * 0xF0000000 address is a hole on this board.  Place it at 0x60000000,
 * between the UEFI image at 0x50000000 and the display at 0x7C250000.
 */
#define PAYLOAD_BASE        0x60000000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

/*
 * The payload carries the kernelcache compressed as `kernel.lz4`.  It is
 * expanded once, here, 32 MB above the container so a payload of up to
 * 32 MB cannot overlap its own output.  The whole 128 MB area is reserved
 * in the firmware memory map and is never part of XNU's RAM window.
 */
#define KERNEL_DECOMP_BASE  0x62000000ULL
#define LZ4_LEGACY_MAGIC    0x184C2102u
#define LZ4_LEGACY_BLOCK    0x00800000u           /* `lz4 -l` block size */

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x08000000ULL         /* RAM reserved for the payload in the UEFI memory map */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[262144];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;
/* boot-arg cycle=N: after N seconds, ask for recovery and reset (the host takes it from there) */
static uint64_t cycle_seconds;
#define MISC_LBA 406528u        /* the misc partition of this phone (mmcblk0p21) */
/*
 * Console-only output: the text goes to the screen but not into log_buf.  The
 * kernel's own log is written to the partition on its own, so echoing it into
 * log_buf would only fill that buffer and silence the loader's later messages.
 */
static int screen_only;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (!screen_only && log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	/* Prior stores must reach the cache before it is cleaned for another core. */
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	/* Prior stores must reach the cache before it is cleaned for another core. */
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

#define LOG_MAX_BYTES   (8u << 20)      /* out_buf below */
#define LOG_BOOT_OFFSET (24ULL << 20)   /* inside the 32 MiB boot partition, past the image */

static efi_block_io *log_bio;
static int log_emmc;
static uint64_t log_lba;
/* the eMMC backend, used for the log target below (defined further down) */
static int emmc_unit;
static int emmc_read_blocks(uint32_t lba, uint32_t count, uint8_t *buf);
static int emmc_write_blocks(uint32_t lba, uint32_t count, const uint8_t *buf);
static const char *log_target = "none";
static const char *log_how = "none";
static uint64_t log_len_written;
static efi_status log_ws;
static efi_status log_rs;
static int log_same;
static int log_write_tried;

static int
part_name_is(const char16 *name, const char16 *want)
{
	unsigned i = 0;

	for (; i < 36 && want[i] != 0; i++) {
		if (name[i] != want[i]) {
			return 0;
		}
	}
	return i == 36 || name[i] == 0;
}

/* The raw eMMC: the block device that is not a partition of another. */
static efi_block_io *
largest_disk(void)
{
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;
	efi_block_io *best = NULL;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_block_io *bio;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media == NULL) {
			continue;
		}
		/* the whole disk, in 512-byte units; partitions are logical and smaller */
		if (bio->media->logical || bio->media->block_size != 512) {
			continue;
		}
		if (best == NULL || bio->media->last_block > best->media->last_block) {
			best = bio;
		}
	}
	return best;
}

/* Find a GPT entry by name and return its first and last LBA. */
static int
gpt_find(efi_block_io *disk, const char16 *want, uint64_t *first, uint64_t *last)
{
	static uint8_t table[128 * 128] __attribute__((aligned(8)));
	uint8_t header[512];
	uint64_t bs = disk->media->block_size, entries_lba, count, size, bytes;

	if (bs != 512 || EFI_ERROR(disk->read_blocks(disk, disk->media->media_id, 1, 1, header))) {
		return 0;
	}
	if (header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
	    header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T') {
		return 0;
	}
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	bytes = count * size;
	if (count == 0 || count > 128 || size != 128 || bytes > sizeof(table)) {
		return 0;
	}
	if (EFI_ERROR(disk->read_blocks(disk, disk->media->media_id, entries_lba, bytes / bs, table))) {
		return 0;
	}
	for (uint64_t i = 0; i < count; i++) {
		uint8_t *e = table + i * size;
		int used = 0;

		for (int k = 0; k < 16; k++) {
			used |= e[k];
		}
		if (!used) {
			continue;
		}
		if (part_name_is((const char16 *)(e + 56), want)) {
			*first = *(uint64_t *)(e + 32);
			*last = *(uint64_t *)(e + 40);
			return 1;
		}
	}
	return 0;
}

static int
log_target_set(efi_block_io *bio, uint64_t first, uint64_t last, uint64_t off, const char *name, const char *how)
{
	uint64_t bs = bio->media->block_size;
	uint64_t off_blocks = (off + bs - 1) / bs;
	uint64_t need = (LOG_MAX_BYTES + bs - 1) / bs;

	if (bs == 0 || off_blocks + need > last - first + 1) {
		return 0;
	}
	log_bio = bio;
	log_lba = first + off_blocks;
	log_target = name;
	log_how = how;
	return 1;
}

/*
 * The A03s has no "logdump" partition.  The boot partition has 32 MiB and the
 * image uses only the first 7 MiB, so its tail is the safest place: the
 * bootloader reads the image size from the header and never looks there, and
 * unlike expdb it is not rewritten by LK on every boot.  expdb is the fallback
 * when boot cannot be found; logdump remains for QEMU and other phones.
 */
static int
find_log_target(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const struct {
		const char16 *name;
		uint64_t off;
		const char *ascii;
	} want[] = {
		{ u"boot", LOG_BOOT_OFFSET, "boot tail" },
		{ u"expdb", 0, "expdb" },
		{ LOG_PARTITION, 0, "logdump" },
	};
	efi_handle *handles;
	uint64_t count;

	if (log_bio != NULL) {
		return 1;
	}
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		for (unsigned w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
			for (uint64_t i = 0; i < count; i++) {
				efi_partition_info *info;
				efi_block_io *bio;

				if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) ||
				    info->type != 2) {
					continue;
				}
				if (!part_name_is(info->name, want[w].name) ||
				    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
					continue;
				}
				if (log_target_set(bio, 0, bio->media->last_block, want[w].off, want[w].ascii,
				    "partition table")) {
					print("log: target %s via the partition table\n", want[w].ascii);
					return 1;
				}
			}
		}
	}

	efi_block_io *disk = largest_disk();
	if (disk != NULL) {
		for (unsigned w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
			uint64_t first, last;

			if (!gpt_find(disk, want[w].name, &first, &last)) {
				continue;
			}
			if (log_target_set(disk, first, last, want[w].off, want[w].ascii, "GPT scan")) {
				print("log: target %s via the GPT itself\n", want[w].ascii);
				return 1;
			}
		}
	}
	/*
	 * A03s eMMC: no UEFI block_io for the partitions; write through the
	 * controller the loader already uses for the storage.  The boot
	 * partition is 65536 blocks at LBA 1167568; the log goes to its tail.
	 */
	if (emmc_unit >= 0) {
		log_emmc = 1;
		log_bio = NULL;
		log_lba = 1167568 + ((24ULL << 20) / 512);
		log_target = "boot tail (emmc)";
		log_how = "A03s PIT";
		print("log: target boot tail at lba %x (A03s emmc)\n", log_lba);
		return 1;
	}
	print("log: no log partition found (no boot/expdb entry)\n");
	return 0;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
static char out_buf[LOG_MAX_BYTES];
/* the storage's reads for the kernel land here (block_poll); also where the log is read back */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;
static int log_full_live;               /* out_buf already holds the whole kernel log */
static int log_skip_readback;           /* a live flush: written, not read back (the card is the kernel's to use) */

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

static void
write_logs(const char *klog, uint64_t klog_len)
{
	efi_block_io *bio;

	if (log_full_live) {
		/* compose_live_log() has already built the whole log in out_buf */
		log_full_live = 0;
		staged_data = out_buf;
	} else {
		compose_logs(klog, klog_len);
	}
	log_write_tried = 0;
	if (!find_log_target()) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len; i++) {
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		return;
	}
	if (log_emmc) {
		uint64_t elen = (staged_len + 511) & ~511ULL;
		int err;

		if (elen > sizeof(out_buf)) {
			elen = sizeof(out_buf);
		}
		err = emmc_write_blocks((uint32_t)log_lba, (uint32_t)(elen / 512),
		    (const uint8_t *)out_buf);
		int same = 0, rs = 0;
		if (!log_skip_readback && err == 0 && elen <= sizeof(block_buffer)) {
			/* the card may still be programming: give it time, then re-read */
			for (int attempt = 0; attempt < 4; attempt++) {
				rs = emmc_read_blocks((uint32_t)log_lba, (uint32_t)(elen / 512), block_buffer);
				if (rs == 0) {
					break;
				}
				for (volatile uint64_t i = 0; i < 20000000; i++) {
				}
			}
			same = rs == 0 && memcmp(block_buffer, out_buf, (size_t)elen) == 0;
		}
		log_write_tried = 1;
		log_len_written = elen;
		log_ws = err ? 1 : 0;
		log_same = log_skip_readback ? 1 : same;
		if (err != 0) {
			print("log: %d bytes NOT written to %s at lba %x (%d)\n", elen, log_target,
			    log_lba, (uint64_t)err);
		} else if (log_skip_readback) {
			print("log: %d bytes written to %s at lba %x (live, not read back)\n", elen,
			    log_target, log_lba);
		} else if (rs != 0) {
			print("log: %d bytes written to %s at lba %x, read back FAILED (%d)\n", elen,
			    log_target, log_lba, (uint64_t)rs);
		} else {
			print("log: %d bytes written to %s at lba %x (0), read back %s\n", elen,
			    log_target, log_lba, same ? "the same" : "DIFFERENT");
		}
		compose_logs(klog, klog_len);
		return;
	}
	bio = log_bio;
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	efi_status ws = 0, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to %s: UEFI's transfer list is not the one in the controller\n",
			    log_target);
		}
		compose_logs(klog, klog_len);
		return;
	}
	log_write_tried = 1;
	log_len_written = len;
	ws = bio->write_blocks(bio, bio->media->media_id, log_lba, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(block_buffer)) {
			rs = bio->read_blocks(bio, bio->media->media_id, log_lba, len, block_buffer);
			same = !EFI_ERROR(rs) && memcmp(block_buffer, out_buf, len) == 0;
		}
	}
	log_ws = ws;
	log_rs = rs;
	log_same = same;
	print("log: %d bytes written to %s at lba %x (%x), read back %s (%x)\n", len, log_target, log_lba, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
}

/* The last thing on the screen: where this run's log can be found on the phone. */
static void
print_log_summary(void)
{
	if ((log_bio == NULL && !log_emmc) || !log_write_tried) {
		print("\nLOG: build execdbg1: no persistent log on this phone; the window above is from RAM\n");
		return;
	}
	print("\nLOG: build execdbg1: %s via %s, lba %x, %d bytes, write %x, read %s\n", log_target, log_how,
	    log_lba, log_len_written, (uint64_t)log_ws,
	    EFI_ERROR(log_ws) ? "not tried" : log_same ? "same" : "DIFFERENT");
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/*
 * The kernel's log, or its last part, on the loader's own screen.  A phone has no serial console
 * and the UEFI shell clears the screen when this loader returns, so without this the only record
 * of a run that ends is the log partition (which needs a host to read it).
 */
static int
log_last_off(const char *log, uint64_t len, const char *needle, uint64_t *off)
{
	uint64_t n = 0;
	int found = 0;

	while (needle[n] != 0) {
		n++;
	}
	if (n == 0 || len < n) {
		return 0;
	}
	for (uint64_t i = 0; i + n <= len; i++) {
		if (log[i] != needle[0]) {
			continue;
		}
		uint64_t j = 1;
		while (j < n && log[i + j] == needle[j]) {
			j++;
		}
		if (j == n) {
			*off = i;
			found = 1;
		}
	}
	return found;
}

static void
print_log_line_at(const char *log, uint64_t len, uint64_t off)
{
	uint64_t start = off, end = off;

	while (start > 0 && log[start - 1] != '\n') {
		start--;
	}
	while (end < len && log[end] != '\n') {
		end++;
	}
	print("  ");
	for (uint64_t i = start; i < end && i - start < 220; i++) {
		char c = log[i];

		if (c == '\t' || (c >= 0x20 && c < 0x7f)) {
			put_char(c);
		}
	}
	print("\n");
}

/*
 * The last 6000 bytes of a panic are mostly the stackshot, and the line that says
 * why the kernel stopped can scroll off the screen.  Say the few lines that matter
 * again, at the end, where they can be photographed.
 */
static void
print_kernel_log_keys(const char *klog, uint64_t klog_len)
{
	static const char *keys[] = {
		"BSD root:",
		"apfs: volume",
		"IOKitBSDInit: done",
		"calling setconf",
		"vfs_mountroot",
		"root device",
		"the iOS root volume",
		"is the root now",
		"switching the root",
		"the OS cryptex",
		"the App cryptex",
		"applications waiting",
		"ignition",
		"ignite()",
		"shared cache",
		"Library not loaded",
		"panic(",
		"Debugger message",
		"Process 1 exec",
		"Stackshot Succeeded",
		"unable to create a validated cs_blob",
		"validation of code signature failed through MACF",
		"disallowing unsupported code signature page shift",
		"load code signature error",
		"dyld cache",
		"HAKIOS-CACHE-FAIL",
		"HAKIOS-CACHE:",
		"HAKIOS-CACHE-MAIN",
	};
	uint64_t off;

	print("\n== KEY LINES ==\n");
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (log_last_off(klog, klog_len, keys[i], &off)) {
			print_log_line_at(klog, klog_len, off);
		} else {
			print("  [%s] not found\n", keys[i]);
		}
	}
}

static void
print_kernel_log_tail(const char *klog, uint64_t klog_len, uint64_t want)
{
	if (klog == NULL || klog_len == 0) {
		return;
	}
	/*
	 * The panic buffer is appended to the log after this marker, and it ends with the
	 * stackshot (kcdata, binary): printed as text it is the rubbish that used to hide
	 * the last lines of the boot.  Leave it out; the console log before it has all.
	 */
	uint64_t marker;
	if (log_last_off(klog, klog_len, "--- panic buffer ---", &marker)) {
		while (marker > 0 && klog[marker - 1] != '\n') {
			marker--;
		}
		klog_len = marker;
	}
	uint64_t start = klog_len > want ? klog_len - want : 0;

	/*
	 * The keys first, the log last: the screen keeps only the last few
	 * lines, and the end of the kernel log is where init says what it did.
	 */
	print_kernel_log_keys(klog, klog_len);
	print("\n== KERNEL LOG: last %d of %d bytes ==\n", klog_len - start, klog_len);
	for (uint64_t i = start; i < klog_len; i++) {
		char c = klog[i];

		if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
			put_char(c);
		}
	}
	print("\n== END OF KERNEL LOG ==\n");
}

/*
 * The kernel now runs until loader_return_after, not until init halts, so the
 * last bytes of the log are the periodic memory statistics, not init's answer.
 * Print the lines around the last thing init said, at the very end of the
 * screen, where the phone can show them without a serial port or a log
 * partition.
 */
static void
print_init_window(const char *klog, uint64_t klog_len)
{
	static const char *keys[] = {
		"=== init: done", "checks,", "hello_dyn", "dynamic linking",
		"HAKIOS-CACHE-MAIN", "HAKIOS-CACHE:", "panic(", "was killed by signal",
		"EXECDBG", "stage0:", "Process 1 exec", "init:",
	};
	const char *best_key = NULL;
	uint64_t best = 0;

	if (klog == NULL || klog_len == 0) {
		return;
	}
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		uint64_t off;

		if (log_last_off(klog, klog_len, keys[i], &off) && (best_key == NULL || off > best)) {
			best = off;
			best_key = keys[i];
		}
	}
	if (best_key == NULL) {
		print("\n== INIT WINDOW: no init lines in the kernel log ==\n");
		return;
	}

	/* a dozen lines before the marker: enough context, and it fits the screen */
	uint64_t start = best;
	unsigned lines = 0;
	while (start > 0 && lines < 12) {
		start--;
		if (klog[start] == '\n') {
			lines++;
		}
	}
	if (start > 0) {
		start++;
	}

	/* and two lines after it, so the marker is not the last thing cut off */
	uint64_t end = best;
	lines = 0;
	while (end < klog_len && lines < 2) {
		if (klog[end] == '\n') {
			lines++;
		}
		end++;
	}

	print("\n== INIT WINDOW (last marker: %s) ==\n", best_key);
	for (uint64_t i = start; i < end; i++) {
		char c = klog[i];

		if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
			put_char(c);
		}
	}
	print("\n== END INIT WINDOW ==\n");
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it iOS takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	int cluster_cores;               /* 0: one cluster of .cores, numbered in .core_shift; otherwise this many per cluster */
	int clusters;                    /* with .cluster_cores: this many clusters, the second at MPIDR bit 8 */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
};

/*
 * Samsung Galaxy A03s (SM-A037F), MediaTek MT6765.  Values from the phone's
 * stock device tree:
 *   GICv3 distributor 0x0C000000, redistributor 0x0C100000
 *   CPUs: 8 Cortex-A53, MPIDR 0x000..0x003 and 0x100..0x103
 *   no PL011 (the SoC UART is MediaTek's, not modelled here)
 *   framebuffer handed over by LK at 0x7C250000
 *
 * MPIDRs are two-level on this SoC (Aff0 = core, Aff1 = cluster), while the
 * loader's device tree builder takes a single shift.  Start with the first
 * cluster; the second one needs a proper MPIDR table when SMP is brought up.
 */
static const struct platform platform_a03s = {
	.name = "a03s", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x0C000000, .gic_redist = 0x0C100000, .uart = 0,
	.cores = 4, .core_shift = 0, .cluster_cores = 4, .clusters = 2,
	.small_cores = 0,
	.fb_base = 0x7C250000,
	/* keep the log partition current while the kernel runs (see flush_live_log) */
	.log_other_cpu = 1,
};

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (common/new-device.sh writes a starting point, docs/PORTING.md says
 * where each value comes from); a display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_a03s,
};

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults.  A53 (and
         * every other ARMv8.0 core) does not implement PAN/UAO: writing them is
         * an undefined instruction, so only touch them when the core has them. */
        "  mrs x9, id_aa64mmfr1_el1\n"
        "  tbz x9, #20, 8f\n"           /* PAN implemented? */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "8:\n"
        "  tbz x9, #0, 9f\n"            /* UAO implemented? */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "9:\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and parks in the loader.  The next run wakes it with a doorbell instead */
/* of PSCI, which this phone's firmware does not handle after a CPU_OFF.     */

#define PSCI_VERSION       0x84000000u      /* major << 16 | minor */
#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_FEATURES      0x8400000Au      /* (function id): 0 = supported */
#define PSCI_NOT_SUPPORTED (-1)
#define PSCI_ALREADY_ON    (-4)             /* CPU_ON: the core is on already (not ours to start) */
#define PSCI_ON_PENDING    (-5)             /* CPU_ON: an earlier start of that core is still completing */

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
volatile uint64_t other_cpu_parked __attribute__((aligned(64))); /* 1: that core waits in the loader between runs */
volatile uint64_t other_cpu_go __attribute__((aligned(64)));     /* 1: wake the parked core for another run */
volatile uint64_t other_cpu_inval_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_inval_end __attribute__((aligned(64)));
volatile uint64_t other_cpu_inval_step __attribute__((aligned(64)));
uint64_t psci_by_hvc __attribute__((aligned(64)));
/* the kernel's window: a warm core must drop its cached copies of it before the next run */
volatile uint64_t other_cpu_window_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_window_end __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* Say that this core arrived before anything that could fault. */
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov  x10, #1\n"
        "  str  x10, [x9]\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov  x10, #2\n"
        "  str  x10, [x9]\n"
        /* Some PSCI implementations do not pass the context id in x0; find
         * the hand-off block by its own address instead. */
        "  adrp x9, other_cpu_start\n"
        "  add  x9, x9, :lo12:other_cpu_start\n"
        "  ldp x9, x10, [x9]\n"
        "  mov x0, x10\n"
        "  adrp x11, other_cpu_stage\n"
        "  add  x11, x11, :lo12:other_cpu_stage\n"
        "  mov  x12, #3\n"
        "  str  x12, [x11]\n"
        "  mov  x12, #4\n"
        "  str  x12, [x11]\n"
        "  dsb  sy\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        /* A53 and every other ARMv8.0 core do not implement PAN/UAO. */
        "  mrs x9, id_aa64mmfr1_el1\n"
        "  tbz x9, #20, 8f\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "8:\n"
        "  tbz x9, #0, 9f\n"
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "9:\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        /* Set these in the order the loader's core relies on: when it sees `done`, this core is
         * already on its way into the park loop, not still on its way out of the kernel. */
        "  adrp x10, other_cpu_parked\n"
        "  add  x10, x10, :lo12:other_cpu_parked\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        /* Stay here, with the MMU and caches off, instead of asking the firmware to switch this
         * core off.  The firmware on this phone does not start it again reliably afterwards. */
        "  adrp x10, other_cpu_go\n"
        "  add  x10, x10, :lo12:other_cpu_go\n"
        "1:\n"
        "  ldr x9, [x10]\n"
        "  cbnz x9, 2f\n"
        "  wfe\n"
        "  b 1b\n"
        "2:\n"
        /* Take the doorbell down ourselves: the loader's core may be slow to do it, and a stale
         * one would start this core into the kernel a second time. */
        "  mov x9, #0\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        /* The kernel ran here with caches on and the loader reloaded its window with caches on
         * on the other core.  Drop every line this core still has of that window, then the
         * instruction cache and TLB, before entering the kernel again. */
        "  adrp x10, other_cpu_inval_start\n"
        "  add  x10, x10, :lo12:other_cpu_inval_start\n"
        "  ldr x11, [x10]\n"
        "  adrp x10, other_cpu_inval_end\n"
        "  add  x10, x10, :lo12:other_cpu_inval_end\n"
        "  ldr x12, [x10]\n"
        "  adrp x10, other_cpu_inval_step\n"
        "  add  x10, x10, :lo12:other_cpu_inval_step\n"
        "  ldr x13, [x10]\n"
        "  cbz x13, 4f\n"
        "  sub x14, x13, #1\n"
        "  bic x11, x11, x14\n"
        "3:\n"
        "  cmp x11, x12\n"
        "  b.hs 4f\n"
        "  dc ivac, x11\n"
        "  add x11, x11, x13\n"
        "  b 3b\n"
        "4:\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  dsb sy\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  isb\n"
        "  b other_cpu_entry\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
};

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */
#define LATE_NOTE_SECONDS       1800     /* how long the kernel is watched and its log kept */
#define SCREEN_QUIET_SECONDS    35       /* stop drawing the log over the kernel's own display */
static uint64_t late_note_seconds = LATE_NOTE_SECONDS;

static void wdt_pet(void);
static int wdt_armed;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	/* the kernel cleaned these to the point of coherency: drop our own copies before reading */
	flush_dcache((uint64_t)(uintptr_t)(live->log_len + slide), (uint64_t)(uintptr_t)(live->log_len + slide) + 4);
	flush_dcache((uint64_t)(uintptr_t)(live->log_ring_at + slide),
	    (uint64_t)(uintptr_t)(live->log_ring_at + slide) + 4);
	flush_dcache((uint64_t)(uintptr_t)(live->log_wrapped + slide),
	    (uint64_t)(uintptr_t)(live->log_wrapped + slide) + 1);
	flush_dcache((uint64_t)(uintptr_t)log, (uint64_t)(uintptr_t)log + live->log_head + live->log_ring);
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len, ring_at;
	int wrapped;
	uint64_t n, room = sizeof(out_buf) - 8192;

	/* the kernel cleaned these to the point of coherency: drop our own copies before reading */
	flush_dcache((uint64_t)(uintptr_t)(live->log_len + slide), (uint64_t)(uintptr_t)(live->log_len + slide) + 4);
	flush_dcache((uint64_t)(uintptr_t)(live->log_ring_at + slide),
	    (uint64_t)(uintptr_t)(live->log_ring_at + slide) + 4);
	flush_dcache((uint64_t)(uintptr_t)(live->log_wrapped + slide),
	    (uint64_t)(uintptr_t)(live->log_wrapped + slide) + 1);
	flush_dcache((uint64_t)(uintptr_t)log, (uint64_t)(uintptr_t)log + live->log_head + live->log_ring);
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);

	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}


static void flush_live_log(void);
static int64_t psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3);

/*
 * The host's loop: leave a "boot-recovery" bootloader message in misc and reset.
 * The bootloader then starts recovery (TWRP), where the host pulls this run's log,
 * flashes the next image and reboots; nothing here needs a person at the phone.
 */
static void
cycle_to_recovery(void)
{
	static uint8_t bcb[512];
	int err;

	flush_live_log();
	memset(bcb, 0, sizeof(bcb));
	memcpy(bcb, "boot-recovery", 13);
	err = emmc_write_blocks(MISC_LBA, 1, bcb);
	{
		static uint8_t back[512];
		int rs = emmc_read_blocks(MISC_LBA, 1, back);
		int same = rs == 0 && memcmp(back, bcb, sizeof(bcb)) == 0;

		print("cycle: boot-recovery in misc at lba %x (write %d), read back %s\n",
		    MISC_LBA, (uint64_t)err, same ? "the same" : "DIFFERENT");
	}
	flush_live_log();
	/* the card may still be programming: let it finish before the reset */
	for (volatile uint64_t i = 0; i < 20000000ULL; i++) {
	}
	/*
	 * The boot mode LK reads: the device tree's syscon-reboot-mode node is
	 * the reset generator at 0x10007000, offset 0x24, mask 0xf; 2 is
	 * "recovery".  A warm reset keeps the register.
	 */
	{
		volatile uint32_t *rgu = (volatile uint32_t *)(uintptr_t) 0x10007024u;
		uint32_t before = *rgu;

		*rgu = (before & ~0xfu) | 2u;
		__asm__ volatile ("dsb sy" ::: "memory");
		print("cycle: TOPRGU mode-recovery (%x -> %x)\n", (uint64_t)before, (uint64_t)*rgu);
	}
	flush_live_log();
	/*
	 * A warm reset through the firmware's power interface, as Android's own
	 * syscon-reboot-mode does: the UEFI reset path here resets the SoC cold
	 * and the reset generator's mode is lost.
	 */
	print("cycle: cold reset\n");
	flush_live_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
		BS->stall(1000000);
	}
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void flush_live_log(void);
static void put_char(char c);
static void fill_screen(uint32_t color);

/*
 * While the kernel runs, its log is what matters, and it is only in the kernel's
 * memory: compose it together with the loader's own lines and write that.  (The
 * old path wrote log_buf alone, so a run that never returned left the partition
 * holding the previous run's log.)
 */
static void
flush_live_log(void)
{
	/*
	 * This is the kernel's log while it runs: written, not read back. The read-back doubles
	 * the traffic through the same controller the kernel's own storage requests use, and a
	 * log written every few seconds is not worth that.
	 */
	log_skip_readback = 1;
	if (running_live != NULL) {
		/*
		 * Write the loader's own lines first: if composing the kernel's ring
		 * faults, the failure is still visible in the partition ("composing"
		 * without the live log that should follow it).
		 */
		print("live-log: composing %d bytes\n", (int)staged_len);
		flush_log();
		compose_live_log(running_live, running_slide);
		log_full_live = 1;
		print("live-log: composed %d bytes\n", (int)staged_len);
	}
	flush_log();
	log_skip_readback = 0;
}


/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * bsd/dev/redwood_ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (bsd/dev/redwood_ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
#define BLOCK_DARWIN_FIRST 25374720ULL                 /* its first and last LBA of unit 0, as the GPT has them */
#define BLOCK_DARWIN_LAST  30617594ULL
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }
#define BLOCK_DARWIN_UNIQ  { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } }

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
/*
 * The phone's own storage (A03s: eMMC through MSDC0).  Its partitions are in the same page as the
 * UEFI ones, with no block_io behind them: block_bios[] is NULL for them, and block_poll reads
 * them through the controller that LK left ready.  emmc_unit is their unit, -1 when there is none.
 */
static int emmc_unit = -1;
static int emmc_page_start(int unit);
static int emmc_read_blocks(uint32_t lba, uint32_t count, uint8_t *buf);
static int emmc_write_blocks(uint32_t lba, uint32_t count, const uint8_t *buf);
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in this phone's Android device tree */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE, uniq = BLOCK_DARWIN_UNIQ;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &uniq, sizeof(uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (info->start_lba != BLOCK_DARWIN_FIRST || info->end_lba != BLOCK_DARWIN_LAST) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != BLOCK_DARWIN_LAST - BLOCK_DARWIN_FIRST + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, BLOCK_WRITE_FIRST);
	return 1;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles = NULL;
	uint64_t count = 0;
	int have_partitions;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
	}
	have_partitions = !EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles));
	if (!have_partitions) {
		/* this firmware has no storage driver: what is left is the phone's own eMMC, below */
		print("ufs: no UEFI partition information: only the eMMC is served\n");
		handles = NULL;
		count = 0;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	/*
	 * The phone's eMMC, if this is the phone and the card answers.  It is the next unit after the
	 * UEFI ones (on the A03s there are none, so it is unit 0), and its partitions are read through
	 * the controller LK left ready -- no UEFI driver, no DMA, no interrupts.
	 */
	emmc_unit = (int)nunits;
	if (emmc_page_start(emmc_unit) <= 0) {
		emmc_unit = -1;
	}
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = BLOCK_WRITE_FIRST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits - (emmc_unit >= 0 ? 1 : 0),
	    (uint64_t)nunits + (emmc_unit >= 0 ? 1 : 0), (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE) {
			/* (no eMMC partition ever takes a write: block_write_part is a UEFI one, if any) */
			if (bio == NULL || block_write_part < 0 || part != (uint32_t)block_write_part ||
			    lba < BLOCK_WRITE_FIRST) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			if (bio == NULL) {
				static int emmc_errors;
				uint64_t card_lba = block_shared.part[part].start_lba + lba;

				if (emmc_unit < 0 || card_lba + count > 0xffffffffULL) {
					s = 0x8000000000000002ULL;      /* EFI_INVALID_PARAMETER */
				} else {
					int err = emmc_read_blocks((uint32_t)card_lba, (uint32_t)count, block_buffer);

					s = err == 0 ? 0 : 0x800000000000000EULL;   /* EFI_DEVICE_ERROR */
					if (err != 0 && emmc_errors++ < 4) {
						print("emmc: read of %d blocks from %x of %s failed (%d)\n", count,
						    card_lba, block_shared.part[part].name, (uint64_t)err);
					}
				}
			} else {
				s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			}
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || PLAT != &platform_redwood) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/* For the record: how much the kernel on the other core has logged so far, and what is new in it. */
/* Does the line (p, n) contain the needle? */
static int
note_line_has(const char *p, uint32_t n, const char *needle)
{
	uint32_t k = 0;

	while (needle[k] != 0) {
		k++;
	}
	if (n < k) {
		return 0;
	}
	for (uint32_t i = 0; i + k <= n; i++) {
		uint32_t j = 0;

		while (j < k && p[i + j] == needle[j]) {
			j++;
		}
		if (j == k) {
			return 1;
		}
	}
	return 0;
}

/* Print the exec/exit lines of the burst, which the 3200-byte tail below may cut off. */
static void
note_exec_lines(const char *log, uint32_t from, uint32_t to)
{
	for (uint32_t i = from; i < to; ) {
		uint32_t j = i;

		while (j < to && log[j] != '\n') {
			j++;
		}
		if (note_line_has(log + i, j - i, "EXECDBG: execve") ||
		    note_line_has(log + i, j - i, "EXECDBG: exit") ||
		    note_line_has(log + i, j - i, "EXECDBG lsig") ||
		    note_line_has(log + i, j - i, "EXECDBG plat force") ||
		    note_line_has(log + i, j - i, "EXECDBG csops status") ||
		    note_line_has(log + i, j - i, "EXECDBG textvp") ||
		    note_line_has(log + i, j - i, "EXECDBG ent ") ||
		    note_line_has(log + i, j - i, "hakios:") ||
		    note_line_has(log + i, j - i, "backboardd[") ||
		    note_line_has(log + i, j - i, "WindowServer") ||
		    note_line_has(log + i, j - i, "GS ready") ||
		    note_line_has(log + i, j - i, "surfaces") ||
		    note_line_has(log + i, j - i, "Terminating app") ||
		    note_line_has(log + i, j - i, "tl: ") ||
		    note_line_has(log + i, j - i, "WATCHDOG")) {
			put_char('\n');
			for (uint32_t k = i; k < j; k++) {
				put_char(log[k] >= 0x20 && log[k] < 0x7f ? log[k] : '.');
			}
		}
		i = j + 1;
	}
}

static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;
	static uint32_t shown;          /* how far the screen has been brought */
	static uint64_t shown_beat;

	screen_only = 1;                /* (cleared below; the kernel log has its own copy) */
	if (live == NULL) {
		print("%d s\n", seconds);
		screen_only = 0;
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	flush_dcache((uint64_t)(uintptr_t)&live->heartbeat, (uint64_t)(uintptr_t)&live->heartbeat + 8);
	if (len < shown) {
		shown = 0;              /* a new run of the kernel */
		shown_beat = 0;
	}
	if (len > shown) {
		/*
		 * Show the bytes that have appeared since the last look, not the same tail
		 * again: a diagnostic dump written in one burst then scrolls once across
		 * the screen instead of being overwritten by copies of its own end.
		 */
		from = shown;
		if (len - from > 3200) {
			from = len - 3200;      /* a burst: the end of it is what matters */
			note_exec_lines(log, shown, from);
		}
		flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
		print("%d s: the kernel has logged %d bytes, heartbeat %d; %d new:\n", seconds,
		    (uint64_t)len, live->heartbeat, (uint64_t)(len - shown));
		for (uint32_t i = from; i < len; i++) {
			put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
		}
		print("\n");
		shown = len;
		shown_beat = live->heartbeat;
	} else if (live->heartbeat != shown_beat) {
		/* no new text, but the kernel is still alive and its timer is ticking */
		print("%d s: the kernel has logged %d bytes, heartbeat %d; no new log\n", seconds,
		    (uint64_t)len, live->heartbeat);
		shown_beat = live->heartbeat;
	}
	screen_only = 0;
}

/* Copy the kernel's live log into out_buf, unwrapping the ring; returns its length. */
static uint64_t
copy_live_log(const struct kernel_live *live, uint64_t slide)
{
	const char *log = (const char *)(live->log + slide);
	uint32_t len, ring_at;
	uint8_t wrapped;
	uint64_t n = 0;

	flush_dcache((uint64_t)(uintptr_t)(live->log_len + slide),
	    (uint64_t)(uintptr_t)(live->log_len + slide) + 4);
	flush_dcache((uint64_t)(uintptr_t)(live->log_ring_at + slide),
	    (uint64_t)(uintptr_t)(live->log_ring_at + slide) + 4);
	flush_dcache((uint64_t)(uintptr_t)(live->log_wrapped + slide),
	    (uint64_t)(uintptr_t)(live->log_wrapped + slide) + 1);
	flush_dcache((uint64_t)(uintptr_t)log,
	    (uint64_t)(uintptr_t)log + live->log_head + live->log_ring);

	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);

	if (len > live->log_head + live->log_ring) {
		len = live->log_head + live->log_ring;
	}

	if (!wrapped || ring_at >= live->log_ring) {
		uint32_t take = len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring;
		if (take > LOG_MAX_BYTES) take = LOG_MAX_BYTES;
		memcpy(out_buf, log, take);
		n = take;
	} else {
		uint32_t part1 = live->log_ring - ring_at;
		uint32_t part2 = ring_at;
		if (part1 > LOG_MAX_BYTES) part1 = LOG_MAX_BYTES;
		memcpy(out_buf, log + live->log_head + ring_at, part1);
		n = part1;
		if (n + part2 > LOG_MAX_BYTES) part2 = LOG_MAX_BYTES - n;
		memcpy(out_buf + n, log + live->log_head, part2);
		n += part2;
	}
	return n;
}

/* Does the live kernel log contain the SpringBoard assertion? */
static int
log_has_assertion(const struct kernel_live *live, uint64_t slide)
{
	static const char marker[] = "Assertion failure";
	uint64_t n = copy_live_log(live, slide);

	for (uint64_t i = 0; i + sizeof(marker) - 1 <= n; i++) {
		if (memcmp(out_buf + i, marker, sizeof(marker) - 1) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Clear the UEFI console (the framebuffer the kernel also writes to). */
static void
clear_screen(void)
{
	if (ST->con_out == NULL) {
		return;
	}
	if (ST->con_out->clear_screen != NULL) {
		ST->con_out->clear_screen(ST->con_out);
	}
	if (ST->con_out->set_cursor_position != NULL) {
		ST->con_out->set_cursor_position(ST->con_out, 0, 0);
	}
}

/* On the frozen screen, show a compact digest of the lines that matter:
   the last few EXECDBG csops/sig lines and the SpringBoard assertion.  A raw
   byte window is taller than the screen, so its top scrolls off; a dozen
   selected lines fit and stay visible. */
static void
print_log_around_marker(const struct kernel_live *live, uint64_t slide)
{
#define DIAG_MAX_LINES 12
#define DIAG_MAX_COLS  110
	uint64_t n = copy_live_log(live, slide);
	const char *lines[DIAG_MAX_LINES];
	unsigned lens[DIAG_MAX_LINES];
	unsigned count = 0, total = 0;

	static const char *markers[] = { "EXECDBG", "Assertion failure", "SpringBoardUI" };
	static const unsigned mlens[] = { 7, 17, 13 };

	uint64_t line_start = 0;
	for (uint64_t i = 0; i <= n; i++) {
		if (i != n && out_buf[i] != '\n') {
			continue;
		}
		unsigned len = (unsigned)(i - line_start);
		if (len > 0 && out_buf[line_start + len - 1] == '\r') {
			len--;
		}
		int match = 0;
		for (unsigned m = 0; m < sizeof(markers) / sizeof(markers[0]) && !match; m++) {
			for (unsigned k = 0; k + mlens[m] <= len; k++) {
				if (memcmp(out_buf + line_start + k, markers[m], mlens[m]) == 0) {
					match = 1;
					break;
				}
			}
		}
		if (match) {
			lines[count] = out_buf + line_start;
			lens[count] = len > DIAG_MAX_COLS ? DIAG_MAX_COLS : len;
			count = (count + 1) % DIAG_MAX_LINES;
			total++;
		}
		line_start = i + 1;
	}

	unsigned shown = total < DIAG_MAX_LINES ? total : DIAG_MAX_LINES;
	unsigned first = total < DIAG_MAX_LINES ? 0 : count;
	for (unsigned i = 0; i < shown; i++) {
		unsigned idx = (first + i) % DIAG_MAX_LINES;
		for (unsigned k = 0; k < lens[idx]; k++) {
			char c = lines[idx][k];
			put_char(c >= 0x20 && c < 0x7f ? c : '.');
		}
		put_char('\n');
	}
#undef DIAG_MAX_LINES
#undef DIAG_MAX_COLS
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

static int64_t
psci_affinity(uint64_t mpidr)
{
	return psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
}

static const char *
psci_state_name(int64_t state)
{
	switch (state) {
	case 0:
		return "on";
	case 1:
		return "off";
	case 2:
		return "coming up";
	default:
		return "not answering";
	}
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 *
 * The first run starts the core with PSCI CPU_ON.  The kernel's return path leaves that core in
 * other_cpu_return_stub with the MMU and caches off instead of switching it off (see the stub):
 * this phone's firmware does not start a core it has switched off a second time reliably.  Later
 * runs wake the parked core with `other_cpu_go` and a SEV, which needs no firmware at all.  It
 * drops its cached copies of the kernel's window and enters the kernel again at other_cpu_entry.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	uint64_t last_log_write;
	int64_t status = 0, state, version, on_feature, affinity_feature;
	int arrived = 0, watching_only = 0, reused;
	uint64_t waiting, beat;

	running_live = live;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;

	/*
	 * Was the core that ran the last kernel left parked in the loader by its return stub?
	 * If so, this run must not ask the firmware for it at all.
	 */
	flush_dcache((uint64_t)(uintptr_t)&other_cpu_parked, (uint64_t)(uintptr_t)&other_cpu_parked + 8);
	flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	reused = other_cpu_parked == 1 && other_cpu_done != 0;

	print("other_cpu_entry %x, other_cpu_start %x, entry %x\n",
	    (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start, entry);
	if (reused) {
		print("core %x: it is parked in the loader from the last run\n", mpidr);
	}

	/* This run answers for itself: the last run's markers are not this one's. */
	other_cpu_stage = 0;
	other_cpu_done = 0;
	other_cpu_parked = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_parked, (uint64_t)(uintptr_t)&other_cpu_parked + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_go, (uint64_t)(uintptr_t)&other_cpu_go + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_inval_start, (uint64_t)(uintptr_t)&other_cpu_inval_start + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_inval_end, (uint64_t)(uintptr_t)&other_cpu_inval_end + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_inval_step, (uint64_t)(uintptr_t)&other_cpu_inval_step + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_window_start, (uint64_t)(uintptr_t)&other_cpu_window_start + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_window_end, (uint64_t)(uintptr_t)&other_cpu_window_end + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	if (reused) {
		/*
		 * Wake it with the doorbell the return stub waits on.  It takes the doorbell down
		 * itself; this core must not write a stale one back over that, so the line is
		 * cleaned after the store and not written again until the next wake.
		 */
		other_cpu_go = 1;
		clean_dcache((uint64_t)(uintptr_t)&other_cpu_go, (uint64_t)(uintptr_t)&other_cpu_go + 8);
		__asm__ volatile ("dsb sy" ::: "memory");
		__asm__ volatile ("sev" ::: "memory");

		waiting = READ_SYSREG(CNTVCT_EL0);
		for (;;) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			if (other_cpu_stage != 0) {
				break;
			}
			if (seconds_since(waiting) >= START_SECONDS) {
				break;
			}
			if (wdt_armed) {
				wdt_pet();
			}
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
		if (other_cpu_stage == 0) {
			print("core %x: it did not answer the wake-up\n", mpidr);
			other_cpu_go = 0;
			clean_dcache((uint64_t)(uintptr_t)&other_cpu_go, (uint64_t)(uintptr_t)&other_cpu_go + 8);
			status = OTHER_CPU_NOT_STARTED;
		} else {
			print("core %x: it is awake\n", mpidr);
			other_cpu_go = 0;
			clean_dcache((uint64_t)(uintptr_t)&other_cpu_go, (uint64_t)(uintptr_t)&other_cpu_go + 8);
		}
	} else {
		/*
		 * First run: what the firmware says about the core before it is asked to start it.
		 */
		version = psci_call(PSCI_VERSION, 0, 0, 0);
		state = psci_affinity(mpidr);
		on_feature = psci_call(PSCI_FEATURES, PSCI_CPU_ON, 0, 0);
		affinity_feature = psci_call(PSCI_FEATURES, PSCI_AFFINITY_INFO, 0, 0);
		if (version >= 0) {
			print("PSCI %x.%x: core %x is %s (affinity %d) before the start\n",
			    (uint64_t)(version >> 16) & 0xffff, (uint64_t)version & 0xffff, mpidr, psci_state_name(state),
			    (uint64_t)state);
		} else {
			print("PSCI version not answered: core %x is %s (affinity %d) before the start\n", mpidr,
			    psci_state_name(state), (uint64_t)state);
		}
		print("PSCI features: CPU_ON %d, AFFINITY_INFO %d\n", (uint64_t)on_feature, (uint64_t)affinity_feature);
		if (state == 0) {
			/*
			 * Ask anyway: AFFINITY_INFO is not always to be trusted, and CPU_ON's own answer
			 * (ALREADY_ON, or a start) is the one that matters.
			 */
			print("core %x: the firmware says it is on; asking to start it anyway\n", mpidr);
		}

		status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry,
		    (uint64_t)(uintptr_t)&other_cpu_start);
		print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	}
	if (status == PSCI_ON_PENDING) {
		/*
		 * An earlier start of this core has not completed.  Wait for the core: if it comes
		 * to our entry, that start is ours after all; if it goes off, try once more.
		 */
		uint64_t waiting = READ_SYSREG(CNTVCT_EL0);
		int pending_arrived = 0;

		print("core %x: a start is already pending; waiting for it\n", mpidr);
		while (seconds_since(waiting) < START_SECONDS) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			if (other_cpu_stage != 0) {
				pending_arrived = 1;
				break;
			}
			if (psci_affinity(mpidr) == 1) {
				break;
			}
			if (wdt_armed) {
				wdt_pet();
			}
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
		if (pending_arrived) {
			status = 0;
		} else if (psci_affinity(mpidr) == 1) {
			print("core %x: the pending start is over; starting it again\n", mpidr);
			status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry,
			    (uint64_t)(uintptr_t)&other_cpu_start);
			print("core %x: the firmware answers %d to the second request\n", mpidr, (uint64_t)status);
		}
	}

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	last_log_write = started;
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		/*
		 * Past SCREEN_QUIET_SECONDS the kernel's own display driver owns the panel:
		 * clear our console once and never touch the framebuffer again.  A phone has
		 * no other console before that, so keep drawing until the cutoff.
		 */
		if (cycle_seconds != 0 && seconds_since(started) >= cycle_seconds) {
			cycle_to_recovery();
		}
		if (!screen_quiet && seconds_since(started) >= SCREEN_QUIET_SECONDS) {
			/*
			 * The handover test: four seconds of white.  Seen, the panel is
			 * powered and only the OS's drawing is missing; unseen, the panel
			 * itself is dark.
			 */
			fill_screen(0x00ffffff);
			for (int i = 0; i < 4; i++) {
				if (wdt_armed) {
					wdt_pet();
				}
				BS->stall(1000000);
			}
			clear_screen();
			print("hakios: loader console quiet; the kernel owns the display\n");
			screen_quiet = 1;   /* from here print() only writes to log_buf */
		}
		if (!watching_only && !screen_quiet && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			/* on the screen until the cutoff: a phone has no other console */
			if (!arrived) {
				print("%d s: the core has not arrived\n", noted);
			} else {
				note_kernel_progress(live, slide, noted);
				if (live != NULL && log_has_assertion(live, slide)) {
					/*
					 * SpringBoard has asserted: clear the scrolling console and put
					 * the log window around the assertion at the top of the screen,
					 * so the EXECDBG csops/sig diagnostics above it stay readable.
					 */
					clear_screen();
					print("== HAKIOS HALT: SpringBoard assertion at %d s ==\n", noted);
					print_log_around_marker(live, slide);
					print("\n== end of kernel log window ==\n");
					compose_live_log(live, slide);
					log_full_live = 1;
					flush_log();
					for (;;) {
						if (wdt_armed) {
							wdt_pet();
						}
						for (volatile int i = 0; i < 200000; i++) {
						}
					}
				}
			}
			if (PLAT->log_other_cpu && seconds_since(last_log_write) >= 15) {
				flush_live_log();
				last_log_write = READ_SYSREG(CNTVCT_EL0);
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			/*
			 * Between NOTE_SECONDS and LATE_NOTE_SECONDS the screen is still
			 * the only console: keep drawing the kernel log so the SpringBoard
			 * entitlement diagnostics (EXECDBG csops/sig) are visible after
			 * the 24 s mark.  Only then do we go watch-only and mask IRQs.
			 */
			if (seconds_since(started) < late_note_seconds) {
				/*
				 * The kernel owns the framebuffer once its display driver is up.
				 * Keep the persistent log current, but stop painting the log over
				 * the OS's own picture after the first few seconds.
				 */
				if (seconds_since(started) < SCREEN_QUIET_SECONDS) {
					note_kernel_progress(live, slide, seconds_since(started));
				}
				if (PLAT->log_other_cpu && seconds_since(last_log_write) >= 15) {
					flush_live_log();
					last_log_write = READ_SYSREG(CNTVCT_EL0);
				}
			} else {
				/*
				 * late_note_seconds passed and the kernel has not returned.
				 * Show the kernel log around the last assertion / EXECDBG csops/sig
				 * line so the diagnostics stay visible on the frozen screen.
				 */
				clear_screen();
				print("== HAKIOS HALT: the kernel is still running at %d s ==\n",
				    seconds_since(started));
				print_log_around_marker(live, slide);
				print("\n== end of kernel log window ==\n");
				compose_live_log(live, slide);
				log_full_live = 1;
				flush_log();
				for (;;) {
					if (wdt_armed) {
						wdt_pet();
					}
					for (volatile int i = 0; i < 200000; i++) {
					}
				}
			}
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			/* the kernel writes it with its caches on: drop our line before reading it */
			flush_dcache((uint64_t)(uintptr_t)&live->heartbeat, (uint64_t)(uintptr_t)&live->heartbeat + 8);
			beat = live->heartbeat;
			if (beat != last_beat) {
				last_beat = beat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		usb_poll_command();
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;

	/*
	 * The return stub sets `other_cpu_parked` before `other_cpu_done`, so by the time the
	 * return is seen the core is already on its way into the park loop (or in it).  There is
	 * no firmware call and nothing to wait for: the core stays on, inside the loader.
	 */
	if (other_cpu_done) {
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_parked, (uint64_t)(uintptr_t)&other_cpu_parked + 8);
		if (other_cpu_parked == 1) {
			print("core %x: parked in the loader for the next run\n", mpidr);
		} else {
			print("core %x: it came back but is not parked; the next run cannot use it\n", mpidr);
		}
	}

	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT != &platform_redwood) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\n*** XNU loader exception ***\n");
	print("type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);

	/*
	 * Do not reset: on a bring-up board the log is the only diagnostic.
	 * The MTK watchdog is disabled by BootShim and this platform does not
	 * arm the hypervisor watchdog, so spinning here keeps the screen up
	 * until the phone is power-cycled.
	 */
	print("halted; power-cycle the phone to continue\n");
	for (;;) {
		__asm__ volatile ("yield");
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	const uint8_t *kernel_lz4;      /* u64 uncompressed size + LZ4 legacy stream */
	uint32_t kernel_lz4_size;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	uint64_t ramdisk_split;         /* offset in the payload ramdisk where the second disk begins */
	const char *cmdline;
	uint64_t checkpoint;
	const char *checkpoint_scan;    /* comma-separated list: run the kernel once per checkpoint */
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

/* ------------------------------------------------------------------------ */
/* LZ4 legacy frames (the `lz4 -l` format the kernelcache is packed with)   */

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/*
 * One LZ4 block.  The expected output is known from the frame (8 MiB, except
 * for the last block).  Every length is checked before a copy so a corrupt
 * stream cannot walk out of the destination buffer.
 */
static int
lz4_block_decompress(const uint8_t *src, uint32_t src_size, uint8_t *dst, uint32_t dst_size)
{
	const uint8_t *s = src, *send = src + src_size;
	uint8_t *d = dst, *dend = dst + dst_size;

	while (s < send) {
		uint32_t token = *s++;
		uint32_t literal = token >> 4;

		if (literal == 15) {
			uint8_t b;
			do {
				if (s >= send) {
					return 0;
				}
				b = *s++;
				literal += b;
			} while (b == 255);
		}
		if (literal > (uint32_t)(send - s) || literal > (uint32_t)(dend - d)) {
			return 0;
		}
		memcpy(d, s, literal);
		d += literal;
		s += literal;
		if (s == send) {
			break;                          /* the last sequence is literals only */
		}
		if (s + 2 > send) {
			return 0;
		}
		uint32_t offset = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
		s += 2;
		if (offset == 0 || offset > (uint32_t)(d - dst)) {
			return 0;
		}
		uint32_t match = token & 15;
		if (match == 15) {
			uint8_t b;
			do {
				if (s >= send) {
					return 0;
				}
				b = *s++;
				match += b;
			} while (b == 255);
		}
		match += 4;
		if (match > (uint32_t)(dend - d)) {
			return 0;
		}
		const uint8_t *m = d - offset;
		for (uint32_t i = 0; i < match; i++) {
			d[i] = m[i];                    /* forward copy: handles overlap */
		}
		d += match;
	}
	return d == dend;
}

static int
lz4_legacy_decompress(const uint8_t *src, uint32_t src_size, uint8_t *dst, uint32_t dst_size)
{
	if (src_size < 4 || le32(src) != LZ4_LEGACY_MAGIC) {
		return 0;
	}
	uint32_t pos = 4, out = 0;
	while (pos + 4 <= src_size) {
		uint32_t csize = le32(src + pos);
		pos += 4;
		if (csize == 0) {
			break;                          /* optional end marker */
		}
		if (csize > src_size - pos) {
			return 0;
		}
		uint32_t block = dst_size - out;
		if (block > LZ4_LEGACY_BLOCK) {
			block = LZ4_LEGACY_BLOCK;
		}
		if (!lz4_block_decompress(src + pos, csize, dst + out, block)) {
			return 0;
		}
		pos += csize;
		out += block;
	}
	return out == dst_size;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	if (ph->size > PAYLOAD_MAX_SIZE - sizeof(*ph)) {
		print("payload size %x is larger than the reserved area\n", (uint64_t)ph->size);
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		uint64_t entry_end = sizeof(*e) + (((uint64_t)e->size + 15) & ~15ULL);
		if (e->name[0] == 0) {
			break;
		}
		if (entry_end > (uint64_t)(pend - p)) {
			print("payload entry '%s' runs past the payload\n", e->name);
			return 0;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "rdsplit")) {
			pi->ramdisk_split = parse_number(text, 16); /* one image, two RAM disks (md0, then md1) */
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
		} else if (str_eq(e->name, "kernel.lz4")) {
			pi->kernel_lz4 = p + sizeof(*e);          /* u64 size + LZ4 legacy stream */
			pi->kernel_lz4_size = e->size;
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "checkpoints")) {
			pi->checkpoint_scan = text;              /* bring-up scan: one run per checkpoint */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += entry_end;
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;

/*
 * Fill the panel from the boot framebuffer: a white screen for a few seconds at
 * the handover is how one tells "the panel is off" from "the OS has not drawn".
 */
static void
fill_screen(uint32_t color)
{
	uint32_t *fb;
	uint64_t stride;

	if (SCREEN.base == 0 || SCREEN.width == 0 || SCREEN.height == 0) {
		return;
	}
	fb = (uint32_t *)(uintptr_t)SCREEN.base;
	stride = SCREEN.stride / 4;
	for (uint64_t y = 0; y < SCREEN.height; y++) {
		for (uint64_t x = 0; x < SCREEN.width; x++) {
			fb[y * stride + x] = color;
		}
	}
	/* the display engine reads memory, not this core's cache */
	flush_dcache((uint64_t)(uintptr_t)SCREEN.base,
	    (uint64_t)(uintptr_t)SCREEN.base + SCREEN.stride * SCREEN.height);
}

static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t forced_win_base, forced_win_size;
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (forced_win_base != 0 && forced_win_size != 0) {
		win_base = forced_win_base;
		win_size = forced_win_size;
		print("phys window %x size %x (forced)\n", win_base, win_size);
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	/*
	 * Collect the free ranges first and merge the ones that touch: the map is
	 * fragmented into many boot-services blocks, and the largest single
	 * descriptor (224 MB here) is much smaller than what the phone has.
	 */
	{
		struct free_range {
			uint64_t lo, hi;
		} ranges[128];
		int count = 0;

		for (uint64_t off = 0; off < map_size && count < 128; off += desc_size) {
			efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
			/*
			 * PrePi leaves most of RAM as boot-services memory rather than
			 * EfiConventionalMemory.  The loader exits boot services before
			 * starting the kernel, so boot-services pages are free for it too.
			 */
			if (d->type != EfiConventionalMemory &&
			    d->type != EfiBootServicesCode &&
			    d->type != EfiBootServicesData) {
				continue;
			}
			uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
			/* the payload sits in RAM that UEFI may consider free: stay clear of it */
			if (lo < pay_hi && hi > pay_lo) {
				if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
					hi = pay_lo;
				} else {
					lo = pay_hi;
				}
			}
			lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
			hi &= ~(BASE_ALIGN - 1);
			if (hi > lo) {
				ranges[count].lo = lo;
				ranges[count].hi = hi;
				count++;
			}
		}
		/* insertion sort by start */
		for (int i = 1; i < count; i++) {
			struct free_range key = ranges[i];
			int j = i - 1;

			while (j >= 0 && ranges[j].lo > key.lo) {
				ranges[j + 1] = ranges[j];
				j--;
			}
			ranges[j + 1] = key;
		}
		uint64_t run_lo = 0, run_hi = 0;
		for (int i = 0; i < count; i++) {
			if (run_hi != 0 && ranges[i].lo <= run_hi) {
				if (ranges[i].hi > run_hi) {
					run_hi = ranges[i].hi;
				}
			} else {
				if (run_hi - run_lo > best_size) {
					best_base = run_lo;
					best_size = run_hi - run_lo;
				}
				run_lo = ranges[i].lo;
				run_hi = ranges[i].hi;
			}
		}
		if (run_hi - run_lo > best_size) {
			best_base = run_lo;
			best_size = run_hi - run_lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/* ------------------------------------------------------------------------ */
/* eMMC (MSDC0) diagnostic, asked for with "emmc_test=1" on the cmdline      */
/*
 * The A03s UEFI has no storage driver, but LK leaves MSDC0 powered and
 * clocked when it hands over.  This reads the card through the controller
 * the way LK's PIO path does, without writing anything to it.  It first
 * tries the state LK left behind (CMD13, then CMD17 at the current clock and
 * bus width); if that does not answer, it re-initialises the card at 400 kHz
 * in 1-bit mode and tries again.  The loader hangs afterwards so the result
 * stays on the screen.
 */

#define EMMC_MSDC0_BASE 0x11230000UL

#define EMMC_MSDC_CFG       0x00
#define EMMC_MSDC_IOCON     0x04
#define EMMC_MSDC_PS        0x08
#define EMMC_MSDC_INT       0x0c
#define EMMC_MSDC_INTEN     0x10
#define EMMC_MSDC_FIFOCS    0x14
#define EMMC_MSDC_RXDATA    0x1c
#define EMMC_MSDC_TXDATA    0x18
#define EMMC_SDC_CFG        0x30
#define EMMC_SDC_CMD        0x34
#define EMMC_SDC_ARG        0x38
#define EMMC_SDC_STS        0x3c
#define EMMC_SDC_RESP0      0x40
#define EMMC_SDC_RESP1      0x44
#define EMMC_SDC_RESP2      0x48
#define EMMC_SDC_RESP3      0x4c
#define EMMC_SDC_BLK_NUM    0x50
#define EMMC_MSDC_PATCH_BIT0 0xb0
#define EMMC_MSDC_VERSION   0x114

#define EMMC_CFG_MODE        (0x1u << 0)
#define EMMC_CFG_RST         (0x1u << 2)
#define EMMC_CFG_PIO         (0x1u << 3)
#define EMMC_CFG_CKSTB       (0x1u << 7)
#define EMMC_CFG_CKDIV       (0xfffu << 8)
#define EMMC_CFG_CKMOD       (0x3u << 20)
#define EMMC_CFG_CKMOD_HS400 (0x1u << 22)
#define EMMC_CFG_START_BIT   (0x3u << 23)

#define EMMC_FIFOCS_CLR      (0x1u << 31)
#define EMMC_FIFOCS_RXCNT    0xffu
#define EMMC_FIFOCS_TXCNT    (0xffu << 16)

#define EMMC_SDC_STS_SDCBUSY (0x1u << 0)
#define EMMC_PS_DAT0         (0x1u << 16)
#define EMMC_SDC_CFG_BUSWIDTH (0x3u << 16)

#define EMMC_INT_ACMDRDY     (0x1u << 3)
#define EMMC_INT_ACMDTMO     (0x1u << 4)
#define EMMC_INT_ACMDCRCERR  (0x1u << 5)
#define EMMC_INT_CMDRDY      (0x1u << 8)
#define EMMC_INT_CMDTMO      (0x1u << 9)
#define EMMC_INT_RSPCRCERR   (0x1u << 10)
#define EMMC_INT_XFER_COMPL  (0x1u << 12)
#define EMMC_INT_DATTMO      (0x1u << 14)
#define EMMC_INT_DATCRCERR   (0x1u << 15)

#define EMMC_CMD_INT_BITS (EMMC_INT_CMDRDY | EMMC_INT_CMDTMO | EMMC_INT_RSPCRCERR | \
    EMMC_INT_ACMDRDY | EMMC_INT_ACMDTMO | EMMC_INT_ACMDCRCERR)
#define EMMC_DATA_INT_BITS (EMMC_INT_XFER_COMPL | EMMC_INT_DATTMO | EMMC_INT_DATCRCERR)

static uint32_t
emmc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(uintptr_t)(EMMC_MSDC0_BASE + reg);
}

static void
emmc_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)(EMMC_MSDC0_BASE + reg) = value;
}

/*
 * One command; dtype 0 = none, 1 = a single block, 2 = the blocks SDC_BLK_NUM says (CMD18).
 * rsp is the controller's response type: 0 = none, 1 = short (R1/R5/R6/R7), 2 = long (R2),
 * 3 = R3, 7 = R1b.  Returns 0, -1 when no response interrupt arrived, -2 on a command
 * timeout or CRC error.
 */
static int
emmc_command(uint32_t opcode, uint32_t arg, uint32_t rsp, uint32_t dtype, uint32_t blklen, uint32_t *resp)
{
	uint32_t raw = (opcode & 0x3f) | (rsp << 7) | (dtype << 11) | ((blklen & 0xfff) << 16);
	uint32_t status = 0;

	if (opcode == 24 || opcode == 25) {
		/* data direction: 1 = card to host (LK's rawcmd "rw" bit for CMD24/25) */
		raw |= (1u << 13);
	}

	if (opcode == 12) {
		/* STOP_TRANSMISSION: end an open-ended CMD18/CMD25 (LK's "stop" bit, no block length) */
		raw |= (1u << 14);
		raw &= ~(0xfffu << 16);
	}

	for (uint32_t i = 0; i < 2000000; i++) {
		if ((emmc_read(EMMC_SDC_STS) & EMMC_SDC_STS_SDCBUSY) == 0) {
			break;
		}
	}
	emmc_write(EMMC_MSDC_INT, EMMC_CMD_INT_BITS);
	emmc_write(EMMC_SDC_ARG, arg);
	emmc_write(EMMC_SDC_CMD, raw);
	for (uint32_t i = 0; i < 20000000; i++) {
		status = emmc_read(EMMC_MSDC_INT);
		if ((status & EMMC_CMD_INT_BITS) != 0) {
			break;
		}
	}
	emmc_write(EMMC_MSDC_INT, status & EMMC_CMD_INT_BITS);
	if ((status & (EMMC_INT_CMDTMO | EMMC_INT_RSPCRCERR | EMMC_INT_ACMDTMO | EMMC_INT_ACMDCRCERR)) != 0) {
		return -2;
	}
	if ((status & (EMMC_INT_CMDRDY | EMMC_INT_ACMDRDY)) == 0) {
		return -1;
	}
	if (rsp == 7) {
		/* R1b: the card holds DAT0 low while it is busy. */
		for (uint32_t i = 0; i < 20000000; i++) {
			if ((emmc_read(EMMC_MSDC_PS) & EMMC_PS_DAT0) != 0) {
				break;
			}
		}
	}
	if (resp != NULL) {
		if (rsp == 2) {
			resp[0] = emmc_read(EMMC_SDC_RESP3);
			resp[1] = emmc_read(EMMC_SDC_RESP2);
			resp[2] = emmc_read(EMMC_SDC_RESP1);
			resp[3] = emmc_read(EMMC_SDC_RESP0);
		} else {
			resp[0] = emmc_read(EMMC_SDC_RESP0);
		}
	}
	return 0;
}

/* Read one 512-byte block through the controller's FIFO (LK's PIO path). */
static int
emmc_read_block(uint32_t lba, uint8_t *buf)
{
	uint32_t left = 512;
	uint32_t done = 0;
	uint32_t at = 0;

	emmc_write(EMMC_MSDC_CFG, emmc_read(EMMC_MSDC_CFG) | EMMC_CFG_PIO);
	emmc_write(EMMC_MSDC_INTEN, 0);
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
	emmc_write(EMMC_MSDC_FIFOCS, EMMC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_CLR) != 0; i++) {
	}
	emmc_write(EMMC_SDC_BLK_NUM, 1);
	if (emmc_command(17, lba, 1, 1, 512, NULL) != 0) {
		return -1;
	}
	for (uint32_t i = 0; i < 50000000; i++) {
		uint32_t status = emmc_read(EMMC_MSDC_INT) & EMMC_DATA_INT_BITS;

		if (status != 0) {
			emmc_write(EMMC_MSDC_INT, status);
		}
		if ((status & EMMC_INT_DATCRCERR) != 0) {
			return -2;
		}
		if ((status & EMMC_INT_DATTMO) != 0) {
			return -3;
		}
		if ((status & EMMC_INT_XFER_COMPL) != 0) {
			done = 1;
		}
		uint32_t count = emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_RXCNT;
		if (count > left) {
			count = left;
		}
		while (count >= 4) {
			*(uint32_t *)(buf + at) = emmc_read(EMMC_MSDC_RXDATA);
			at += 4;
			left -= 4;
			count -= 4;
		}
		if (left == 0 && done) {
			return 0;
		}
	}
	return -4;
}

/* Drop what is left in the data FIFO and any data interrupt it raised. */
static void
emmc_clear_data(void)
{
	emmc_write(EMMC_MSDC_FIFOCS, EMMC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_CLR) != 0; i++) {
	}
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
}

/* End an open-ended data transfer (CMD12), whatever state the controller is in. */
static void
emmc_stop(void)
{
	(void)emmc_command(12, 0, 1, 0, 0, NULL);
	emmc_clear_data();
}

/*
 * Several blocks through one CMD18.  eMMC stops at CMD12, which the controller does not send by
 * itself here (LK leaves autocmd off), so the stop goes out after the data either way.  At LK's
 * HS-DDR clock this is the difference between one command per 512 bytes and one per megabyte.
 */
#define EMMC_MAX_BLOCKS 2048u

static int
emmc_read_multi(uint32_t lba, uint32_t count, uint8_t *buf)
{
	uint32_t left = count * 512, at = 0, done = 0, status = 0;

	emmc_write(EMMC_MSDC_CFG, emmc_read(EMMC_MSDC_CFG) | EMMC_CFG_PIO);
	emmc_write(EMMC_MSDC_INTEN, 0);
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
	emmc_write(EMMC_MSDC_FIFOCS, EMMC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_CLR) != 0; i++) {
	}
	emmc_write(EMMC_SDC_BLK_NUM, count);
	if (emmc_command(18, lba, 1, 2, 512, NULL) != 0) {
		emmc_stop();
		return -1;
	}
	for (uint32_t i = 0; i < 100000000u; i++) {
		uint32_t have;

		status = emmc_read(EMMC_MSDC_INT) & EMMC_DATA_INT_BITS;
		if (status != 0) {
			emmc_write(EMMC_MSDC_INT, status);
		}
		if ((status & EMMC_INT_DATCRCERR) != 0) {
			emmc_stop();
			return -2;
		}
		if ((status & EMMC_INT_DATTMO) != 0) {
			emmc_stop();
			return -3;
		}
		if ((status & EMMC_INT_XFER_COMPL) != 0) {
			done = 1;
		}
		have = emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_RXCNT;
		if (have > left) {
			have = left;
		}
		while (have >= 4 && left >= 4) {
			*(uint32_t *)(buf + at) = emmc_read(EMMC_MSDC_RXDATA);
			at += 4;
			left -= 4;
			have -= 4;
		}
		if (left != 0 && left < 4 && have >= 4) {
			uint32_t word = emmc_read(EMMC_MSDC_RXDATA);

			memcpy(buf + at, &word, left);
			at += left;
			left = 0;
		}
		if (left == 0 && done) {
			break;
		}
	}
	if (left != 0 || !done) {
		emmc_stop();
		return -4;
	}
	if (emmc_command(12, 0, 1, 0, 0, NULL) != 0) {
		emmc_clear_data();
		return -5;
	}
	emmc_clear_data();
	return 0;
}

/*
 * Bytes of the card at 512-byte blocks: CMD18 while it works, one CMD17 per block if it does not.
 * The caller has already bounded lba and count (card_lba + count <= 2^32, count <= 4 MB / 512).
 */
static int
emmc_read_blocks(uint32_t lba, uint32_t count, uint8_t *buf)
{
	static int multi = 1;

	while (count != 0) {
		uint32_t n = count > EMMC_MAX_BLOCKS ? EMMC_MAX_BLOCKS : count;

		if (multi && emmc_read_multi(lba, n, buf) == 0) {
			lba += n;
			buf += n * 512;
			count -= n;
			continue;
		}
		if (multi) {
			multi = 0;
			print("emmc: CMD18 at %x of %d blocks failed: one CMD17 per block from now on\n",
			    (uint64_t)lba, (uint64_t)n);
		}
		for (uint32_t i = 0; i < n; i++) {
			int err = emmc_read_block(lba + i, buf + (uint64_t)i * 512);

			if (err != 0) {
				print("emmc: block %x read failed (%d)\n", (uint64_t)(lba + i), (uint64_t)err);
				return err;
			}
		}
		lba += n;
		buf += n * 512;
		count -= n;
	}
	return 0;
}

/* Several blocks out through CMD25 and the controller's TX FIFO (the read path above, mirrored). */
static int
emmc_write_multi(uint32_t lba, uint32_t count, const uint8_t *buf)
{
	uint32_t left = count * 512, at = 0, done = 0, status = 0;

	emmc_write(EMMC_MSDC_CFG, emmc_read(EMMC_MSDC_CFG) | EMMC_CFG_PIO);
	emmc_write(EMMC_MSDC_INTEN, 0);
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
	emmc_write(EMMC_MSDC_FIFOCS, EMMC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_CLR) != 0; i++) {
	}
	emmc_write(EMMC_SDC_BLK_NUM, count);
	if (emmc_command(25, lba, 1, 2, 512, NULL) != 0) {
		emmc_stop();
		return -1;
	}
	for (uint32_t i = 0; i < 100000000u; i++) {
		uint32_t space;

		status = emmc_read(EMMC_MSDC_INT) & EMMC_DATA_INT_BITS;
		if (status != 0) {
			emmc_write(EMMC_MSDC_INT, status);
		}
		if ((status & EMMC_INT_DATCRCERR) != 0) {
			emmc_stop();
			return -2;
		}
		if ((status & EMMC_INT_DATTMO) != 0) {
			emmc_stop();
			return -3;
		}
		if ((status & EMMC_INT_XFER_COMPL) != 0) {
			done = 1;
		}
		space = (emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_TXCNT) >> 16;
		if (space == 0) {
			/* FIFO empty: fill one round, at most its 128 bytes (LK's msdc_pio_write) */
			uint32_t round = left > 128 ? 128 : left;

			while (round >= 4) {
				emmc_write(EMMC_MSDC_TXDATA, *(const uint32_t *)(const void *)(buf + at));
				at += 4;
				left -= 4;
				round -= 4;
			}
			while (round-- > 0) {
				emmc_write(EMMC_MSDC_TXDATA, (uint32_t)buf[at]);
				at++;
				left--;
			}
		}
		if (left == 0 && done) {
			break;
		}
	}
	if (left != 0 || !done) {
		emmc_stop();
		return -4;
	}
	if (emmc_command(12, 0, 7, 0, 0, NULL) != 0) {
		emmc_clear_data();
		return -5;
	}
	emmc_clear_data();
	return 0;
}

static int
emmc_write_blocks(uint32_t lba, uint32_t count, const uint8_t *buf)
{
	while (count != 0) {
		uint32_t n = count > EMMC_MAX_BLOCKS ? EMMC_MAX_BLOCKS : count;
		int err = emmc_write_multi(lba, n, buf);

		if (err != 0) {
			print("emmc: write of %d blocks at %x failed (%d)\n", (uint64_t)n,
			    (uint64_t)lba, (uint64_t)err);
			return err;
		}
		lba += n;
		buf += (uint64_t)n * 512;
		count -= n;
	}
	return 0;
}

static void
emmc_dump(const char *when)
{
	print("emmc: %s cfg=%x iocon=%x ps=%x int=%x inten=%x fifo=%x sdc_cfg=%x sts=%x pb0=%x ver=%x\n",
	    when, emmc_read(EMMC_MSDC_CFG), emmc_read(EMMC_MSDC_IOCON), emmc_read(EMMC_MSDC_PS),
	    emmc_read(EMMC_MSDC_INT), emmc_read(EMMC_MSDC_INTEN), emmc_read(EMMC_MSDC_FIFOCS),
	    emmc_read(EMMC_SDC_CFG), emmc_read(EMMC_SDC_STS), emmc_read(EMMC_MSDC_PATCH_BIT0),
	    emmc_read(EMMC_MSDC_VERSION));
}

/* CMD0 + CMD1 + CMD2 + CMD3 + CMD9 + CMD7 at 400 kHz, 1-bit, PIO. */
static int
emmc_reinit(void)
{
	uint32_t cfg = emmc_read(EMMC_MSDC_CFG);
	uint32_t resp[4] = { 0, 0, 0, 0 };
	uint32_t status = 0;
	int err;

	/* 400 kHz from the 400 MHz MSDC0 source (CKDIV=250), 1-bit SDR, PIO. */
	cfg &= ~(EMMC_CFG_CKMOD_HS400 | EMMC_CFG_CKMOD | EMMC_CFG_CKDIV | EMMC_CFG_START_BIT);
	cfg |= EMMC_CFG_MODE | EMMC_CFG_PIO | (250u << 8);
	emmc_write(EMMC_MSDC_CFG, cfg | EMMC_CFG_RST);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_CFG) & EMMC_CFG_RST) != 0; i++) {
	}
	emmc_write(EMMC_MSDC_CFG, cfg);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_CFG) & EMMC_CFG_CKSTB) == 0; i++) {
	}
	emmc_write(EMMC_MSDC_INTEN, 0);
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
	emmc_write(EMMC_MSDC_FIFOCS, EMMC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 1000000 && (emmc_read(EMMC_MSDC_FIFOCS) & EMMC_FIFOCS_CLR) != 0; i++) {
	}
	emmc_write(EMMC_SDC_CFG, (emmc_read(EMMC_SDC_CFG) & ~EMMC_SDC_CFG_BUSWIDTH) | (0xffu << 24));
	emmc_dump("reinit");

	err = emmc_command(0, 0, 0, 0, 0, NULL);
	print("emmc: CMD0 -> %d\n", err);
	if (err != 0) {
		return -1;
	}

	err = -1;
	for (uint32_t round = 0; round < 3 && err != 0; round++) {
		/* 2.7-3.6 V window from LK, then the full window, then 1.8 V. */
		uint32_t arg = round == 0 ? 0x40008000u : (round == 1 ? 0x40ff8000u : 0x40000080u);

		for (uint32_t i = 0; i < 200; i++) {
			int r = emmc_command(1, arg, 3, 0, 0, resp);

			status = resp[0];
			if (r == 0 && (status & 0x80000000u) != 0) {
				err = 0;
				break;
			}
			BS->stall(1000);
		}
		print("emmc: CMD1 arg=%x -> %d ocr=%x\n", arg, err, status);
	}
	if (err != 0) {
		return -2;
	}

	err = emmc_command(2, 0, 2, 0, 0, resp);
	print("emmc: CMD2 -> %d cid=%x %x %x %x\n", err, resp[0], resp[1], resp[2], resp[3]);
	if (err != 0) {
		return -3;
	}
	err = emmc_command(3, 1u << 16, 1, 0, 0, resp);
	print("emmc: CMD3 -> %d status=%x\n", err, resp[0]);
	if (err != 0) {
		return -4;
	}
	err = emmc_command(9, 1u << 16, 2, 0, 0, resp);
	print("emmc: CMD9 -> %d csd=%x %x %x %x\n", err, resp[0], resp[1], resp[2], resp[3]);
	if (err != 0) {
		return -5;
	}
	err = emmc_command(7, 1u << 16, 7, 0, 0, resp);
	print("emmc: CMD7 -> %d status=%x\n", err, resp[0]);
	if (err != 0) {
		return -6;
	}
	err = emmc_command(13, 1u << 16, 1, 0, 0, resp);
	print("emmc: CMD13 -> %d status=%x state=%d\n", err, resp[0], (resp[0] >> 9) & 0xf);
	if (err != 0) {
		return -7;
	}
	return 0;
}

/* ---- the GPT: the diagnostic and the block service read the same one ---- */

static uint32_t emmc_last_lba;          /* the device's last LBA, from the GPT header */
static uint32_t emmc_userdata_lba;      /* where userdata begins, when the GPT has it */

static uint64_t
emmc_ms_since(uint64_t t0)
{
	return (READ_SYSREG(CNTVCT_EL0) - t0) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

static uint64_t
emmc_rate(uint64_t bytes, uint64_t ticks)
{
	return ticks == 0 ? 0 : (bytes * READ_SYSREG(CNTFRQ_EL0)) / ticks / 1000000;
}

/*
 * Read the GPT through the card itself.  verbose prints what it finds (the diagnostic); add puts
 * the non-empty entries into block_shared.part[], as partitions of unit (the page).  Returns how
 * many entries there were, -1 when the header or the table did not read, 0 when there is no GPT.
 * The header and the table are checked with their CRCs: a partition is only ever exposed from a
 * table that is all there.
 */
static int
emmc_read_gpt(int verbose, int add, int unit)
{
	static uint8_t hdr[512];
	static uint8_t table[128 * 512];
	uint8_t check[512];
	uint64_t entry_lba, alt_lba;
	uint32_t header_size, want, got, count, size, bytes;
	uint32_t found = 0;

	emmc_userdata_lba = 0;
	if (emmc_read_block(1, hdr) != 0) {
		print("emmc: LBA 1 read failed\n");
		return -1;
	}
	if (verbose) {
		print("emmc: LBA1 %x %x %x %x\n", le32(hdr), le32(hdr + 4), le32(hdr + 8), le32(hdr + 12));
	}
	if (memcmp(hdr, "EFI PART", 8) != 0) {
		print("emmc: no GPT signature at LBA 1\n");
		return 0;
	}
	header_size = le32(hdr + 12);
	want = le32(hdr + 16);
	alt_lba = le64(hdr + 32);
	entry_lba = le64(hdr + 72);
	count = le32(hdr + 80);
	size = le32(hdr + 84);
	if (verbose) {
		print("emmc: GPT entries at %x count %d size %d\n", entry_lba, (uint64_t)count, (uint64_t)size);
	}
	if (header_size < 92 || header_size > sizeof(check) || alt_lba > 0xffffffffULL ||
	    entry_lba > 0xffffffffULL || count == 0 || size < 128 || size > 512 || count > 128 ||
	    count * size > sizeof(table)) {
		print("emmc: GPT header is not usable\n");
		return -1;
	}
	memcpy(check, hdr, header_size);
	memset(check + 16, 0, 4);
	got = ufs_crc32(check, header_size);
	if (got != want) {
		print("emmc: GPT header CRC %x, not %x: no partition is used\n", (uint64_t)got, (uint64_t)want);
		return -1;
	}
	bytes = count * size;
	for (uint32_t off = 0; off < bytes; off += 512) {
		if (emmc_read_block((uint32_t)entry_lba + off / 512, table + off) != 0) {
			print("emmc: GPT entries read failed at %x\n", (uint64_t)off);
			return -1;
		}
	}
	want = le32(hdr + 88);
	got = ufs_crc32(table, bytes);
	if (got != want) {
		print("emmc: GPT table CRC %x, not %x: no partition is used\n", (uint64_t)got, (uint64_t)want);
		return -1;
	}
	emmc_last_lba = (uint32_t)alt_lba;
	for (uint32_t i = 0; i < count; i++) {
		uint8_t *e = table + i * size;
		char name[40];
		int empty = 1, n = 0;
		uint64_t first, last;

		for (int k = 0; k < 16; k++) {
			if (e[k] != 0) {
				empty = 0;
			}
		}
		if (empty) {
			continue;
		}
		for (int k = 0; k < 36 && n < (int)sizeof(name) - 1; k++) {
			uint16_t ch = (uint16_t)(e[56 + k * 2] | (e[57 + k * 2] << 8));

			if (ch == 0) {
				break;
			}
			name[n++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '_';
		}
		name[n] = 0;
		first = le64(e + 32);
		last = le64(e + 40);
		found++;
		if (verbose) {
			print("emmc: part %d %s %x..%x\n", (uint64_t)i, name, first, last);
		}
		if (str_eq(name, "userdata") && first <= 0xffffffffULL) {
			emmc_userdata_lba = (uint32_t)first;
		}
		if (!add || first == 0 || last < first || first > 0xffffffffULL || last > 0xffffffffULL) {
			continue;
		}
		if (block_shared.parts + 1 >= BLOCK_MAX_PARTS) {      /* keep one for the unit */
			print("emmc: the partitions do not fit the page\n");
			break;
		}
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];

		memset(bp, 0, sizeof(*bp));
		for (int k = 0; k < (int)sizeof(bp->name) - 1 && name[k] != 0; k++) {
			bp->name[k] = name[k];
		}
		bp->block_size = 512;
		bp->blocks = last - first + 1;
		bp->start_lba = first;
		bp->unit = (uint16_t)unit;
		bp->lun = 0;
		bp->flags = 0;
		block_bios[block_shared.parts] = NULL;
		block_shared.parts++;
	}
	return (int)found;
}

/*
 * The card as a unit of the block service, asked for by block_start().  The state LK left is
 * tried first, exactly as the diagnostic does; only if the card does not answer is it brought
 * up again (at 400 kHz, one bit, which is slow but works).  Returns the number of GPT entries
 * added, 0 when this is not the phone or the card did not answer.
 */
static int
emmc_page_start(int unit)
{
	uint32_t status = 0;
	int found;

	if (PLAT != &platform_a03s) {
		return 0;
	}
	/* LK's command interrupts are not ours; PIO, and the longest data timeout the controller has */
	emmc_write(EMMC_MSDC_INTEN, 0);
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
	emmc_write(EMMC_MSDC_CFG, emmc_read(EMMC_MSDC_CFG) | EMMC_CFG_PIO);
	emmc_write(EMMC_SDC_CFG, (emmc_read(EMMC_SDC_CFG) & ~(0xffu << 24)) | (0xffu << 24));
	if (emmc_command(13, 1u << 16, 1, 0, 0, &status) != 0) {
		print("emmc: LK's state did not answer; re-initialising the card at 400 kHz, 1 bit\n");
		if (emmc_reinit() != 0) {
			print("emmc: no card: the block service has nothing to read\n");
			return 0;
		}
	}
	found = emmc_read_gpt(0, 1, unit);
	if (found <= 0 || block_shared.parts >= BLOCK_MAX_PARTS) {
		return 0;
	}
	struct loader_block_part *bp = &block_shared.part[block_shared.parts];

	memset(bp, 0, sizeof(*bp));
	bp->name[0] = 'e', bp->name[1] = 'm', bp->name[2] = 'm', bp->name[3] = 'c', bp->name[4] = '0';
	bp->name[5] = 0;
	bp->block_size = 512;
	bp->blocks = (uint64_t)emmc_last_lba + 1;
	bp->start_lba = 0;
	bp->unit = (uint16_t)unit;
	bp->lun = 0;
	bp->flags = BLOCK_PART_UNIT;
	block_bios[block_shared.parts] = NULL;
	block_shared.parts++;
	print("emmc: %d partitions, unit %d, last LBA %x, read through MSDC0 at LK's clock\n",
	    (uint64_t)found, (uint64_t)unit, (uint64_t)emmc_last_lba);
	return found;
}

/* Read LBA 1 and, if it is a GPT, list its partitions and read userdata; CMD18 is tried too. */
static void
emmc_diag(void)
{
	static uint8_t hdr[512];
	uint32_t status = 0, lba;
	int err, kept = 1;

	if (PLAT != &platform_a03s) {
		print("emmc: not the phone this diagnostic is for\n");
		return;
	}
	/* LK's command interrupts are not ours: mask them before touching the controller. */
	emmc_write(EMMC_MSDC_INTEN, 0);
	emmc_write(EMMC_MSDC_INT, 0xffffffffu);
	emmc_dump("start");
	/* First ask the card in exactly the state LK left it in. */
	err = emmc_command(13, 1u << 16, 1, 0, 0, &status);
	print("emmc: LK-state CMD13 -> %d status=%x state=%d\n", (uint64_t)(int64_t)err, (uint64_t)status,
	    (uint64_t)((status >> 9) & 0xf));
	if (err != 0 || emmc_read_gpt(1, 0, 0) <= 0) {
		kept = 0;
		print(err == 0 ? "emmc: the card answers, but its GPT did not; re-initialising\n" :
		    "emmc: LK state did not answer; re-initialising the card\n");
		if (emmc_reinit() != 0) {
			print("emmc: TEST DONE (re-init failed)\n");
			return;
		}
		emmc_read_gpt(1, 0, 0);
	}
	lba = emmc_userdata_lba != 0 ? emmc_userdata_lba : 14524416;
	if (emmc_read_block(lba, hdr) == 0) {
		print("emmc: userdata LBA %x: %x %x %x %x %x %x %x %x\n", (uint64_t)lba,
		    le32(hdr), le32(hdr + 4), le32(hdr + 8), le32(hdr + 12),
		    le32(hdr + 16), le32(hdr + 20), le32(hdr + 24), le32(hdr + 28));
		print("emmc: userdata magic at +0x20 = %x (NXSB is 4253584e)\n", le32(hdr + 32));
	} else {
		print("emmc: userdata LBA %x read failed\n", (uint64_t)lba);
	}
	/* CMD18 at the same place: 64 KB, then 1 MB.  A failure falls back to CMD17 and says so. */
	print("emmc: CMD18 64 KB at %x\n", (uint64_t)lba);
	uint64_t t0 = READ_SYSREG(CNTVCT_EL0);

	err = emmc_read_blocks(lba, 128, block_buffer);
	print("emmc: CMD18 64 KB -> %d, %d ms\n", (uint64_t)(int64_t)err, emmc_ms_since(t0));
	print("emmc: CMD18 1 MB at %x\n", (uint64_t)lba);
	t0 = READ_SYSREG(CNTVCT_EL0);
	err = emmc_read_blocks(lba, 2048, block_buffer);
	print("emmc: CMD18 1 MB -> %d, %d ms, %d MB/s\n", (uint64_t)(int64_t)err, emmc_ms_since(t0),
	    emmc_rate(1u << 20, READ_SYSREG(CNTVCT_EL0) - t0));
	print("emmc: TEST DONE (%s)\n", kept ? "LK state kept" : "after re-init");
}

/* One request through the same page the kernel uses: partition-relative lba and count. */
static int
emmc_page_read(uint32_t part, uint64_t lba, uint64_t count, uint8_t *dst)
{
	static uint32_t seq;
	uint32_t want = ++seq;
	uint64_t bytes = count * block_shared.part[part].block_size;

	block_shared.ask.part = part;
	block_shared.ask.lba = lba;
	block_shared.ask.count = count;
	block_shared.ask.op = BLOCK_OP_READ;
	__asm__ volatile ("dmb sy" ::: "memory");
	block_shared.ask.request = want;
	__asm__ volatile ("dmb sy" ::: "memory");
	block_poll();
	if (block_shared.answer.answered != want) {
		return -1000;
	}
	if (block_shared.answer.status != 0) {
		return (int)block_shared.answer.status;
	}
	if (dst != NULL) {
		memcpy(dst, block_buffer, (size_t)bytes);
	}
	return 0;
}

/*
 * emmc_test=2: the whole way the kernel will read -- block_start() publishes the eMMC's GPT
 * partitions, and then requests are put in the page and served by block_poll(), exactly as the
 * kernel does.  Compare one block with a direct read, then measure 16 MB sequential and random
 * 4 KB and 64 KB.  The answer says whether this is worth booting from.
 */
static void
emmc_bench(void)
{
	static uint8_t direct[512], through[512];
	uint64_t t0, ticks, seed = 0x9e3779b97f4a7c15ULL;
	int part = -1, err = 0;

	if (PLAT != &platform_a03s) {
		print("emmc: not the phone this benchmark is for\n");
		return;
	}
	block_start();
	for (uint32_t i = 0; i < block_shared.parts; i++) {
		if (str_eq(block_shared.part[i].name, "userdata")) {
			part = (int)i;
		}
	}
	if (part < 0) {
		print("emmc: no userdata partition in the page; BENCH DONE\n");
		return;
	}
	print("emmc: page: %d partitions; userdata is %d blocks of %d from LBA %x\n",
	    (uint64_t)block_shared.parts - 1, block_shared.part[part].blocks,
	    (uint64_t)block_shared.part[part].block_size, block_shared.part[part].start_lba);
	if (emmc_userdata_lba != 0 && emmc_read_block(emmc_userdata_lba, direct) == 0 &&
	    emmc_page_read((uint32_t)part, 0, 1, through) == 0) {
		print("emmc: userdata block 0 through the page: %s\n",
		    memcmp(direct, through, 512) == 0 ? "the same as a direct read" : "DIFFERENT");
	} else {
		print("emmc: userdata block 0 could not be compared\n");
	}
	/* 16 MB sequential in 4 MB requests, the whole page path (PIO, flush of the buffer) */
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (uint32_t k = 0; k < 4; k++) {
		err = emmc_page_read((uint32_t)part, (uint64_t)k * 8192, 8192, NULL);
		if (err != 0) {
			break;
		}
	}
	ticks = READ_SYSREG(CNTVCT_EL0) - t0;
	print("emmc: 16 MB sequential through the page: %d, %d ms, %d MB/s\n", (uint64_t)(int64_t)err,
	    ticks * 1000 / READ_SYSREG(CNTFRQ_EL0), emmc_rate(16ull << 20, ticks));
	/* 256 random 4 KB */
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (uint32_t k = 0; k < 256; k++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		err = emmc_page_read((uint32_t)part, (seed >> 16) % (block_shared.part[part].blocks - 8), 8, NULL);
		if (err != 0) {
			break;
		}
	}
	ticks = READ_SYSREG(CNTVCT_EL0) - t0;
	print("emmc: 256 random 4 KB through the page: %d, %d ms, %d MB/s, %d us each\n",
	    (uint64_t)(int64_t)err, ticks * 1000 / READ_SYSREG(CNTFRQ_EL0), emmc_rate(1ull << 20, ticks),
	    ticks * 1000000 / READ_SYSREG(CNTFRQ_EL0) / 256);
	/* 64 random 64 KB */
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (uint32_t k = 0; k < 64; k++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		err = emmc_page_read((uint32_t)part, (seed >> 16) % (block_shared.part[part].blocks - 128), 128, NULL);
		if (err != 0) {
			break;
		}
	}
	ticks = READ_SYSREG(CNTVCT_EL0) - t0;
	print("emmc: 64 random 64 KB through the page: %d, %d ms, %d MB/s, %d us each\n",
	    (uint64_t)(int64_t)err, ticks * 1000 / READ_SYSREG(CNTFRQ_EL0), emmc_rate(4ull << 20, ticks),
	    ticks * 1000000 / READ_SYSREG(CNTFRQ_EL0) / 64);
	print("emmc: BENCH DONE\n");
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	/*
	 * Each run answers for itself.  The loader stays resident across the runs of a checkpoint
	 * scan, and the last run's reason (2, a checkpoint) must not be read as this run's answer
	 * when this one fails before the kernel is even entered.
	 */
	loader_ctx[CTX_LOG_PA] = 0;
	loader_ctx[CTX_LOG_LEN] = 0;
	loader_ctx[CTX_REASON] = 0;
	loader_ctx[CTX_ESR] = 0;
	loader_ctx[CTX_ELR] = 0;
	loader_ctx[CTX_FAR] = 0;
	loader_ctx[CTX_LR] = 0;
	loader_ctx[CTX_KSP] = 0;

	if (!parse_payload(&pi, 1) || (pi.kernel == NULL && pi.kernel_lz4 == NULL)) {
		print("no kernel in payload\n");
		return 0;
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}

	const uint8_t *kernel = pi.kernel;
	if (kernel == NULL) {
		/*
		 * `kernel.lz4`: an 8-byte uncompressed size followed by an LZ4
		 * legacy frame.  Expand it once into the reserved area; later
		 * runs (the loader stays resident) reuse the same image.
		 */
		uint64_t size = pi.kernel_lz4_size >= 8 ? le64(pi.kernel_lz4) : 0;
		uint64_t room = KERNEL_DECOMP_BASE - PAYLOAD_BASE;
		uint64_t payload_end = PAYLOAD_BASE + sizeof(struct payload_header) +
		    ((struct payload_header *)PAYLOAD_BASE)->size;
		static int decompressed;
		static uint64_t decompressed_size;

		if (size == 0 || size > 0xFFFFFFFFULL || size > PAYLOAD_MAX_SIZE - room) {
			print("kernel.lz4: bad uncompressed size %x\n", size);
			return 0;
		}
		if (payload_end > KERNEL_DECOMP_BASE) {
			print("kernel.lz4: payload overlaps the decompression buffer\n");
			return 0;
		}
		if (!decompressed || decompressed_size != size) {
			print("kernel.lz4: %x compressed -> %x at %x\n",
			    (uint64_t)pi.kernel_lz4_size - 8, size, KERNEL_DECOMP_BASE);
			if (!lz4_legacy_decompress(pi.kernel_lz4 + 8, pi.kernel_lz4_size - 8,
			    (uint8_t *)KERNEL_DECOMP_BASE, (uint32_t)size)) {
				print("kernel.lz4: decompression failed\n");
				return 0;
			}
			decompressed = 1;
			decompressed_size = size;
		}
		kernel = (const uint8_t *)KERNEL_DECOMP_BASE;
	}
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	/*
	 * A prelinked kernelcache puts kexts below the kernel text: on iOS 16
	 * __PRELINK_TEXT starts at base+0xDD0000 while __TEXT is at
	 * base+0x3004000.  Accept anything at or above virtBase, but the entry
	 * point itself must be in the kernel's own text region.
	 */
	if (vm_lo < KERNEL_VIRT_BASE || entry_va < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET) {
		print("unexpected kernel layout\n");
		return 0;
	}
	/* the forced window must be known before the window is chosen (the args are parsed below) */
	if (pi.cmdline != NULL) {
		for (const char *c = pi.cmdline; *c; c++) {
			if (starts_with(c, "win_base=")) {
				const char *v = c + 9;

				if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
					v += 2;
				}
				forced_win_base = parse_number(v, 16);
			}
			if (starts_with(c, "win_size=")) {
				const char *v = c + 9;

				if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
					v += 2;
				}
				forced_win_size = parse_number(v, 16);
			}
		}
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;
	/*
	 * One payload image that holds both disks: `rdsplit` is where the second begins.  The first
	 * is md0, the second md1 -- a flash boot has no other way to carry two (no host upload).
	 */
	int rd_split = 0;

	if (pi.ramdisk_split != 0) {
		if (rd_size == 0 || pi.ramdisk_split >= rd_size ||
		    (pi.ramdisk_split & (PAGE_SIZE_MAX - 1)) != 0) {
			print("rdsplit %x is not inside the %x-byte payload ramdisk\n",
			    pi.ramdisk_split, (uint64_t)pi.ramdisk_size);
			return 0;
		}
		if (pi.resident_rd != 0 || resident_rd_size != 0) {
			print("rdsplit cannot be combined with a resident image\n");
			return 0;
		}
		rd1_pa = rd_pa + pi.ramdisk_split;
		rd1_size = rd_size - pi.ramdisk_split;
		rd_size = pi.ramdisk_split;
		rd_split = 1;
	}

	if (pi.resident_rd != 0) {
		resident_rd_size = pi.resident_rd;
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/*
	 * A core that ran the previous kernel is parked in the loader with its caches off.  When it
	 * is woken for this run it must drop every cached copy of what this kernel wrote: the
	 * static region up to topOfKernelData and the margin the RAM check above already reserves
	 * for the kernel's early allocations.  The window itself never changes during a scan, but
	 * these are cheap to set again and the parked core reads them with its caches off.
	 */
	other_cpu_window_start = phys_base;
	other_cpu_window_end = top + 0x4000000ULL;
	if (other_cpu_window_end > win_base + win_size) {
		other_cpu_window_end = win_base + win_size;
	}
	other_cpu_inval_step = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_window_start, (uint64_t)(uintptr_t)&other_cpu_window_start + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_window_end, (uint64_t)(uintptr_t)&other_cpu_window_end + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_inval_step, (uint64_t)(uintptr_t)&other_cpu_inval_step + 8);

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		uint64_t copy = rd_split ? rd_size + rd1_size : rd_size;

		memset((void *)rd_pa, 0, copy);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
		if (rd_split) {
			print("ramdisk1 at pa %x size %x (split at %x)\n", rd1_pa, rd1_size,
			    pi.ramdisk_split);
		}
	}
	if (rd1_size != 0 && !rd_split) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "clusters=N" there too: how many processor clusters the kernel may have (default 1) */
	int clusters_asked = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (bsd/dev/redwood_torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is iOS's (bsd/dev/redwood_camera.c) */
	int camera_requested = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		if (starts_with(c, "clusters=") && (c == pi.cmdline || c[-1] == ' ') &&
		    c[9] >= '1' && c[9] <= '9' && (c[10] == 0 || c[10] == ' ')) {
			clusters_asked = c[9] - '0';
		}
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '2' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '9';
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
		if (starts_with(c, "win_base=")) {
			const char *v = c + 9;

			if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
				v += 2;
			}
			forced_win_base = parse_number(v, 16);
		}
		if (starts_with(c, "win_size=")) {
			const char *v = c + 9;

			if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
				v += 2;
			}
			forced_win_size = parse_number(v, 16);
		}
		if (starts_with(c, "cycle=")) {
			uint64_t v = 0;

			for (c += 6; *c >= '0' && *c <= '9'; c++) {
				v = v * 10 + (uint64_t)(*c - '0');
			}
			if (v >= 30) {
				cycle_seconds = v;
			}
		}
		if (starts_with(c, "late_note=")) {
			uint64_t v = 0;

			for (c += 10; *c >= '0' && *c <= '9'; c++) {
				v = v * 10 + (uint64_t)(*c - '0');
			}
			if (v >= 60 && v <= 86400) {
				late_note_seconds = v;
			}
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		/*
		 * Bit 0 tells XNU this is a physical address and must be mapped
		 * (initialize_screen() in osfmk/console/video_console.c). Without
		 * it the kernel would take the value for a virtual address and the
		 * text console would write to an unmapped physical page.
		 */
		ba->video.v_baseAddr = SCREEN.base | 1;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0, own_debug = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
		own_debug |= starts_with(c, "debug=") && (c == pi.cmdline || c[-1] == ' ');
	}
	/*
	 * XNU's boot-argument parser takes the first occurrence of a name, so a
	 * "debug=" in the payload (debug=0x100 keeps the phone alive on a panic)
	 * must not be preceded by our debug=0x8.
	 */
	const char *base_cmdline = "";
	if (PLAT->uart != 0 && !own_serial) {
		base_cmdline = "serial=3 ";
	}
	if (!own_debug) {
		base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	}
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	for (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "HAKIOSA03S01", 32);
	dt_prop_padded(&w, "mlb-serial-number", "HAKIOSA03S01MLB01", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk; a split payload image is the other way
	 * round: its first part is md0 and its second md1.
	 */
	if (rd_split) {
		uint64_t first[2] = { rd_pa, rd_size }, second[2] = { rd1_pa, rd1_size };

		dt_prop(&w, "RAMDisk", first, sizeof(first));
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	} else if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (!rd_split && rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes iOS play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it iOS looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = (int)(kernel_mpidr >> plat->core_shift);

	if (more_cores) {
		int per = plat->cluster_cores != 0 ? plat->cluster_cores : plat->cores;
		int max_clusters = plat->cluster_cores != 0 ? plat->clusters : 1;
		int clusters = 1;
		int boot_aff0 = boot_core & (per - 1);

		/* "clusters=N": how many clusters to offer the kernel (default 1: the boot core's). */
		if (clusters_asked >= 1) {
			clusters = clusters_asked < max_clusters ? clusters_asked : max_clusters;
		}
		print("loader: processor list (core %x first, %d cluster(s), %d each)\n", (uint32_t)boot_core, clusters, per);
		for (int cluster = 0; cluster < clusters; cluster++) {
			for (int i = 0; i < per; i++) {
				int core = (boot_aff0 - i + per) % per;
				uint64_t mpidr = ((uint64_t)cluster << 8) | ((uint64_t)core << plat->core_shift);
				int64_t state;

				if (cpu_count >= 8 || mpidr == kernel_mpidr || mpidr == this_mpidr) {
					continue;
				}
				/*
				 * Only cores of the boot cluster are asked about: a firmware that does not
				 * know the second one may answer by hanging rather than with an error, and
				 * the kernel starts those itself (PSCI CPU_ON) where a failure is reported.
				 */
				if (cluster != 0) {
					cpu_mpidr[cpu_count++] = mpidr;
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (plat->small_cores & (1u << ((cpu_mpidr[i] >> plat->core_shift) & 31))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		print("loader: starting the kernel on core %x (window %x size %x)\n", (uint32_t)kernel_mpidr,
		    win_base, win_size);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				/* Not a returned kernel: a scan must stop here instead of taking a stale reason for an answer. */
				return 0;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into darwin, %d bytes, read back the same; %d refused\n", block_writes,
		    block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	flush_dcache((uint64_t)(uintptr_t)&other_cpu_parked, (uint64_t)(uintptr_t)&other_cpu_parked + 8);
	int parked_core = other_cpu_parked == 1;
	for (int i = 0; i < cpu_count; i++) {
		int64_t state;

		if (cpu_mpidr[i] == this_mpidr) {
			continue;       /* this is the core the loader is running on: it is on by definition */
		}
		if (cpu_mpidr[i] == kernel_mpidr && parked_core) {
			continue;       /* the kernel's core stays on, parked in the loader, between runs */
		}
		state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);
		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	if (checkpoint != CHECKPOINT_SELFTEST) {
		/* keep the end of what the kernel said on the screen, where the phone can show it */
		print_kernel_log_tail(klog, klog_len, 6000);
		print_init_window(klog, klog_len);
		print_log_summary();
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
			}
			if (got == total) {
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
/* HACKIOS_RAMFB_HEIGHT: QEMU's ramfb refuses a mode taller than 2048 (bochs-vbe's
 * VBE_DISPI_MAX_YRES), so the panel is asked for 2048 rows and the kernel scales the
 * 1125x2436 display into it -- the same thing it does on a real phone. */
#define RAMFB_HEIGHT    2048
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * RAMFB_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(RAMFB_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	if (dma.control != 0) {
		/* HACKIOS: this is the value we wrote ourselves; nothing writes back into it, so it
		 * says nothing about whether the request worked. QEMU's ramfb refuses a mode taller
		 * than 2048 (VBE_DISPI_MAX_YRES), and the mode it takes is the one in the panel. */
		print("ramfb: control %x after the request (the mode is taken as it stands)\n",
		    (uint64_t)be32(dma.control));
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = RAMFB_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	print("== XNULOADER v4 (live log) ==\n");
	print("boot: TOPRGU+0x24 = %x\n", (uint64_t)*(volatile uint32_t *)(uintptr_t) 0x10007024u);
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);

	install_exception_hook();
	wdt_arm();

	print("payload area %x..%x\n", PAYLOAD_BASE, PAYLOAD_BASE + PAYLOAD_MAX_SIZE);
	int have_payload = parse_payload(&pi, 1);
	print("payload: %s\n", have_payload ? "present" : "none");
	if (have_payload) {
		for (const char *c = pi.cmdline; *c; c++) {
			if ((c == pi.cmdline || c[-1] == ' ') && starts_with(c, "emmc_test=") &&
			    (c[10] == '1' || c[10] == '2') && (c[11] == 0 || c[11] == ' ')) {
				if (c[10] == '2') {
					emmc_bench();
				} else {
					emmc_diag();
				}
				print("emmc: halted; power-cycle the phone to continue\n");
				for (;;) {
					BS->stall(1000000);
				}
			}
		}
	}
	if (have_payload) {
		const char *c = pi.cmdline;

		for (; *c; c++) {
			if ((c == pi.cmdline || c[-1] == ' ') && starts_with(c, "memmap=1")) {
				print_memory_map();
				break;
			}
		}
	}
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/*
	 * One-shot mode: run the kernel that came with the image.  During
	 * bring-up, return to the firmware even when the kernel refuses to
	 * start or comes back with a panic: the UEFI hold then keeps its log
	 * on screen instead of resetting the phone.
	 */
	if (have_payload) {
		if (pi.checkpoint_scan != NULL && pi.checkpoint_scan[0] != 0) {
			/*
			 * Bring-up scan: run the kernel once per checkpoint, in
			 * increasing order.  The first run that does not come back
			 * leaves "SCAN: trying checkpoint n" and the last
			 * "STOPPED AT CHECKPOINT" on the screen.
			 */
			const char *s = pi.checkpoint_scan;
			int completed = 1;

			while (*s != 0) {
				while (*s == ' ' || *s == ',') {
					s++;
				}
				if (*s < '0' || *s > '9') {
					break;
				}
				uint64_t cp = parse_number(s, 10);

				while (*s >= '0' && *s <= '9') {
					s++;
				}
				print("SCAN: trying checkpoint %d\n", cp);
				if (!run_kernel(1, cp)) {
					print("SCAN: checkpoint %d did not start\n", cp);
					completed = 0;
					break;
				}
				if (cp != CHECKPOINT_SELFTEST && loader_ctx[CTX_REASON] != 2) {
					print("SCAN: stopped at checkpoint %d, reason %d\n", cp, loader_ctx[CTX_REASON]);
					completed = 0;
					break;
				}
				print("SCAN: checkpoint %d returned\n", cp);
			}
			/* Keep the results on the screen: the UEFI menu would clear it. */
			print(completed ? "SCAN COMPLETE\n" : "SCAN STOPPED\n");
			for (;;) {
				BS->stall(1000000);
			}
		} else if (!run_kernel(0, 0)) {
			print("the kernel did not run: back to UEFI\n");
			return 0;
		}
		print("kernel returned: back to UEFI\n");
	}

	/*
	 * Bring-up: there is no payload yet (the phone's UEFI has no USB or
	 * storage driver to receive one).  Return to the firmware instead of
	 * resetting, so the hand-off can be seen and the shell stays usable.
	 */
	print("no payload: returning to UEFI\n");
	return 0;
}
