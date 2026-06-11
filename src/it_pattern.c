/*
 * it_pattern.c — unpacked pattern model + pack/unpack. See it_pattern.h.
 */

#include <stdlib.h>
#include <string.h>
#include "it_pattern.h"

void (*Engine_Lock)(void)   = NULL;
void (*Engine_Unlock)(void) = NULL;

static void engine_lock(void)   { if (Engine_Lock)   Engine_Lock();   }
static void engine_unlock(void) { if (Engine_Unlock) Engine_Unlock(); }

/* engine note byte -> grid note */
static uint8_t eng_to_grid_note(uint8_t e)
{
    if (e < 120)
        return (uint8_t)(e + 1);
    if (e == 255) return GNOTE_OFF;
    if (e == 254) return GNOTE_CUT;
    return GNOTE_FADE;          /* 120..253 collapse to fade display */
}

/* grid note -> engine note byte */
static uint8_t grid_to_eng_note(uint8_t g)
{
    if (g >= 1 && g <= 120)
        return (uint8_t)(g - 1);
    return g;                   /* 253/254/255 pass through */
}

int Pattern_Unpack(uint16_t patnum, editcell_t *grid)
{
    pattern_t *p;
    const uint8_t *si, *end;
    uint16_t rows, row;
    /* persistent per-channel mask, as the player keeps in [DI+2] */
    static uint8_t maskcache[64];
    /* per-channel last values, mirroring the player's persisted host
     * channel state: a "repeat" mask bit (0x10/0x20/0x40/0x80) re-uses
     * the last value read for that field on that channel. */
    static uint8_t lastnote[64], lastins[64], lastvol[64];
    static uint8_t lastcmd[64], lastcmdval[64];

    /* empty cell = mask 0 (all fields absent) */
    memset(grid, 0, sizeof(editcell_t) * 64 * 64);

    if (patnum >= MAX_PATTERNS)
        return 64;
    p = &Song.Patterns[patnum];
    if (!p->PackedData || p->Rows == 0)
        return 64;

    rows = p->Rows;
    if (rows > MAX_PATROWS)
        rows = MAX_PATROWS;

    memset(grid, 0, sizeof(editcell_t) * rows * 64);

    memset(maskcache, 0, sizeof(maskcache));
    memset(lastnote, 0, sizeof(lastnote));
    memset(lastins, 0, sizeof(lastins));
    memset(lastvol, 0, sizeof(lastvol));
    memset(lastcmd, 0, sizeof(lastcmd));
    memset(lastcmdval, 0, sizeof(lastcmdval));
    si = p->PackedData;
    end = si + p->DataLength;

    for (row = 0; row < rows; row++) {
        for (;;) {
            uint8_t cv, mask, ch;
            editcell_t *c;

            if (si >= end)
                return rows;
            cv = *si++;
            if (cv == 0)
                break;          /* end of row */

            ch = (uint8_t)((cv & 0x7F) - 1) & 63;
            if (cv & 0x80) {
                if (si >= end) return rows;
                maskcache[ch] = *si++;
            }
            mask = maskcache[ch];
            c = &grid[row * 64 + ch];

            /* A field is present if its read bit OR its repeat bit is
             * set. Read bits also update the channel's "last" value.
             * This is exactly what the player resolves into hc->Nte etc. */
            if (mask & 1) {
                if (si < end) lastnote[ch] = eng_to_grid_note(*si++);
                c->note = lastnote[ch]; c->mask |= CM_NOTE;
            } else if (mask & 0x10) {
                c->note = lastnote[ch]; c->mask |= CM_NOTE;
            }

            if (mask & 2) {
                if (si < end) lastins[ch] = *si++;
                c->ins = lastins[ch]; c->mask |= CM_INS;
            } else if (mask & 0x20) {
                c->ins = lastins[ch]; c->mask |= CM_INS;
            }

            if (mask & 4) {
                if (si < end) lastvol[ch] = *si++;
                c->vol = lastvol[ch]; c->mask |= CM_VOL;
            } else if (mask & 0x40) {
                c->vol = lastvol[ch]; c->mask |= CM_VOL;
            }

            if (mask & 8) {
                if (si + 1 < end) {
                    lastcmd[ch] = si[0]; lastcmdval[ch] = si[1]; si += 2;
                } else si = end;
                c->cmd = lastcmd[ch]; c->cmdval = lastcmdval[ch];
                c->mask |= CM_CMD;
            } else if (mask & 0x80) {
                c->cmd = lastcmd[ch]; c->cmdval = lastcmdval[ch];
                c->mask |= CM_CMD;
            }
        }
    }
    return rows;
}

int Pattern_Pack(uint16_t patnum, const editcell_t *grid, uint16_t rows)
{
    uint8_t *buf;
    size_t cap, len = 0;
    uint16_t row;
    pattern_t *p;

    if (patnum >= MAX_PATTERNS || rows == 0 || rows > MAX_PATROWS)
        return 0;

    /* worst case: every cell present -> 2 (cv+mask) + 1 + 1 + 1 + 2 = 7
     * bytes per channel, + 1 row terminator. */
    cap = (size_t)rows * (64 * 7 + 1) + 16;
    buf = (uint8_t *)malloc(cap);
    if (!buf)
        return 0;

    for (row = 0; row < rows; row++) {
        int ch;
        for (ch = 0; ch < 64; ch++) {
            const editcell_t *c = &grid[row * 64 + ch];
            uint8_t mask = c->mask & 0x0F;  /* read bits only */

            if (!mask)
                continue;

            /* Always (re)send the mask byte. Equivalent for the player
             * to the original's persisted-mask/repeat-bit encoding, just
             * without the size optimisation. */
            buf[len++] = (uint8_t)((ch + 1) | 0x80);
            buf[len++] = mask;
            if (mask & CM_NOTE) buf[len++] = grid_to_eng_note(c->note);
            if (mask & CM_INS)  buf[len++] = c->ins;
            if (mask & CM_VOL)  buf[len++] = c->vol;
            if (mask & CM_CMD) { buf[len++] = c->cmd; buf[len++] = c->cmdval; }
        }
        buf[len++] = 0;         /* end of row */
    }

    p = &Song.Patterns[patnum];

    engine_lock();
    {
        uint8_t *old = p->PackedData;
        uint8_t *nb = (uint8_t *)realloc(buf, len + 1);
        if (nb) buf = nb;
        buf[len] = 0;
        p->PackedData = buf;
        p->DataLength = (uint16_t)len;
        p->Rows = rows;
        free(old);
    }
    engine_unlock();
    return 1;
}

uint16_t Pattern_EnsureExists(uint16_t patnum, uint16_t rows)
{
    pattern_t *p;

    if (patnum >= MAX_PATTERNS)
        return 64;
    p = &Song.Patterns[patnum];
    if (p->PackedData && p->Rows)
        return p->Rows;

    if (rows == 0 || rows > MAX_PATROWS)
        rows = 64;

    /* an empty pattern packs to `rows` row-terminator bytes */
    {
        uint8_t *buf = (uint8_t *)calloc(rows + 1, 1);
        if (!buf)
            return 64;
        engine_lock();
        p->PackedData = buf;
        p->DataLength = rows;
        p->Rows = rows;
        engine_unlock();
    }
    return rows;
}
