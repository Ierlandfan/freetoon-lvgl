/**
 * @file fbdev.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "fbdev.h"
#if USE_FBDEV || USE_BSD_FBDEV

#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

#if USE_BSD_FBDEV
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/consio.h>
#include <sys/fbio.h>
#else  /* USE_BSD_FBDEV */
#include <linux/fb.h>
#endif /* USE_BSD_FBDEV */

/*********************
 *      DEFINES
 *********************/
#ifndef FBDEV_PATH
#define FBDEV_PATH  "/dev/fb0"
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      STRUCTURES
 **********************/

struct bsd_fb_var_info{
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t xres;
    uint32_t yres;
    int bits_per_pixel;
 };

struct bsd_fb_fix_info{
    long int line_length;
    long int smem_len;
};

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/
#if USE_BSD_FBDEV
static struct bsd_fb_var_info vinfo;
static struct bsd_fb_fix_info finfo;
#else
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
#endif /* USE_BSD_FBDEV */
static char *fbp = 0;
static long int screensize = 0;
static int fbfd = 0;

/**********************
 *      MACROS
 **********************/

#if USE_BSD_FBDEV
#define FBIOBLANK FBIO_BLANK
#endif /* USE_BSD_FBDEV */

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void fbdev_init(void)
{
    // Open the file for reading and writing
    fbfd = open(FBDEV_PATH, O_RDWR);
    if(fbfd == -1) {
        perror("Error: cannot open framebuffer device");
        return;
    }
    LV_LOG_INFO("The framebuffer device was opened successfully");

#if FBDEV_DISPLAY_POWER_ON
    // Make sure that the display is on.
    if (ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
        perror("ioctl(FBIOBLANK)");
        return;
    }
#endif /* FBDEV_DISPLAY_POWER_ON */

#if USE_BSD_FBDEV
    struct fbtype fb;
    unsigned line_length;

    //Get fb type
    if (ioctl(fbfd, FBIOGTYPE, &fb) != 0) {
        perror("ioctl(FBIOGTYPE)");
        return;
    }

    //Get screen width
    if (ioctl(fbfd, FBIO_GETLINEWIDTH, &line_length) != 0) {
        perror("ioctl(FBIO_GETLINEWIDTH)");
        return;
    }

    vinfo.xres = (unsigned) fb.fb_width;
    vinfo.yres = (unsigned) fb.fb_height;
    vinfo.bits_per_pixel = fb.fb_depth;
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;
    finfo.line_length = line_length;
    finfo.smem_len = finfo.line_length * vinfo.yres;
#else /* USE_BSD_FBDEV */

    // Get fixed screen information
    if(ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("Error reading fixed information");
        return;
    }

    // Get variable screen information
    if(ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("Error reading variable information");
        return;
    }
#endif /* USE_BSD_FBDEV */

    LV_LOG_INFO("%dx%d, %dbpp", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    // Figure out the size of the screen in bytes
    screensize =  finfo.smem_len; //finfo.line_length * vinfo.yres;    

    // Map the device to memory
    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if((intptr_t)fbp == -1) {
        perror("Error: failed to map framebuffer device to memory");
        return;
    }

    // Don't initialise the memory to retain what's currently displayed / avoid clearing the screen.
    // This is important for applications that only draw to a subsection of the full framebuffer.

    LV_LOG_INFO("The framebuffer device was mapped to memory successfully");
}

void fbdev_exit(void)
{
    close(fbfd);
}

/* ---- Toon 1: rectangular cutout for live video on /dev/fb0 ------------
 * vpu_stream writes the doorbell feed straight to fb0 inside a fixed rect
 * (fb0 + cutout mode, video_overlay=0). Without skipping that rect here,
 * every LVGL flush would clobber the video. With the cutout active, each
 * overlapping row is written in up to two segments (left/right of the
 * cutout) and the middle is left for vpu_stream. toonui (video.c) sets and
 * clears it via fbdev_set_cutout/clear_cutout. (Unused in overlay mode,
 * where the LCDC composites a separate plane.) */
static struct { int active; int32_t x1, y1, x2, y2; } g_fbcut = {0};

void fbdev_set_cutout(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    g_fbcut.x1 = x1; g_fbcut.y1 = y1;
    g_fbcut.x2 = x2; g_fbcut.y2 = y2;
    g_fbcut.active = 1;
}
void fbdev_clear_cutout(void) { g_fbcut.active = 0; }

/* Write up to two segments of one LVGL row, skipping the cutout columns. */
static inline void fbflush_row(uint8_t *dst, const uint8_t *src,
                               int32_t x_start, int32_t count, int32_t y,
                               int32_t bpp_bytes)
{
    if (!g_fbcut.active || y < g_fbcut.y1 || y > g_fbcut.y2) {
        memcpy(dst, src, (size_t)count * bpp_bytes); return;
    }
    int32_t x_end = x_start + count - 1;
    int32_t cx1 = g_fbcut.x1, cx2 = g_fbcut.x2;
    if (x_end < cx1 || x_start > cx2) {
        memcpy(dst, src, (size_t)count * bpp_bytes); return;
    }
    if (x_start < cx1) {
        int32_t le = (cx1 - 1 < x_end ? cx1 - 1 : x_end);
        memcpy(dst, src, (size_t)(le - x_start + 1) * bpp_bytes);
    }
    if (x_end > cx2) {
        int32_t rs = (cx2 + 1 > x_start ? cx2 + 1 : x_start);
        int32_t off = (rs - x_start) * bpp_bytes;
        memcpy(dst + off, src + off, (size_t)(x_end - rs + 1) * bpp_bytes);
    }
}

/**
 * Flush a buffer to the marked area
 * @param drv pointer to driver where this function belongs
 * @param area an area where to copy `color_p`
 * @param color_p an array of pixels to copy to the `area` part of the screen
 */
void fbdev_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p)
{
    if(fbp == NULL ||
            area->x2 < 0 ||
            area->y2 < 0 ||
            area->x1 > (int32_t)vinfo.xres - 1 ||
            area->y1 > (int32_t)vinfo.yres - 1) {
        lv_disp_flush_ready(drv);
        return;
    }

    /*Truncate the area to the screen*/
    int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t act_x2 = area->x2 > (int32_t)vinfo.xres - 1 ? (int32_t)vinfo.xres - 1 : area->x2;
    int32_t act_y2 = area->y2 > (int32_t)vinfo.yres - 1 ? (int32_t)vinfo.yres - 1 : area->y2;


    lv_coord_t w = (act_x2 - act_x1 + 1);

    /* Perf probe (env TOONUI_PERF=1): measure raw fb write bandwidth so we can
     * tell flush-bound from render-bound. The i.MX27 fb is dma_alloc_writecombine
     * (imxfb.c), so this should be ~fast; if it is, the 200ms/frame is render. */
    static int fperf = -1;
    struct timespec ft0;
    if (fperf < 0) fperf = (getenv("TOONUI_PERF") != NULL);
    if (fperf) clock_gettime(CLOCK_MONOTONIC, &ft0);

    long int location = 0;
    long int byte_location = 0;
    unsigned char bit_location = 0;

    /*32 or 24 bit per pixel*/
    if(vinfo.bits_per_pixel == 32 || vinfo.bits_per_pixel == 24) {
        uint32_t * fbp32 = (uint32_t *)fbp;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {
            location = (act_x1 + vinfo.xoffset) + (y + vinfo.yoffset) * finfo.line_length / 4;
            fbflush_row((uint8_t *)&fbp32[location], (const uint8_t *)color_p,
                        act_x1, act_x2 - act_x1 + 1, y, 4);
            color_p += w;
        }
    }
    /*16 bit per pixel*/
    else if(vinfo.bits_per_pixel == 16) {
        uint16_t * fbp16 = (uint16_t *)fbp;
        int32_t y;
        /* Fast path: a full-width area with no padding (line stride == width)
         * and no cutout overlapping these rows is one contiguous block in both
         * src and dst — copy it in a single memcpy instead of one per row.
         * This is the common scroll / full-refresh case. */
        if(act_x1 == 0 && act_x2 == (int32_t)vinfo.xres - 1 && vinfo.xoffset == 0 &&
           finfo.line_length == vinfo.xres * 2u &&
           (!g_fbcut.active || act_y2 < g_fbcut.y1 || act_y1 > g_fbcut.y2)) {
            location = (act_y1 + vinfo.yoffset) * (finfo.line_length / 2);
            memcpy(&fbp16[location], color_p, (size_t)w * (act_y2 - act_y1 + 1) * 2);
        } else {
            for(y = act_y1; y <= act_y2; y++) {
                location = (act_x1 + vinfo.xoffset) + (y + vinfo.yoffset) * finfo.line_length / 2;
                fbflush_row((uint8_t *)&fbp16[location], (const uint8_t *)color_p,
                            act_x1, act_x2 - act_x1 + 1, y, 2);
                color_p += w;
            }
        }
    }
    /*8 bit per pixel*/
    else if(vinfo.bits_per_pixel == 8) {
        uint8_t * fbp8 = (uint8_t *)fbp;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {
            location = (act_x1 + vinfo.xoffset) + (y + vinfo.yoffset) * finfo.line_length;
            memcpy(&fbp8[location], (uint32_t *)color_p, (act_x2 - act_x1 + 1));
            color_p += w;
        }
    }
    /*1 bit per pixel*/
    else if(vinfo.bits_per_pixel == 1) {
        uint8_t * fbp8 = (uint8_t *)fbp;
        int32_t x;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {
            for(x = act_x1; x <= act_x2; x++) {
                location = (x + vinfo.xoffset) + (y + vinfo.yoffset) * vinfo.xres;
                byte_location = location / 8; /* find the byte we need to change */
                bit_location = location % 8; /* inside the byte found, find the bit we need to change */
                fbp8[byte_location] &= ~(((uint8_t)(1)) << bit_location);
                fbp8[byte_location] |= ((uint8_t)(color_p->full)) << bit_location;
                color_p++;
            }

            color_p += area->x2 - act_x2;
        }
    } else {
        /*Not supported bit per pixel*/
    }

    //May be some direct update command is required
    //ret = ioctl(state->fd, FBIO_UPDATE, (unsigned long)((uintptr_t)rect));

    if(fperf) {
        struct timespec ft1; clock_gettime(CLOCK_MONOTONIC, &ft1);
        static double fbytes = 0, fns = 0; static uint32_t flast = 0;
        static int fcnt = 0, fmaxw = 0, fmaxh = 0; static long fmaxpx = 0, ftotpx = 0;
        int fh = act_y2 - act_y1 + 1;
        fbytes += (double)w * fh * (vinfo.bits_per_pixel / 8);
        fns    += (ft1.tv_sec - ft0.tv_sec) * 1e9 + (ft1.tv_nsec - ft0.tv_nsec);
        fcnt++;
        ftotpx += (long)w * fh;
        if((long)w * fh > fmaxpx) { fmaxpx = (long)w * fh; fmaxw = w; fmaxh = fh; }
        uint32_t fnow = (uint32_t)(ft1.tv_sec * 1000u + ft1.tv_nsec / 1000000u);
        if(fnow - flast > 2000 && fns > 1e6) {
            double bps = fbytes / (fns / 1e9);
            /* avg redraw is the key number: ~384k px => EVERY frame redraws the
             * whole screen (the invalidation overflow); small avg => only the
             * scrolled area is redrawn (fixed). max is the worst single frame. */
            fprintf(stderr, "[perf] fb flush: %.0f MB/s; %d flushes/2s; "
                    "redraw avg=%ldk px, max=%dx%d (%ldk px)\n",
                    bps / (1024 * 1024), fcnt, ftotpx / (fcnt > 0 ? fcnt : 1) / 1000,
                    fmaxw, fmaxh, fmaxpx / 1000);
            flast = fnow; fbytes = 0; fns = 0; fcnt = 0; fmaxw = fmaxh = 0; fmaxpx = 0; ftotpx = 0;
        }
    }

    lv_disp_flush_ready(drv);
}

void fbdev_get_sizes(uint32_t *width, uint32_t *height, uint32_t *dpi) {
    if (width)
        *width = vinfo.xres;

    if (height)
        *height = vinfo.yres;

    if (dpi && vinfo.height)
        *dpi = DIV_ROUND_UP(vinfo.xres * 254, vinfo.width * 10);
}

void fbdev_set_offset(uint32_t xoffset, uint32_t yoffset) {
    vinfo.xoffset = xoffset;
    vinfo.yoffset = yoffset;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif
