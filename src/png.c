/* png.c — PNG output via the vendored stb_image_write (public domain).
 * The engine's asset loader only needs valid RGBA/RGB PNGs; stb's default
 * filtered zlib output is compatible with any PNG reader (incl. Pillow). */
#include "marsminer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int mm_write_png(const char *path, int w, int h, int comp, const uint8_t *pixels) {
    /* stride = w*comp (tightly packed). Returns non-zero on success in stb. */
    int rc = stbi_write_png(path, w, h, comp, pixels, w * comp);
    return rc ? 0 : -1;
}
