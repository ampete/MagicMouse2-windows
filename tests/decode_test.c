/* Cross-check mm2_touch.h against the literal Linux hid-magicmouse.c expressions. */
#include <stdio.h>
#include <stdint.h>
#include "mm2_touch.h"

static int linux_x(const uint8_t *t)      { return (int)((uint32_t)t[1] << 28 | (uint32_t)t[0] << 20) >> 20; }
static int linux_y(const uint8_t *t)      { return -((int)((uint32_t)t[2] << 24 | (uint32_t)t[1] << 16) >> 20); }
static int linux_id(const uint8_t *t)     { return (t[6] << 2 | t[5] >> 6) & 0xf; }
static int linux_orient(const uint8_t *t) { return (t[6] >> 2) - 32; }

int main(void)
{
    unsigned seed = 12345, fails = 0, n;
    for (n = 0; n < 1000000; n++) {
        uint8_t t[8]; mm2_touch d; int i;
        for (i = 0; i < 8; i++) { seed = seed * 1103515245u + 12345u; t[i] = (uint8_t)(seed >> 16); }
        mm2_decode_touch(t, &d);
        if (d.x != linux_x(t) || d.y != linux_y(t) || d.id != linux_id(t) ||
            d.orientation != linux_orient(t) || d.size != (t[5] & 0x3f) ||
            d.touch_major != t[3] || d.touch_minor != t[4] || d.state != (t[7] & 0xf0)) {
            if (fails++ < 5) printf("mismatch: %02X %02X %02X x=%d/%d y=%d/%d\n", t[0], t[1], t[2], d.x, linux_x(t), d.y, linux_y(t));
        }
    }
    {   /* report framing */
        uint8_t r[14 + 16] = { 0x12, 0x01, 0xFE, 0xFF, 0x03, 0x00 };
        mm2_report rep;
        if (mm2_decode(r, sizeof r, &rep) || rep.touch_count != 2 || rep.buttons != 1 || rep.dx != -2 || rep.dy != 3) { puts("framing FAIL"); fails++; }
        if (mm2_decode(r, 8, &rep) == 0 || mm2_decode(r, 15, &rep) == 0) { puts("length check FAIL"); fails++; }
    }
    printf("%s (%u mismatches)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
