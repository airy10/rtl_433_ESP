/** @file
    Generic doorbell implementation for Evology C2-301 devices.

    Copyright (C) 2023 Airy André

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

*/
/**
Doorbell implementation for Evology C2-301 devices.

    modulation  = OOK_PWM,
    rhort       = 204,
    long        = 604,
    reset       = 6200,
    gap         = 608,
    tolerance   = 160,
    sync        = 0,
    bits       >= 25,
    rows       >= 32,
    unique,
    get         = @0:{24}:id,

*/

#include "decoder.h"

static int evology_c2_301_callback(r_device *decoder, bitbuffer_t *bitbuffer)
{
    // 25 bits expected, 32x repetition
    int row = bitbuffer_find_repeated_row(bitbuffer, 32, 25);

    if (row < 0 || bitbuffer->bits_per_row[row] != 25)
        return DECODE_ABORT_LENGTH;

    uint8_t *b = bitbuffer->bb[row];

    // 25 bits
    char id_str[4 * 2 + 1];
    sprintf(id_str, "%02x%02x%02x%02x", b[0], b[1], b[2], b[3] & 0x80);

    /* clang-format off */
    data_t *data = data_make(
            "model",    "",        DATA_STRING, "Evology-C2-301",
            "id",       "ID",      DATA_STRING, id_str,
            NULL);
    /* clang-format on */

    decoder_output_data(decoder, data);
    return 1;
}

static char const *const output_fields[] = {
        "model",
        "id",
        NULL,
};

r_device const evology_c2_301 = {
        .name        = "Evology C2-301 Doorbell",
        .modulation  = OOK_PULSE_PWM,
        .short_width = 204,
        .long_width  = 604,
        .reset_limit = 6200,
        .gap_limit   = 608,
        .sync_width = 0,
        .tolerance   = 160,
        .decode_fn   = &evology_c2_301_callback,
        .fields      = output_fields,
};
