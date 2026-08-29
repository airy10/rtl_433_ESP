/** @file
    Decode CAME remote control TOP-432EV, TOP-432NA, TOP-432EE.

    This remote control is used for garage doors and sliding gates. It
    transmits on 433.92 MHz (as written on the case), built since 2006.

    It works with CAME radio receiver cards "AF43S", capable of handling
    4096 codes. CAME is an Italian company. These remote controls are mainly
    sold in Europe (France, Italy, Belgium). https://www.came.com

    Protocol analysis and reference rtl_433 flex conf by J. Forestier (2020),
    see rtl_433 conf/CAME-TOP432.conf and
    https://github.com/psa-jforestier/rtl_433_tests/tree/master/tests/Came/TOP432

    Copyright (C) 2020 J. Forestier
    Copyright (C) 2026 Airy André

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/**
CAME TOP-432 remote control.

The device uses PWM encoding:
- 0 is encoded as a 640 us pulse and a 320 us gap,
- 1 is encoded as a 320 us pulse and a 640 us gap.

A transmission starts with a 320 us start pulse, then the 12 data bits are
sent MSB first. Each packet is repeated 4 times, separated by a minimum of
36 periods of 320 us (11520 us). There is no CRC, no parity and no preamble:
the leading start pulse is the only framing.

Since the pulse width carries the bit value, the demodulated row is the start
pulse (a short pulse, read as a '1' bit) followed by the 12-bit code, i.e.
13 bits in total. Only rows of exactly 13 bits starting with the start bit
are accepted.

Because there is no MIC, the only redundancy is the 4x repetition of every
packet. A single 13-bit row is far too weak a signature to accept on its own
(12 unknown bits, any noise burst or bit slip can synthesize one), so this
decoder requires the repeats to agree: reset_limit is kept above the
inter-packet gap so one button press arrives as one bitbuffer holding the
repeats as rows, and a code is emitted only when at least
CAME_TOP432_MIN_REPEATS rows carry the same value. The result is one message
per press, not one per repeat.

Diagram :
          |        <---> logical 0          <---> logical 1                     <-- 11520us -->
          |       _   __    _   __   __   __    _    _    _   __   __    _    _                 _
Pulse/gap | _____| |_|  |__| |_|  |_|  |_|  |__| |__| |__| |_|  |_|  |__| |__| |_____(..)______| |
          +------------------------------------------------------------------------------
bits              ~~ : start bit (320 us)
                    |  0| 1  |  0 |  0 |  0 |  1 |  1 |  1 |  0 |  0 |  1 |  1 |
                    |        0x4       |       0x7         |       0x3         |

Data layout (13 bits):
    S cccccccccccc
- S : start bit (short 320 us pulse), always 1
- c : 12-bit code, identifies the remote/system and the pressed button
*/

#include "decoder.h"

#define CAME_TOP432_BITLEN      13 // start bit + 12 data bits
#define CAME_TOP432_MIN_REPEATS 3  // of the 4 transmitted packets must agree

static int came_top432_decode(r_device *decoder, bitbuffer_t *bitbuffer)
{
    data_t *data;
    int events = 0;

    // Collect the valid rows (13 bits, start bit set) and their codes.
    // A bitbuffer never holds more than a handful of rows for this protocol
    // (the 4 repeats of one press), so the O(n^2) vote below is trivial.
    int codes[BITBUF_ROWS];
    int num_codes = 0;

    for (int r = 0; r < bitbuffer->num_rows; ++r) {
        if (bitbuffer->bits_per_row[r] != CAME_TOP432_BITLEN) {
            continue; // not a CAME packet (no preamble, length is the only framing)
        }

        uint8_t *b = bitbuffer->bb[r];

        // The first (short) pulse of every packet demodulates to the start bit
        if (!(b[0] & 0x80)) {
            decoder_logf(decoder, 2, __func__, "Row %d: missing start bit", r);
            continue;
        }

        // 12-bit code, MSB first: bits 1..12 of the row
        int code = ((b[0] & 0x7f) << 5) | (b[1] >> 3);

        // No MIC available, at least reject the degenerate all-zero/all-ones codes
        if (code == 0x000 || code == 0xfff) {
            decoder_logf(decoder, 2, __func__, "Row %d: degenerate code 0x%03x", r, code);
            continue;
        }

        if (num_codes < BITBUF_ROWS) {
            codes[num_codes++] = code;
        }
    }

    // Require the packet repeats to agree before trusting a code: emit each
    // code at most once per transmission, and only when at least
    // CAME_TOP432_MIN_REPEATS of the (up to 4) packets carry it. A lone
    // valid-looking row from noise or another device is discarded here.
    for (int i = 0; i < num_codes; ++i) {
        int votes = 0;
        for (int j = 0; j < num_codes; ++j) {
            if (codes[j] == codes[i]) {
                votes++;
            }
        }
        if (votes < CAME_TOP432_MIN_REPEATS) {
            decoder_logf(decoder, 2, __func__, "Code 0x%03x seen %d time(s), need %d agreeing repeats",
                    codes[i], votes, CAME_TOP432_MIN_REPEATS);
            continue;
        }

        // Emit once per code per transmission, not once per repeat
        int emitted = 0;
        for (int j = 0; j < i; ++j) {
            if (codes[j] == codes[i]) {
                emitted = 1;
                break;
            }
        }
        if (emitted) {
            continue;
        }

        // The 12-bit code is opaque: it is the ONLY identity this protocol
        // has. Two independent remotes prove there is no fixed id/button
        // bit split:
        //
        //   J. Forestier's captures (Button1 0x473 / Button2 0x873):
        //     buttons differ in the TOP 2 bits, share the low 10
        //   Live field remote (button 1 0x5CE / button 2 0x5CD):
        //     buttons differ in the LOW 2 bits, share the top 10
        //
        // (Forestier's flex conf used OOK_PPM, which reads bits from the
        // gaps: PPM and PWM rows are exact bit COMPLEMENTS of each other --
        // his PPM readings 0xb8c/0x78c invert to 0x473/0x873, matching this
        // PWM decoder. A complementary read shifts WHICH bits look "common"
        // vs "differing", which is why the two observations disagree.)
        //
        // So publish the full code as the device identity ("id") and let
        // each code be its own Home Assistant device: rtl_433_mqtt_hass.py
        // builds device_id from the "id" field and creates a
        // device_automation button trigger per device from the "button"
        // field, so every code gets its own trigger and automations can
        // tell the buttons apart. "button" mirrors the code (its value is
        // not read by the trigger, but a constant would be ambiguous for
        // value-matching consumers). "code" repeats the value in hex for
        // logs and OpenMQTTGateway-style JSON consumers.
        int const code = codes[i];

        /* clang-format off */
        data = data_make(
                "model",    "Model",       DATA_STRING, "CAME-TOP432",
                "id",       "Code",            DATA_INT,    code,
                "button",   "Button",     DATA_INT,    code,
                "code",     "Code (hex)", DATA_FORMAT, "0x%03x", DATA_INT, code,
                NULL);
        /* clang-format on */

        decoder_output_data(decoder, data);
        events++;
    }

    return events ? events : DECODE_ABORT_LENGTH;
}

static char const *const output_fields[] = {
        "model",
        "id",
        "button",
        "code",
        NULL,
};

/*
 * Timings are per the device documentation / rtl_433_tests captures:
 * 320 us start pulse, 320/640 us PWM pulses, at least 11520 us between the
 * 4 packet repeats (real captures show ~14.9 ms).
 *
 * reset_limit (30000 us) is deliberately LONGER than the inter-packet gap so
 * the 4 repeats of one press are accumulated into a single bitbuffer (one row
 * per repeat) and the decoder can require them to agree. The decoder is
 * stateless -- rtl_433 clears the bitbuffer after every decode call -- so a
 * generous reset_limit only affects grouping inside a train, never across
 * trains. gap_limit (830 us) still splits rows should repeats arrive inside
 * one buffer with sub-reset gaps.
 */
r_device came_top432 = {
        .name        = "CAME TOP432 remote control",
        .modulation  = OOK_PULSE_PWM,
        .short_width = 320,   // start pulse and '1' pulse
        .long_width  = 640,   // '0' pulse
        .gap_limit   = 830,   // larger than 640 us pulse + 320 us gap
        .reset_limit = 30000, // above the ~11.5-14.9 ms inter-packet gap: keep the 4 repeats together
        .tolerance   = 160,   // +/- 50% of the short pulse
        .decode_fn   = &came_top432_decode,
        .disabled    = 0,
        .fields      = output_fields,
};
