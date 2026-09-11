// license:BSD-3-Clause
/*
 * dcs2_p2k.h — the Pinball-2000 DCS-2 sound board harness around the vendored ADSP-2105 core.
 *
 * Boots the original sound flash, maps U109/U110 through the SDRC, drives the host<->DSP mailbox,
 * and pulls the SPORT1 autobuffer into stereo PCM — i.e. everything the DCS board did except the
 * DSP itself, which the core executes. Single instance, single thread (offline renderer).
 *
 * The board mechanics (SDRC banking, SPORT autobuffer, mailbox, flash boot) follow MAME's DCS-2
 * implementation (vendor/mame_ref/dcs.cpp, BSD-3) and the Pinball-2000 register specifics; the
 * reference emulator's equivalent was read as documentation only.
 */
#ifndef DCS_EXTRACT_DCS2_P2K_H
#define DCS_EXTRACT_DCS2_P2K_H

#include <cstdint>
#include <cstddef>

/* Load the original assets and boot the DSP. u109/u110 are 4 MiB each; flash is the 1 MiB 28F800
   sound flash (the DSP program). Returns false if a size is wrong. */
bool dcs2_prepare(const uint8_t *u109, size_t u109_len,
                  const uint8_t *u110, size_t u110_len,
                  const uint8_t *flash, size_t flash_len);

/* Submit one 16-bit DCS command word to the host->DSP mailbox and let the DSP consume it. */
void dcs2_write_cmd(uint16_t command);

/* The DSP->host response latch (0 if none pending); advances the DSP while a response is live. */
uint16_t dcs2_read_response(void);

/* The board flag byte: bit6 = command FIFO empty (ready), bit7 = a response is available. */
uint8_t dcs2_flag_byte(void);

/* Render `frames` stereo S16 samples at `output_rate`, running the DSP as the SPORT clock demands. */
void dcs2_render(int16_t *samples, int frames, int output_rate);

#endif
