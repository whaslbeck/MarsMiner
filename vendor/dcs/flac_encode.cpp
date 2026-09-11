// license:BSD-3-Clause
/*
 * flac_encode.cpp — encode interleaved S16 PCM to lossless FLAC via libFLAC's stream encoder.
 */
#include "flac_encode.h"

#include <FLAC/stream_encoder.h>
#include <vector>

bool flac_encode_s16(const char *path, const int16_t *interleaved,
                     size_t frames, int channels, int rate, int level)
{
    if (!path || !interleaved || channels < 1 || channels > 2 || rate <= 0)
        return false;

    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) return false;

    FLAC__stream_encoder_set_channels(enc, channels);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, (unsigned)rate);
    FLAC__stream_encoder_set_compression_level(enc, (unsigned)level);
    FLAC__stream_encoder_set_total_samples_estimate(enc, frames);

    if (FLAC__stream_encoder_init_file(enc, path, nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(enc);
        return false;
    }

    /* libFLAC takes one FLAC__int32 per sample (interleaved). Feed in chunks. */
    const size_t CHUNK = 4096;
    std::vector<FLAC__int32> buf(CHUNK * channels);
    bool ok = true;
    size_t pos = 0;
    while (pos < frames && ok) {
        size_t n = (frames - pos) < CHUNK ? (frames - pos) : CHUNK;
        for (size_t i = 0; i < n * (size_t)channels; i++)
            buf[i] = interleaved[pos * channels + i];
        ok = FLAC__stream_encoder_process_interleaved(enc, buf.data(), (unsigned)n);
        pos += n;
    }

    ok = FLAC__stream_encoder_finish(enc) && ok;
    FLAC__stream_encoder_delete(enc);
    return ok;
}
