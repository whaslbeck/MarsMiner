// license:BSD-3-Clause
/*
 * flac_encode.h — minimal libFLAC wrapper: encode interleaved S16 PCM to a lossless .flac file.
 * Lossless keeps the extracted audio bit-identical to the DCS decoder output.
 */
#ifndef RFM_FLAC_ENCODE_H
#define RFM_FLAC_ENCODE_H

#include <cstdint>
#include <cstddef>

/* Encode `frames` interleaved S16 samples (`channels` per frame) at `rate` Hz into a FLAC file at
   `path`, at the given compression level (0..8). Returns true on success. */
bool flac_encode_s16(const char *path, const int16_t *interleaved,
                     size_t frames, int channels, int rate, int level);

#endif
