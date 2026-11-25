/*
 * mm2_touch.h - user-mode decoder for Magic Mouse 2 report 0x12.
 *
 * Header-only C, no dependencies. Protocol facts follow the Linux kernel's
 * drivers/hid/hid-magicmouse.c (the 0x12 branch of magicmouse_raw_event and
 * magicmouse_emit_touch); this is an independent implementation.
 *
 * `report` points at the report ID byte, i.e. MM2_FRAME.Report.
 *
 *   [0]      0x12 report ID
 *   [1]      buttons (bit0 left, bit1 right)
 *   [2..3]   relative X (int16 LE)
 *   [4..5]   relative Y (int16 LE)
 *   [6..13]  header, undocumented (timestamp etc.)
 *   [14..]   N touches, 8 bytes each; total length = 14 + 8*N
 *
 * Touch record t[0..7]:
 *   x        = sign-extended 12 bits: t[0] | (t[1] & 0x0F) << 8
 *   y        = -(sign-extended 12 bits: t[1] >> 4 | t[2] << 4)   (up is +)
 *   major    = t[3]            touch ellipse, major axis
 *   minor    = t[4]            touch ellipse, minor axis
 *   size     = t[5] & 0x3F
 *   id       = (t[6] << 2 | t[5] >> 6) & 0x0F
 *   orient   = (t[6] >> 2) - 32
 *   state    = t[7] & 0xF0     0x00 none, 0x30 start, 0x40 drag; other
 *                              values are transitional (not documented by
 *                              Linux; log them with rawtouch to learn them)
 *
 * Linux uses x in [-1100, 1258], y in [-1589, 2047] as the axis ranges.
 */
#pragma once
#include <stdint.h>

#define MM2_REPORT_ID            0x12
#define MM2_HEADER_BYTES         14
#define MM2_TOUCH_BYTES          8
#define MM2_MAX_TOUCHES          15

#define MM2_X_MIN   (-1100)
#define MM2_X_MAX   1258
#define MM2_Y_MIN   (-1589)
#define MM2_Y_MAX   2047

#define MM2_TOUCH_STATE_MASK     0xF0
#define MM2_TOUCH_STATE_NONE     0x00
#define MM2_TOUCH_STATE_START    0x30
#define MM2_TOUCH_STATE_DRAG     0x40

typedef struct mm2_touch {
    int id;
    int x, y;
    int touch_major, touch_minor;
    int size;
    int orientation;
    int state;   /* raw t[7] & 0xF0 */
    int down;    /* state != NONE, as in Linux */
} mm2_touch;

typedef struct mm2_report {
    int buttons;
    int dx, dy;
    int touch_count;
    mm2_touch touches[MM2_MAX_TOUCHES];
} mm2_report;

static inline int mm2_sext(int v, int bits)
{
    int shift = (int)(sizeof(int) * 8) - bits;
    return (int)((unsigned)v << shift) >> shift;
}

static inline void mm2_decode_touch(const uint8_t *t, mm2_touch *out)
{
    out->x           = mm2_sext(t[0] | ((t[1] & 0x0F) << 8), 12);
    out->y           = -mm2_sext((t[1] >> 4) | (t[2] << 4), 12);
    out->touch_major = t[3];
    out->touch_minor = t[4];
    out->size        = t[5] & 0x3F;
    out->id          = ((t[6] << 2) | (t[5] >> 6)) & 0x0F;
    out->orientation = (t[6] >> 2) - 32;
    out->state       = t[7] & MM2_TOUCH_STATE_MASK;
    out->down        = out->state != MM2_TOUCH_STATE_NONE;
}

/* Returns 0 on success, -1 if the buffer is not a multitouch 0x12 report. */
static inline int mm2_decode(const uint8_t *report, size_t len, mm2_report *out)
{
    size_t n, i;
    if (len < MM2_HEADER_BYTES || report[0] != MM2_REPORT_ID)
        return -1;
    if ((len - MM2_HEADER_BYTES) % MM2_TOUCH_BYTES != 0)
        return -1;
    n = (len - MM2_HEADER_BYTES) / MM2_TOUCH_BYTES;
    if (n > MM2_MAX_TOUCHES)
        return -1;

    out->buttons     = report[1];
    out->dx          = (int16_t)(report[2] | (report[3] << 8));
    out->dy          = (int16_t)(report[4] | (report[5] << 8));
    out->touch_count = (int)n;
    for (i = 0; i < n; i++)
        mm2_decode_touch(report + MM2_HEADER_BYTES + i * MM2_TOUCH_BYTES, &out->touches[i]);
    return 0;
}
