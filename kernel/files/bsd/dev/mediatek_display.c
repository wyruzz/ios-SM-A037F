/*
 * MediaTek MT6765 display bring-up indicator.
 *
 * The display pipeline (DSI0 <- RDMA0 <- COLOR0 <- OVL0) is left running by
 * the UEFI firmware: its DisplayDxe programs the panel and the OVL0 layer to
 * scan out the boot framebuffer.  XNU only has to write pixels into that
 * framebuffer; there is no reason to reset or reprogram any display register
 * here (doing so on the A03s turns the panel off).
 *
 * The kernel's text console already knows where the framebuffer is: vinfo
 * (osfmk/console/video_console.c) carries its kernel virtual address, size,
 * stride and depth.  When the loader hands XNU a physical address, vinfo may
 * still hold that physical address; in that case map it once through
 * ml_io_map_wcomb(), which goes through kmem_alloc()/pmap_map() properly,
 * unlike a raw pmap_enter() into a kmem_alloc()ed range.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <mach/vm_param.h>
#include <console/video_console.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>

#include "mediatek_display.h"

extern struct vc_info vinfo;
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define MTK_FB_FALLBACK_PHYS    0x7C250000UL    /* LK/UEFI boot framebuffer on the A03s */
#define MTK_FB_FALLBACK_WIDTH   720
#define MTK_FB_FALLBACK_HEIGHT  1600
#define MTK_FB_FALLBACK_PITCH   (MTK_FB_FALLBACK_WIDTH * 4)
#define MTK_FB_MAX_DIM          4096

static uint32_t *g_fb;
static uint32_t  g_fb_width;
static uint32_t  g_fb_height;
static uint32_t  g_fb_pitch;
static boolean_t g_display_inited = FALSE;

static boolean_t
mtk_fb_plausible(uint64_t base, uint32_t width, uint32_t height, uint32_t pitch)
{
	if (base == 0) {
		return FALSE;
	}
	if (width < 64 || height < 64 ||
	    width > MTK_FB_MAX_DIM || height > MTK_FB_MAX_DIM) {
		return FALSE;
	}
	if (pitch < width * 4 || pitch > 16384) {
		return FALSE;
	}
	return TRUE;
}

static boolean_t
mtk_is_kernel_va(vm_offset_t addr)
{
	return (addr >> 48) == 0xffff;
}

void
mtk_display_paint(uint32_t color)
{
	uint32_t x, y;

	if (g_fb == NULL || g_fb_width == 0 || g_fb_height == 0) {
		return;
	}

	for (y = 0; y < g_fb_height; y++) {
		volatile uint32_t *row =
		    (volatile uint32_t *)((uint8_t *)g_fb + (size_t)y * g_fb_pitch);

		for (x = 0; x < g_fb_width; x++) {
			row[x] = color;
		}
	}
}

int
mtk_display_init(uint32_t color)
{
	vm_offset_t base;
	vm_size_t   size;
	uint32_t    width, height, pitch;

	if (g_display_inited) {
		mtk_display_paint(color);
		return 0;
	}

	printf("EXECDBG: mtk_display: vinfo base=0x%lx w=%u h=%u pitch=%u depth=%u phys=0x%llx\n",
	    (unsigned long)vinfo.v_baseaddr, vinfo.v_width, vinfo.v_height,
	    vinfo.v_rowbytes, vinfo.v_depth,
	    (unsigned long long)vinfo.v_physaddr);

	base   = (vm_offset_t)vinfo.v_baseaddr;
	width  = vinfo.v_width;
	height = vinfo.v_height;
	pitch  = vinfo.v_rowbytes;

	if (!mtk_fb_plausible(base, width, height, pitch)) {
		printf("EXECDBG: mtk_display: vinfo unusable, using A03s fallback\n");
		base   = MTK_FB_FALLBACK_PHYS;
		width  = MTK_FB_FALLBACK_WIDTH;
		height = MTK_FB_FALLBACK_HEIGHT;
		pitch  = MTK_FB_FALLBACK_PITCH;
	}

	/*
	 * If vinfo still carries a physical address (the loader did not mark it
	 * with the low bit), map it the safe way: kmem_alloc() + pmap_map().
	 */
	if (!mtk_is_kernel_va(base)) {
		vm_offset_t mapped;

		size = round_page((vm_size_t)pitch * height);
		mapped = ml_io_map_wcomb(base, size);
		printf("EXECDBG: mtk_display: mapped phys 0x%lx size=%llu -> va 0x%lx\n",
		    (unsigned long)base, (unsigned long long)size,
		    (unsigned long)mapped);
		if (mapped == 0) {
			printf("EXECDBG: mtk_display: ml_io_map_wcomb failed\n");
			return -1;
		}
		base = mapped;
	}

	g_fb        = (uint32_t *)base;
	g_fb_width  = width;
	g_fb_height = height;
	g_fb_pitch  = pitch;

	/*
	 * The console has been drawing the kernel log into this same buffer.
	 * Switch it to graphics mode and then stop it from drawing altogether:
	 * every later printf() would paint over the indicator, and scrolling
	 * this 720x1600 buffer for each log line is very slow on one CPU.
	 * The log itself keeps going to msgbuf; the loader reads it back from
	 * RAM after a panic.
	 */
	/* diagnostic: keep the kernel console mode untouched */

	printf("EXECDBG: mtk_display: painting %ux%u, pitch %u, color 0x%08X\n",
	    width, height, pitch, color);
	mtk_display_paint(color);
	/* diagnostic: keep the console so launchd output is visible */

	g_display_inited = TRUE;
	return 0;
}
