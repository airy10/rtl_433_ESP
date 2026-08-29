/** @file
    Generic doorbell implementation for Evology C2-301 devices.

    Copyright (C) 2023 Airy André
    Copyright (C) 2026 Airy André

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

*/
/**
Doorbell implementation for Evology C2-301 devices.

    modulation  = OOK_PWM,
    short       = 204,
    long        = 604,
    reset       = 15000,
    gap         = 1000,
    tolerance   = 160,
    sync        = 0,
    bits       >= 25,
    rows       >= 32,
    unique,
    get         = @0:{24}:id,

*/

#include "decoder.h"

#define EVOLOGY_C2_301_BITLEN      25  // 24-bit id + 1 constant trailing bit
#define EVOLOGY_C2_301_MIN_REPEATS 8   // of the ~32 packets sent per press must agree

static int evology_c2_301_callback(r_device *decoder, bitbuffer_t *bitbuffer)
{
    // The doorbell repeats each packet ~32 times per press. Requiring all 32
    // identical rows in one bitbuffer is impossible in practice: the row
    // buffer is capped (50 rows on this build, 25 with
    // RTL_433_REDUCE_STACK_USE) and corrupted packets demodulate to 24-bit
    // rows which do not count. Find any row that repeats enough times
    // (EVOLOGY_C2_301_MIN_REPEATS of the ~32 sent) and emit once per press.
    int row = bitbuffer_find_repeated_row(bitbuffer, EVOLOGY_C2_301_MIN_REPEATS,
                                          EVOLOGY_C2_301_BITLEN);

    if (row < 0 || bitbuffer->bits_per_row[row] != EVOLOGY_C2_301_BITLEN)
        return DECODE_ABORT_LENGTH;

    uint8_t *b = bitbuffer->bb[row];

    // 25-bit row: 24-bit id (bits 0..23, MSB first, per the flex conf
    // "get=@0:{24}:id") followed by a constant trailing bit (b[3] & 0x80).
    // Do NOT include the trailing bit in the id (an earlier version appended
    // it as a fake byte, e.g. "6e73a780" instead of "6e73a7").
    char id_str[6 + 1];
    sprintf(id_str, "%02x%02x%02x", b[0], b[1], b[2]);

    /* clang-format off */
    data_t *data = data_make(
            "model",    "",        DATA_STRING, "Evology-C2-301",
            "id",       "ID",      DATA_STRING, id_str,
            "button",   "Button",  DATA_STRING, id_str,
            NULL);
    /* clang-format on */

    decoder_output_data(decoder, data);
    return 1;
}

static char const *const output_fields[] = {
        "model",
        "id",
        "button",
        NULL,
};

/*
 * Timings verified against real captures (rtl_433_ESP OOK pipeline):
 * short pulse ~204 us (bit 1), long pulse ~604 us (bit 0), inter-bit gap
 * ~200/600 us, inter-packet gap ~6200 us, inter-press gap ~75 ms.
 *
 * reset_limit is deliberately LONGER than the inter-packet gap (6200 us) so
 * the ~32 repeats of one press land in a single bitbuffer (one row per
 * packet) and the decoder can vote on them. The old value of 6200 matched
 * the inter-packet gap exactly: any measured gap over 6200 ended the message
 * mid-press, so the decoder never saw enough repeats and never decoded.
 * 15000 us still splits consecutive presses (>= 75 ms apart).
 *
 * gap_limit (1000 us) sits above the long inter-bit gap (~600 us, measured
 * up to ~700 us with demod skew) and below the inter-packet gap. The old
 * value of 608 split rows mid-packet on real signals.
 */
r_device const evology_c2_301 = {
        .name        = "Evology C2-301 Doorbell",
        .modulation  = OOK_PULSE_PWM,
        .short_width = 204,
        .long_width  = 604,
        .reset_limit = 15000,
        .gap_limit   = 1000,
        .sync_width  = 0,
        .tolerance   = 160,
        .decode_fn   = &evology_c2_301_callback,
        .fields      = output_fields,
};
