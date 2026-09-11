/* addrspace.c — the BAR5 image-ROM windows + optional BAR3 flash.
 * 1:1 with walk_anims.py:AddrSpace. */
#include "marsminer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mm_aspace_init(mm_aspace *as, const char *banks_dir, mm_buf flash) {
    memset(as, 0, sizeof *as);
    as->image_len = 4u * MM_BANK_SZ; /* 64 MiB */
    as->image = (uint8_t *)malloc(as->image_len);
    if (!as->image)
        return -1;
    memset(as->image, 0xFF, as->image_len); /* absent banks read as 0xFF */

    for (int i = 0; i < 4; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/rfm_bank%d.bin", banks_dir, i);
        mm_buf b = mm_load(p);
        if (b.data) {
            size_t n = b.len < MM_BANK_SZ ? b.len : MM_BANK_SZ; /* trim to 16 MiB */
            memcpy(as->image + (size_t)i * MM_BANK_SZ, b.data, n);
            as->have[i] = 1;
            mm_buf_free(&b);
        } else {
            as->have[i] = 0;
        }
    }
    as->flash = flash; /* borrowed, not owned */
    as->flash_top = 0x12;
    return 0;
}

void mm_aspace_free(mm_aspace *as) {
    if (as->image) {
        free(as->image);
        as->image = NULL;
    }
    as->image_len = 0;
    /* as->flash is borrowed from mm_ctx; do not free here */
}

const uint8_t *mm_region(const mm_aspace *as, uint32_t ptr, size_t *off, size_t *avail) {
    if (ptr >= MM_IMG_BASE && ptr < MM_IMG_END) {
        size_t o = ptr - MM_IMG_BASE;
        if (off)
            *off = o;
        if (avail)
            *avail = o < as->image_len ? as->image_len - o : 0;
        return as->image;
    }
    if (as->flash.data && (ptr >> 24) == as->flash_top) {
        size_t o = ptr & 0xFFFFFF;
        if (off)
            *off = o;
        if (avail)
            *avail = o < as->flash.len ? as->flash.len - o : 0;
        return as->flash.data;
    }
    return NULL;
}

uint32_t mm_as_u32(const mm_aspace *as, uint32_t ptr, uint32_t def) {
    size_t off, avail;
    const uint8_t *b = mm_region(as, ptr, &off, &avail);
    if (!b || avail < 4)
        return def;
    const uint8_t *p = b + off;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
