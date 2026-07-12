/*
 * it_load.c
 * ---------
 * .IT module loader for the engine port. The original loader lives in
 * IT_DISK.ASM; this is a re-implementation against ITTECH.TXT that fills
 * the same in-memory structures the engine expects:
 *  - song header, orders, instruments and sample headers in their
 *    original byte layouts,
 *  - patterns kept in the packed on-disk format (the player decodes the
 *    packed stream row by row, as in the original),
 *  - sample data decompressed (IT2.14/IT2.15 compression) into flat
 *    memory, padded by 4 frames for the cubic interpolator.
 *  - the MIDI macro data area, from the embedded configuration when
 *    present, otherwise the stock ITMIDI.CFG defaults.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "it_music.h"
#include "it_save.h"                    /* song message buffer */

/* ---------------------------------------------------------------- */

typedef struct reader_t {
    const uint8_t *data;
    size_t size;
    size_t pos;
} reader_t;

static int rd_seek(reader_t *r, size_t pos)
{
    if (pos > r->size)
        return 0;
    r->pos = pos;
    return 1;
}

static int rd_read(reader_t *r, void *dst, size_t n)
{
    if (r->pos + n > r->size)
        return 0;
    memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    return 1;
}

static uint16_t rd_u16(reader_t *r)
{
    uint16_t v = 0;
    rd_read(r, &v, 2);
    return v;
}

static uint32_t rd_u32(reader_t *r)
{
    uint32_t v = 0;
    rd_read(r, &v, 4);
    return v;
}

/* ---------------------------------------------------------------- *
 * Default MIDI macro configuration (contents of ITMIDI.CFG).
 * Exported for the module importers (it_import.c).
 * ---------------------------------------------------------------- */
void SetDefaultMIDIDataArea(void)
{
    int i;

    memset(MIDIDataArea, 0, MIDIDATAAREA_SIZE);

    strcpy(&MIDIDataArea[0x000], "FF");          /* MIDI Start        */
    strcpy(&MIDIDataArea[0x020], "FC");          /* MIDI Stop         */
    /* 0x040: tick (empty) */
    strcpy(&MIDIDataArea[0x060], "9c n v");      /* play note         */
    strcpy(&MIDIDataArea[0x080], "9c n 0");      /* stop note         */
    /* 0x0A0: change volume, 0x0C0: change pan (empty) */
    strcpy(&MIDIDataArea[0x0E0], "Bc 0 a 20 b"); /* bank select       */
    strcpy(&MIDIDataArea[0x100], "Cc p");        /* program select    */

    strcpy(&MIDIDataArea[0x120], "F0F000z");     /* SF0: filter cutoff */

    for (i = 0; i < 16; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "F0F001%02X", i * 8);
        strcpy(&MIDIDataArea[0x320 + i * 32], buf); /* Z80..Z8F: reso  */
    }
}

/* ---------------------------------------------------------------- *
 * IT 2.14 / 2.15 sample decompression
 * ---------------------------------------------------------------- */
typedef struct bitreader_t {
    reader_t *r;
    uint32_t bitbuf;
    int bitnum;
} bitreader_t;

static uint32_t ReadBits(bitreader_t *b, int n)
{
    uint32_t value = 0;
    int i = n;

    while (i-- > 0) {
        if (b->bitnum == 0) {
            uint8_t byte = 0;
            rd_read(b->r, &byte, 1);
            b->bitbuf = byte;
            b->bitnum = 8;
        }
        value >>= 1;
        value |= (b->bitbuf & 1) << 31;
        b->bitbuf >>= 1;
        b->bitnum--;
    }
    return value >> (32 - n);
}

static int DecompressIT8(reader_t *r, int8_t *dst, uint32_t length, int it215)
{
    while (length > 0) {
        uint32_t blklen = (length < 0x8000) ? length : 0x8000;
        uint32_t blkpos = 0;
        uint16_t cbytes;
        bitreader_t br;
        int width = 9;
        int8_t d1 = 0, d2 = 0;
        size_t blockend;

        cbytes = rd_u16(r);
        blockend = r->pos + cbytes;

        br.r = r;
        br.bitbuf = 0;
        br.bitnum = 0;

        while (blkpos < blklen && r->pos <= blockend) {
            uint32_t value = ReadBits(&br, width);

            if (width < 7) {
                if (value == (uint32_t)(1 << (width - 1))) {
                    uint32_t nw = ReadBits(&br, 3) + 1;
                    width = ((int)nw < width) ? (int)nw : (int)nw + 1;
                    continue;
                }
            } else if (width < 9) {
                uint32_t border = (0xFFu >> (9 - width)) - 4;
                if (value > border && value <= border + 8) {
                    uint32_t nw = value - border;
                    width = ((int)nw < width) ? (int)nw : (int)nw + 1;
                    continue;
                }
            } else {
                if (value & 0x100) {
                    width = (int)(value + 1) & 0xFF;
                    continue;
                }
            }

            /* sign extend */
            {
                int8_t v;
                if (width < 8) {
                    int shift = 8 - width;
                    v = (int8_t)(value << shift);
                    v >>= shift;
                } else {
                    v = (int8_t)value;
                }
                d1 = (int8_t)(d1 + v);
                d2 = (int8_t)(d2 + d1);
                *dst++ = it215 ? d2 : d1;
                blkpos++;
            }
        }
        if (!rd_seek(r, blockend))
            return 0;
        length -= blklen;
    }
    return 1;
}

static int DecompressIT16(reader_t *r, int16_t *dst, uint32_t length,
                          int it215)
{
    while (length > 0) {
        uint32_t blklen = (length < 0x4000) ? length : 0x4000;
        uint32_t blkpos = 0;
        uint16_t cbytes;
        bitreader_t br;
        int width = 17;
        int16_t d1 = 0, d2 = 0;
        size_t blockend;

        cbytes = rd_u16(r);
        blockend = r->pos + cbytes;

        br.r = r;
        br.bitbuf = 0;
        br.bitnum = 0;

        while (blkpos < blklen && r->pos <= blockend) {
            uint32_t value = ReadBits(&br, width);

            if (width < 7) {
                if (value == (uint32_t)(1 << (width - 1))) {
                    uint32_t nw = ReadBits(&br, 4) + 1;
                    width = ((int)nw < width) ? (int)nw : (int)nw + 1;
                    continue;
                }
            } else if (width < 17) {
                uint32_t border = (0xFFFFu >> (17 - width)) - 8;
                if (value > border && value <= border + 16) {
                    uint32_t nw = value - border;
                    width = ((int)nw < width) ? (int)nw : (int)nw + 1;
                    continue;
                }
            } else {
                if (value & 0x10000) {
                    width = (int)(value + 1) & 0xFF;
                    continue;
                }
            }

            {
                int16_t v;
                if (width < 16) {
                    int shift = 16 - width;
                    v = (int16_t)(value << shift);
                    v >>= shift;
                } else {
                    v = (int16_t)value;
                }
                d1 = (int16_t)(d1 + v);
                d2 = (int16_t)(d2 + d1);
                *dst++ = it215 ? d2 : d1;
                blkpos++;
            }
        }
        if (!rd_seek(r, blockend))
            return 0;
        length -= blklen;
    }
    return 1;
}

/* ---------------------------------------------------------------- *
 * D_LoadSampleData (IT_DISK.ASM 2867..): read + convert one sample's
 * data from a source file image, driven by the header's Flags/Cvt.
 * Flag word (BP in the ASM): 1 = 16-bit, 2 = already signed,
 * 4 = swap bytes (big endian), 8 = delta values, 16 = byte delta
 * (PTM), 64 = stereo (standalone WAV: read both channels, keep one),
 * 8000h = IT-compressed. (32 = TX 12-bit is TXWave-only, not ported.)
 * Stereo (feature 013): the original prompts Left/Right
 * (O1_StereoSampleList; right = BP bit 128). The editor installs
 * Load_StereoChoice to run that requester; a NULL hook (headless
 * paths, DisableStereoMenu parity) takes the left channel.
 * Exported for the sample/instrument library (it_ris.c).
 * ---------------------------------------------------------------- */
int (*Load_StereoChoice)(void);         /* returns 64 left / 192 right */

int Load_SampleData(const uint8_t *filedata, size_t size, sample_t *s)
{
    reader_t rr = { filedata, size, 0 };
    reader_t *r = &rr;
    int is16, compressed, it215, bswap, delta, bytedelta, stereo, tx12;
    uint32_t len = s->Length;
    uint32_t units;
    size_t bytes;
    uint8_t *data;

    s->Data = NULL;
    if (len == 0 || !(s->Flags & 1))
        return 1;                       /* D_NoLoadSample */
    if (len > 0x0FFFFFFF)
        return 0;

    is16       = (s->Flags & 2) != 0;
    compressed = (s->Flags & 8) != 0;
    it215      = compressed && (s->Cvt & 4) != 0;
    bswap      = is16 && (s->Cvt & 2) != 0;
    /* the DOS decompressor is IT214-only and the shared delta pass
     * supplies the IT215 second integration (Cvt bit 2); ours applies
     * it internally, so the pass runs for uncompressed data only */
    delta      = !compressed && (s->Cvt & 0x0C) != 0;
    bytedelta  = !compressed && (s->Cvt & 8) != 0;
    stereo     = !compressed && (s->Cvt & 32) != 0;
    tx12       = !compressed && (s->Cvt & 0x10) != 0;   /* TX Wave */

    /* stereo reads both channels (the ASM doubles EDX again) and
     * compacts to one after conversion */
    units = len << (stereo ? 1 : 0);
    bytes = (size_t)units << (is16 ? 1 : 0);
    data = (uint8_t *)calloc((size_t)units + 4, is16 ? 2 : 1);
    if (!data)
        return 0;
    s->Data = data;

    if (!rd_seek(r, s->OffsetInFile)) {
        /* missing data: keep silence */
    } else {
        if (compressed) {
            if (is16)
                DecompressIT16(r, (int16_t *)data, len, it215);
            else
                DecompressIT8(r, (int8_t *)data, len, it215);
        } else {
            size_t avail = r->size - r->pos;
            size_t rd = bytes;
            if (tx12)                   /* 12-bit: 3 bytes / 2 samples */
                rd = (bytes * 3 + 3) >> 2;
            if (rd > avail)
                rd = avail;
            memcpy(data, r->data + r->pos, rd);
            if (!tx12) {
                bytes = rd;
            } else {
                /* ConvertTXSample1 (IT_DISK.ASM, feature 013): expand
                 * each 3-byte group into two LE 16-bit samples,
                 * backwards in place */
                size_t g = (rd + 2) / 3;
                while (g--) {
                    uint8_t b0 = data[g * 3], b1 = data[g * 3 + 1],
                            b2 = data[g * 3 + 2];
                    data[g * 4]     = (uint8_t)(b1 & 0xF0);
                    data[g * 4 + 1] = b0;
                    data[g * 4 + 2] = (uint8_t)((b1 << 4) & 0xF0);
                    data[g * 4 + 3] = b2;
                }
            }
        }

        /* order as in the ASM: byte-swap, then delta, then sign --
         * each pass covers the full (interleaved, if stereo) block */
        if (bswap) {
            uint32_t n;
            for (n = 0; n < units; n++) {
                uint8_t t = data[n * 2];
                data[n * 2] = data[n * 2 + 1];
                data[n * 2 + 1] = t;
            }
        }
        if (delta) {
            if (is16 && !bytedelta) {
                int16_t *p = (int16_t *)data, acc = 0;
                uint32_t n;
                for (n = 0; n < units; n++) {
                    acc = (int16_t)(acc + p[n]);
                    p[n] = acc;
                }
            } else {
                uint32_t n, cnt = is16 ? units * 2 : units;
                uint8_t acc = 0;
                for (n = 0; n < cnt; n++) {
                    acc = (uint8_t)(acc + data[n]);
                    data[n] = acc;
                }
            }
        }
        if (!(s->Cvt & 1)) {            /* unsigned -> signed */
            if (is16) {
                int16_t *p = (int16_t *)data;
                uint32_t n;
                for (n = 0; n < units; n++)
                    p[n] = (int16_t)((uint16_t)p[n] ^ 0x8000);
            } else {
                uint32_t n;
                for (n = 0; n < units; n++)
                    data[n] = (uint8_t)(data[n] ^ 0x80);
            }
        }
        if (stereo) {
            /* channel compaction (IT_DISK.ASM 3172..3211): keep every
             * other sample; left starts at 0, right at +1 per the
             * O1_StereoSampleList choice (BP bit 128) */
            uint32_t n, ch = 0;
            if (Load_StereoChoice && Load_StereoChoice() & 128)
                ch = 1;
            if (is16) {
                int16_t *p = (int16_t *)data;
                for (n = 0; n < len; n++)
                    p[n] = p[n * 2 + ch];
            } else {
                for (n = 0; n < len; n++)
                    data[n] = data[n * 2 + ch];
            }
        }
    }

    /* header rewrite, as the original does up front */
    s->Flags &= (uint8_t)~0x0C;
    s->Cvt = 1;

    /* pad for the cubic interpolator (reads pos+1, pos+2) */
    if (is16) {
        int16_t *p = (int16_t *)data;
        p[len] = p[len + 1] = (len > 0) ? p[len - 1] : 0;
        p[len + 2] = p[len + 3] = 0;
    } else {
        int8_t *p = (int8_t *)data;
        p[len] = p[len + 1] = (len > 0) ? p[len - 1] : 0;
        p[len + 2] = p[len + 3] = 0;
    }

    /* clamp loop points defensively (malformed files) */
    if (s->LoopEnd > s->Length)     s->LoopEnd = s->Length;
    if (s->SusLoopEnd > s->Length)  s->SusLoopEnd = s->Length;
    if (s->LoopBeg > s->Length)     s->LoopBeg = 0;
    if (s->SusLoopBeg > s->Length)  s->SusLoopBeg = 0;
    s->ViT &= 3;
    return 1;
}

/* ---------------------------------------------------------------- *
 * Old-format (cmwt < 2.00) instrument conversion
 * ---------------------------------------------------------------- */
static void LoadOldInstrument(const uint8_t *src, instrument_t *in)
{
    int i, n;

    memset(in, 0, sizeof(*in));
    memcpy(&in->ID, src, 4);
    memcpy(in->DOSFileName, src + 4, 12);

    {
        uint8_t flags = src[0x11];
        uint8_t vls = src[0x12], vle = src[0x13];
        uint8_t sls = src[0x14], sle = src[0x15];
        uint16_t fadeout;

        memcpy(&fadeout, src + 0x18, 2);

        in->NNA = src[0x1A];
        /* DNC: 0 = off, 1 = note, 2 = sample */
        if (src[0x1B] == 1) {
            in->DCT = 1;        /* note */
            in->DCA = 0;        /* cut  */
        } else if (src[0x1B] == 2) {
            in->DCT = 2;        /* sample */
            in->DCA = 0;
        }
        in->FadeOut = (uint16_t)(fadeout << 1);  /* 0..64 -> 0..128 *512 */
        in->GbV = 128;
        in->DfP = 32 | 0x80;    /* don't use */
        memcpy(in->InstrumentName, src + 0x20, 26);
        memcpy(in->NoteSampleTable, src + 0x40, 240);

        /* volume envelope node points at +1F8h: 25 (tick, magnitude)
         * pairs, FFh-terminated */
        n = 0;
        for (i = 0; i < 25; i++) {
            uint8_t tick = src[0x1F8 + i * 2];
            uint8_t mag = src[0x1F8 + i * 2 + 1];
            if (tick == 0xFF)
                break;
            in->VEnvelope.NodePoints[n].Tick = tick;
            in->VEnvelope.NodePoints[n].Magnitude = (int8_t)mag;
            n++;
        }
        in->VEnvelope.Num = (uint8_t)n;
        in->VEnvelope.LpB = vls;
        in->VEnvelope.LpE = vle;
        in->VEnvelope.SLB = sls;
        in->VEnvelope.SLE = sle;
        /* old flags: 1 = vol env on, 2 = loop, 4 = susloop */
        in->VEnvelope.Flags = (uint8_t)((flags & 1) |
                                        ((flags & 2) ? 2 : 0) |
                                        ((flags & 4) ? 4 : 0));
    }
}

void Load_OldInstrument(const uint8_t *src, instrument_t *in)
{
    LoadOldInstrument(src, in);
}

/* ---------------------------------------------------------------- */

static void FreeSongData(void)
{
    int i;

    for (i = 0; i < MAX_PATTERNS; i++) {
        free(Song.Patterns[i].PackedData);
        Song.Patterns[i].PackedData = NULL;
    }
    for (i = 0; i < MAX_SAMPLES; i++) {
        free(Song.Smp[i].Data);
        Song.Smp[i].Data = NULL;
    }
    memset(&Song, 0, sizeof(Song));
}

int Music_LoadIT(const char *path)
{
    FILE *fp;
    uint8_t *filedata;
    long filesize;
    reader_t rr, *r = &rr;
    uint32_t insoffs[MAX_INSTRUMENTS];
    uint32_t smpoffs[MAX_SAMPLES];
    uint32_t patoffs[MAX_PATTERNS];
    uint16_t i;

    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END);
    filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (filesize < 0xC0) {
        fclose(fp);
        return 0;
    }
    filedata = (uint8_t *)malloc((size_t)filesize);
    if (!filedata || fread(filedata, 1, (size_t)filesize, fp) !=
                         (size_t)filesize) {
        free(filedata);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    r->data = filedata;
    r->size = (size_t)filesize;
    r->pos = 0;

    FreeSongData();
    SetDefaultMIDIDataArea();

    /* song header */
    if (!rd_read(r, &Song.Header, sizeof(songheader_t)) ||
        Song.Header.ID != 0x4D504D49u /* "IMPM" */) {
        free(filedata);
        return 0;
    }

    if (Song.Header.OrdNum > MAX_ORDERS ||
        Song.Header.InsNum > MAX_INSTRUMENTS ||
        Song.Header.SmpNum > MAX_SAMPLES ||
        Song.Header.PatNum > MAX_PATTERNS) {
        free(filedata);
        return 0;
    }

    memset(Song.Orders, 0xFF, sizeof(Song.Orders));
    if (!rd_read(r, Song.Orders, Song.Header.OrdNum))
        goto fail;

    for (i = 0; i < Song.Header.InsNum; i++)
        insoffs[i] = rd_u32(r);
    for (i = 0; i < Song.Header.SmpNum; i++)
        smpoffs[i] = rd_u32(r);
    for (i = 0; i < Song.Header.PatNum; i++)
        patoffs[i] = rd_u32(r);

    /* embedded MIDI macros: skip edit history first */
    {
        size_t pos = r->pos;

        if (Song.Header.Special & 2) {
            uint16_t hist = rd_u16(r);
            pos = r->pos + (size_t)hist * 8;
        }
        if ((Song.Header.Special & 8) && rd_seek(r, pos)) {
            rd_read(r, MIDIDataArea, MIDIDATAAREA_SIZE);
        }
    }

    /* song message (Special bit 0) into the editor's buffer */
    memset(IT_MessageData, 0, sizeof(IT_MessageData));
    if ((Song.Header.Special & 1) && Song.Header.MsgLgth > 1 &&
        rd_seek(r, Song.Header.MsgOffset)) {
        uint16_t n = Song.Header.MsgLgth;
        if (n > IT_MESSAGELENGTH - 1)
            n = IT_MESSAGELENGTH - 1;
        rd_read(r, IT_MessageData, n);
        IT_MessageData[IT_MESSAGELENGTH - 1] = 0;
    }
    Save_LoadTime = time(NULL);

    /* instruments */
    for (i = 0; i < Song.Header.InsNum; i++) {
        if (insoffs[i] == 0 || !rd_seek(r, insoffs[i]))
            continue;

        if (Song.Header.Cmwt >= 0x200) {
            rd_read(r, &Song.Ins[i], sizeof(instrument_t));
        } else if (insoffs[i] + 554 <= r->size) {
            LoadOldInstrument(filedata + insoffs[i], &Song.Ins[i]);
        }
    }

    /* sample headers + data */
    for (i = 0; i < Song.Header.SmpNum; i++) {
        sample_t *s = &Song.Smp[i];

        if (smpoffs[i] == 0 || !rd_seek(r, smpoffs[i]))
            continue;
        if (!rd_read(r, s, 0x50))
            goto fail;
        s->Data = NULL;

        if (!(s->Flags & 1) || s->Length == 0)
            continue;
        if (s->Length > 0x0FFFFFFF)
            goto fail;

        {
            int is16 = (s->Flags & 2) != 0;
            int compressed = (s->Flags & 8) != 0;
            int it215 = compressed && (s->Cvt & 4) != 0;
            uint32_t len = s->Length;
            size_t bytes = (size_t)len << (is16 ? 1 : 0);
            uint8_t *data = (uint8_t *)calloc(len + 4, is16 ? 2 : 1);

            if (!data)
                goto fail;
            s->Data = data;

            if (!rd_seek(r, s->OffsetInFile)) {
                /* missing data: keep silence */
            } else if (compressed) {
                if (is16)
                    DecompressIT16(r, (int16_t *)data, len, it215);
                else
                    DecompressIT8(r, (int8_t *)data, len, it215);
            } else {
                size_t avail = r->size - r->pos;
                if (bytes > avail)
                    bytes = avail;
                memcpy(data, r->data + r->pos, bytes);

                /* unsigned -> signed conversion (Cvt bit 0 = signed) */
                if (!(s->Cvt & 1)) {
                    if (is16) {
                        int16_t *p = (int16_t *)data;
                        uint32_t n;
                        for (n = 0; n < len; n++)
                            p[n] = (int16_t)((uint16_t)p[n] ^ 0x8000);
                    } else {
                        uint32_t n;
                        for (n = 0; n < len; n++)
                            data[n] = (uint8_t)(data[n] ^ 0x80);
                    }
                }
            }

            /* pad for the cubic interpolator (reads pos+1, pos+2) */
            if (is16) {
                int16_t *p = (int16_t *)data;
                p[len] = p[len + 1] = (len > 0) ? p[len - 1] : 0;
                p[len + 2] = p[len + 3] = 0;
            } else {
                int8_t *p = (int8_t *)data;
                p[len] = p[len + 1] = (len > 0) ? p[len - 1] : 0;
                p[len + 2] = p[len + 3] = 0;
            }

            /* clamp loop points defensively (malformed files) */
            if (s->LoopEnd > s->Length)
                s->LoopEnd = s->Length;
            if (s->SusLoopEnd > s->Length)
                s->SusLoopEnd = s->Length;
            if (s->LoopBeg > s->Length)
                s->LoopBeg = 0;
            if (s->SusLoopBeg > s->Length)
                s->SusLoopBeg = 0;
            s->ViT &= 3;

            /* in-memory header cleanup, as D_LoadSampleData does:
             * data is now flat signed */
            s->Flags &= (uint8_t)~0x0C;
            s->Cvt = 1;
        }
    }

    /* patterns: keep the packed stream verbatim */
    for (i = 0; i < Song.Header.PatNum; i++) {
        uint16_t length, rows;

        if (patoffs[i] == 0 || !rd_seek(r, patoffs[i]))
            continue;

        length = rd_u16(r);
        rows = rd_u16(r);
        rd_u32(r);              /* 4 unused bytes */

        if (rows == 0 || r->pos + length > r->size)
            continue;

        Song.Patterns[i].Rows = rows;
        Song.Patterns[i].DataLength = length;
        /* one extra 0 byte so the row decoder always terminates */
        Song.Patterns[i].PackedData = (uint8_t *)malloc((size_t)length + 1);
        if (!Song.Patterns[i].PackedData)
            goto fail;
        memcpy(Song.Patterns[i].PackedData, r->data + r->pos, length);
        Song.Patterns[i].PackedData[length] = 0;
    }

    free(filedata);
    return 1;

fail:
    FreeSongData();
    free(filedata);
    return 0;
}

void Music_FreeIT(void)
{
    FreeSongData();
}
