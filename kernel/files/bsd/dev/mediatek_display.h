/*
 * MediaTek MT6765 display bring-up indicator for the Samsung Galaxy A03s.
 *
 * The display hardware is already running when XNU starts: the UEFI
 * DisplayDxe leaves DSI0/OVL0 scanning out the boot framebuffer.  This
 * driver only fills that framebuffer with a solid colour.
 */

#ifndef _MEDIATEK_DISPLAY_H_
#define _MEDIATEK_DISPLAY_H_

#include <kern/kern_types.h>

/* Colours in the framebuffer's native 32-bit ARGB8888/BGRA layout. */
#define COLOR_WHITE             0xFFFFFFFF
#define COLOR_BLACK             0xFF000000
#define COLOR_RED               0xFFFF0000
#define COLOR_GREEN             0xFF00FF00
#define COLOR_BLUE              0xFF0000FF

/*
 * mtk_display_init - locate the boot framebuffer and paint it with a solid
 * colour.  Returns 0 on success, nonzero if no usable framebuffer was found.
 */
int mtk_display_init(uint32_t color);

/*
 * mtk_display_paint - fill the framebuffer with a solid colour.
 */
void mtk_display_paint(uint32_t color);

#endif /* _MEDIATEK_DISPLAY_H_ */
