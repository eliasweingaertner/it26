/*
 * it_save.c
 * ---------
 * .IT module writer. Transliterated from the original editor's disk
 * module: D_SaveIT (IT_D_WM.INC line 53), the bit-width LUT setup block
 * inside it, and IT_DISK.ASM's WriteBits / D_SaveSampleData /
 * D_SaveSampleDataCompressed / D_SaveBlock / D_DeleteIfError.
 *
 * The writer never modifies the in-memory song: all header fixups
 * happen in local buffers, exactly as the original patches only its
 * DiskDataArea copies.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "it_music.h"
#include "it_save.h"

uint8_t SaveFormat = 3;                 /* SWITCH.INC DEFAULTFORMAT = 3 */

char IT_MessageData[IT_MESSAGELENGTH];

/* set by the loader / editor when a module is (re)initialised; feeds
 * the header's obfuscated edit-time counter (18.2 Hz DOS ticks). */
time_t Save_LoadTime;

void (*Save_Progress)(int stage, int param);

uint16_t Msg_GetMessageLength(void)     /* strlen + 1, as the original */
{
    uint16_t n = 0;
    while (n < IT_MESSAGELENGTH && IT_MessageData[n])
        n++;
    return (uint16_t)(n + 1);
}

/* ---- D_SaveBlock: sticky-error block writer ------------------------ */

static FILE *SaveFile;
static int   NoSaveError;

static int save_block(const void *p, size_t n)
{
    if (n == 0)
        return 1;
    if (NoSaveError)
        return 0;
    if (fwrite(p, 1, n, SaveFile) != n) {
        NoSaveError = 1;
        return 0;
    }
    return 1;
}

/* ---- bit-width lookup tables (D_SaveIT table-setup block) ----------
 * Built byte-for-byte as the original streams them into
 * DiskDataArea:10240 (8-bit table) and 10496.. (six 256-byte pages for
 * the two-level 16-bit lookup). */

static const uint8_t BitLUT[23] = {
    1, 2, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5,
    4, 4, 4, 4, 3, 3, 2,
};

static const uint8_t BitLUT16[32] = {
    1,           10*8 + 3,    11*8 + 5,    11*8 + 3,
    12*8 + 5,    12*8 + 5,    12*8 + 5,    12*8 + 3,
    13*8 + 5,    13*8 + 5,    13*8 + 5,    13*8 + 5,
    13*8 + 5,    13*8 + 5,    13*8 + 5,    13*8 + 3,
    13*8 + 4,    13*8 + 5,    13*8 + 5,    13*8 + 5,
    13*8 + 5,    13*8 + 5,    13*8 + 5,    13*8 + 5,
    12*8 + 4,    12*8 + 5,    12*8 + 5,    12*8 + 5,
    11*8 + 4,    11*8 + 5,    10*8 + 4,    2,
};

static uint8_t Lut8[256];
static uint8_t Lut16[6][256];           /* [0]=main (by high byte),
                                           [1..5]=aux pages (by low)  */
static int LutsReady;

static uint8_t *lut_fill(uint8_t *d, uint8_t v, int n)
{
    memset(d, v, (size_t)n);
    return d + n;
}

static void build_luts(void)
{
    uint8_t *d;

    /* 8-bit: BitLUT[0..15], 16x6, 28x7, 64x8, 8x9, 64x8, 29x7, 16x6,
     * BitLUT[8..22] */
    d = Lut8;
    memcpy(d, BitLUT, 16);       d += 16;
    d = lut_fill(d, 6, 16);
    d = lut_fill(d, 7, 28);
    d = lut_fill(d, 8, 64);
    d = lut_fill(d, 9, 8);
    d = lut_fill(d, 8, 64);
    d = lut_fill(d, 7, 29);
    d = lut_fill(d, 6, 16);
    memcpy(d, BitLUT + 8, 15);   d += 15;

    /* 16-bit main page (indexed by the delta's high byte; entry =
     * base<<3 | aux page index) */
    d = Lut16[0];
    memcpy(d, BitLUT16, 16);     d += 16;
    d = lut_fill(d, 14*8 + 5, 15);
    d = lut_fill(d, 14*8 + 3, 1);
    d = lut_fill(d, 15*8 + 5, 31);
    d = lut_fill(d, 15*8 + 3, 1);
    d = lut_fill(d, 16*8 + 5, 63);
    d = lut_fill(d, 16*8 + 3, 1);
    d = lut_fill(d, 16*8 + 4, 1);
    d = lut_fill(d, 16*8 + 5, 63);
    d = lut_fill(d, 15*8 + 4, 1);
    d = lut_fill(d, 15*8 + 5, 31);
    d = lut_fill(d, 14*8 + 4, 1);
    d = lut_fill(d, 14*8 + 5, 15);
    memcpy(d, BitLUT16 + 16, 16); d += 16;

    /* aux pages 1..5, streamed contiguously exactly as the original
     * (the 16x10 and 16x1 runs straddle page boundaries) */
    d = Lut16[1];
    memcpy(d, BitLUT, 16);       d += 16;      /* LUT1: high byte 0    */
    d = lut_fill(d, 6, 16);
    d = lut_fill(d, 7, 24);
    d = lut_fill(d, 8, 64);
    d = lut_fill(d, 9, 128);
    d = lut_fill(d, 10, 16);                   /* LUT2: high byte 0FFh */
    d = lut_fill(d, 9, 128);
    d = lut_fill(d, 8, 64);
    d = lut_fill(d, 7, 25);
    d = lut_fill(d, 6, 16);
    memcpy(d, BitLUT + 8, 15);   d += 15;
    d = lut_fill(d, 0, 256 - 8);               /* aux 3/4/5 zero pages */
    d = lut_fill(d, 1, 16);
    d = lut_fill(d, 0, 256 - 8 + 256);

    LutsReady = 1;
}

static int bits16(uint16_t v)
{
    uint8_t e = Lut16[0][v >> 8];
    return (e >> 3) + Lut16[e & 7][v & 0xFF];
}

/* ---- WriteBits + block buffer -------------------------------------- */

#define BLOCKSLACK 0x10000              /* worst case > input size */
static uint8_t  BlockBuf[2 + BLOCKSLACK];
static uint32_t BitAcc;
static int      BitCnt;
static int      BlockLen;

static void write_bits(unsigned val, int nbits)
{
    BitAcc |= (val & ((1u << nbits) - 1)) << BitCnt;
    BitCnt += nbits;
    while (BitCnt >= 8) {
        if (BlockLen < BLOCKSLACK)
            BlockBuf[2 + BlockLen] = (uint8_t)BitAcc;
        BlockLen++;
        BitAcc >>= 8;
        BitCnt -= 8;
    }
}

/* ---- bit-table minimise pass (D_MinimiseBitTable / ...16BitTable) --
 * For every width dl, runs of equal width bounded by wider neighbours
 * are widened when keeping them would cost more in width-change
 * escapes than it saves; runs of length 1 are always widened. */
static void minimise_table(uint8_t *bt, int count, int is16)
{
    int maxw    = is16 ? 16 : 8;
    int runcap  = is16 ? 33 : 17;
    int addv    = is16 ? 4 : 3;
    int sentinel = maxw + 1;            /* boundary before element 0 */
    int dl, si, di = 0, run;

    for (dl = 1; dl <= maxw; dl++) {
        run = 0;
        for (si = 0; si < count; si++) {
            if (run == 0) {
                if (bt[si] == dl) {
                    di = si;
                    run = 1;
                }
                continue;
            }
            if (bt[si] == dl) {
                run++;
                continue;
            }
            /* run = bt[di..si-1], ended by bt[si] */
            if (run <= runcap) {
                int bl = bt[si];
                int bh = (di > 0) ? bt[di - 1] : sentinel;
                int minb, other, convert = 0;

                if (bl > bh) { int t = bl; bl = bh; bh = t; }
                if (bl > dl) {
                    minb = bl; other = bh; convert = 1;
                } else if (bh > dl) {
                    minb = bh; other = bl; convert = 1;
                } else {
                    minb = other = 0;   /* both below: keep */
                }
                if (convert) {
                    if (run != 1) {
                        int cost = minb + (minb <= 5 ? addv : 0)
                                        + (other <= 5 ? addv : 0);
                        long wasted = (long)run * (minb - dl);
                        if (run > cost || wasted > cost)
                            convert = 0;
                    }
                    if (convert) {
                        int k;
                        for (k = di; k < si; k++)
                            bt[k] = (uint8_t)minb;
                    }
                }
            }
            run = 0;
        }
    }
}

/* ---- bitstream conversion (D_ConvertBitBuffer / ...16BitBuffer) ---- */

static void convert_block8(const uint8_t *deltas, const uint8_t *bt,
                           int count)
{
    int cur = 9, i;
    for (i = 0; i < count; i++) {
        int nw = bt[i];
        if (nw != cur) {
            if (cur <= 6) {
                write_bits(1u << (cur - 1), cur);
                write_bits((unsigned)(nw - 1 - (cur < nw ? 1 : 0)), 3);
            } else if (cur <= 8) {
                unsigned v = (1u << (cur - 1)) - 5 + (unsigned)nw
                             - (cur < nw ? 1u : 0u);
                write_bits(v, cur);
            } else {
                write_bits(0x100u | ((unsigned)(nw - 1) & 0xFF), 9);
            }
            cur = nw;
        }
        write_bits(deltas[i], cur);
    }
}

static void convert_block16(const uint16_t *deltas, const uint8_t *bt,
                            int count)
{
    int cur = 17, i;
    for (i = 0; i < count; i++) {
        int nw = bt[i];
        if (nw != cur) {
            if (cur <= 6) {
                write_bits(1u << (cur - 1), cur);
                write_bits((unsigned)(nw - 1 - (cur < nw ? 1 : 0)), 4);
            } else if (cur <= 16) {
                /* the original's borrow only touches the low byte
                 * (SbB BL,0) -- replicated bit-exactly */
                unsigned v = ((1u << (cur - 1)) - 9 + (unsigned)nw)
                             & 0xFFFF;
                if (cur < nw)
                    v = (v & 0xFF00u) | ((v - 1) & 0xFFu);
                write_bits(v, cur);
            } else {
                write_bits(0x10000u | ((unsigned)(nw - 1) & 0xFF), 17);
            }
            cur = nw;
        }
        write_bits(deltas[i], cur);
    }
}

/* ---- sample data writers ------------------------------------------- */

static int save_sample_raw(const sample_t *s)   /* D_SaveSampleData */
{
    uint32_t bytes = s->Length << ((s->Flags & 2) ? 1 : 0);
    return save_block(s->Data, bytes);
}

/* D_SaveSampleDataCompressed: 32KB blocks; delta (twice for IT215),
 * bit table, minimise, bitstream; u16 length prefix per block. */
static int save_sample_compressed(const sample_t *s)
{
    static uint8_t dbuf[32768];
    static uint8_t btab[32768];
    int is16 = (s->Flags & 2) != 0;
    uint32_t bytes = s->Length << (is16 ? 1 : 0);
    const uint8_t *src = (const uint8_t *)s->Data;

    if (!LutsReady)
        build_luts();

    while (bytes) {
        uint32_t chunk = bytes > 32768 ? 32768 : bytes;
        int count = is16 ? (int)(chunk >> 1) : (int)chunk;
        int passes = (SaveFormat == 3) ? 2 : 1;
        int p, i;

        if (is16) {
            uint16_t *d16 = (uint16_t *)dbuf;
            uint16_t prev = 0, orig;
            for (i = 0; i < count; i++) {
                orig = (uint16_t)(src[i*2] | (src[i*2 + 1] << 8));
                d16[i] = (uint16_t)(orig - prev);
                prev = orig;
            }
            for (p = 1; p < passes; p++) {
                prev = 0;
                for (i = 0; i < count; i++) {
                    orig = d16[i];
                    d16[i] = (uint16_t)(orig - prev);
                    prev = orig;
                }
            }
            for (i = 0; i < count; i++)
                btab[i] = (uint8_t)bits16(d16[i]);
        } else {
            uint8_t prev = 0, orig;
            for (i = 0; i < count; i++) {
                orig = src[i];
                dbuf[i] = (uint8_t)(orig - prev);
                prev = orig;
            }
            for (p = 1; p < passes; p++) {
                prev = 0;
                for (i = 0; i < count; i++) {
                    orig = dbuf[i];
                    dbuf[i] = (uint8_t)(orig - prev);
                    prev = orig;
                }
            }
            for (i = 0; i < count; i++)
                btab[i] = Lut8[dbuf[i]];
        }

        minimise_table(btab, count, is16);

        BitAcc = 0;
        BitCnt = 0;
        BlockLen = 0;
        if (is16)
            convert_block16((const uint16_t *)dbuf, btab, count);
        else
            convert_block8(dbuf, btab, count);
        if (BitCnt > 0 && BlockLen < BLOCKSLACK)
            BlockBuf[2 + BlockLen++] = (uint8_t)BitAcc;
        BlockBuf[0] = (uint8_t)BlockLen;
        BlockBuf[1] = (uint8_t)(BlockLen >> 8);
        if (BlockLen > 0xFFFF || !save_block(BlockBuf, (size_t)BlockLen + 2))
            return 0;

        src   += chunk;
        bytes -= chunk;
    }
    return 1;
}

/* ---- helpers for the header pass ----------------------------------- */

static int mem_nonzero(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        if (b[i])
            return 1;
    return 0;
}

/* Music_GetNumberOfSamples / ...Instruments: index of the last slot
 * that differs from a pristine (all-zero in this port) header. */
static int count_samples(void)
{
    int n;
    for (n = 99; n >= 1; n--)
        if (mem_nonzero(&Song.Smp[n - 1], 80))
            break;
    return n;
}

static int count_instruments(void)
{
    int n;
    for (n = 99; n >= 1; n--)
        if (mem_nonzero(&Song.Ins[n - 1], 554))
            break;
    return n;
}

static int max_pattern(void)            /* PE_GetMaxPattern */
{
    int p, last = 0;
    for (p = 0; p < 200 && p < MAX_PATTERNS; p++)
        if (Song.Patterns[p].PackedData && Song.Patterns[p].DataLength)
            last = p;
    return last;
}

static void wr32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;        d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

static void progress(int stage, int param)
{
    if (Save_Progress)
        Save_Progress(stage, param);
}

/* ---- D_SaveIT ------------------------------------------------------- */

int Save_ITModule(const char *path)
{
    static uint8_t head[0xC0 + 257 + 4 * (99 + 99 + 200)];
    static uint8_t smphdr[99 * 80];
    songheader_t h;
    uint32_t off, msglen, smppos_provisional;
    long smphdr_filepos;
    int ins, smp, pat, i, n, hlen;

    SaveFile = fopen(path, "wb");
    if (!SaveFile)
        return 0;
    NoSaveError = 0;

    h = Song.Header;                    /* fixups in the copy only */

    /* OrdNum: 257 - trailing-0FFh count (via the last non-FF index) */
    n = 0;
    for (i = 255; i > 0; i--)
        if (Song.Orders[i] != 0xFF) {
            n = i;
            break;
        }
    h.OrdNum = (uint16_t)(n + 2);

    ins = count_instruments();
    smp = count_samples();
    pat = max_pattern() + 1;
    h.InsNum = (uint16_t)ins;
    h.SmpNum = (uint16_t)smp;
    h.PatNum = (uint16_t)pat;

    h.Cwt = 0x0217;                     /* TRACKERVERSION */
    if (SaveFormat == 0)
        h.Cmwt = 0x214;
    else if (SaveFormat == 3)
        h.Cmwt = 0x215;
    else
        h.Cmwt = ins ? 0x200 : 0x100;
    for (i = 0; i < ins; i++)           /* filter envelopes need 2.16 */
        if (Song.Ins[i].PtEnvelope.Flags & 0x80)
            h.Cmwt = 0x216;

    h.Special = 4;                      /* row hilight information */
    if (h.PHiligt == 0)
        h.PHiligt = 0x1004;             /* IT default 4/16 (the original
                                           takes the PE's config)     */
    if (Song.Header.Flags & ITF_REQ_MACROS)
        h.Special |= 8;                 /* embedded MIDI configuration */

    /* obfuscated cumulative edit time (Reserved dword) */
    {
        uint32_t ticks = 0;
        if (Save_LoadTime)
            ticks = (uint32_t)((double)(time(NULL) - Save_LoadTime)
                               * 18.2);
        uint32_t e = ticks + h.Reserved;
        e ^= 0x4A54484Cu;               /* 'JTHL' */
        e = (e >> 4) | (e << 28);       /* ROR 4  */
        e = 0u - e;                     /* NEG    */
        e = (e << 7) | (e >> 25);       /* ROL 7  */
        e ^= 0x4954524Bu;               /* 'ITRK' */
        h.Reserved = e;
    }

    /* first data offset: header + orders + the three offset tables,
     * then MIDI config, then message */
    off = 0xC0u + h.OrdNum + 4u * (uint32_t)(ins + smp + pat);
    if (h.Special & 8)
        off += MIDIDATAAREA_SIZE;
    msglen = Msg_GetMessageLength();
    if (msglen != 1) {
        h.Special |= 1;
        h.MsgLgth = (uint16_t)msglen;
        h.MsgOffset = off;
        off += msglen;
    } else {
        h.MsgLgth = 0;
        h.MsgOffset = 0;
    }

    /* assemble header block: header, orders, offset tables */
    memcpy(head, &h, 0xC0);
    hlen = 0xC0;
    memcpy(head + hlen, Song.Orders, (size_t)h.OrdNum - 1);
    hlen += h.OrdNum - 1;
    head[hlen++] = 0xFF;

    for (i = 0; i < ins; i++) {         /* instrument offsets */
        wr32(head + hlen, off);
        hlen += 4;
        off += 554;
    }
    smppos_provisional = off + 80u * (uint32_t)smp;
    for (i = 0; i < smp; i++) {         /* sample header offsets */
        wr32(head + hlen, off);
        hlen += 4;
        off += 80;
    }
    for (i = 0; i < pat; i++) {         /* pattern offsets */
        if (i < MAX_PATTERNS && Song.Patterns[i].PackedData &&
            Song.Patterns[i].DataLength) {
            wr32(head + hlen, off);
            off += (uint32_t)Song.Patterns[i].DataLength + 8;
        } else
            wr32(head + hlen, 0);
        hlen += 4;
    }

    progress(0, 0);                     /* "File Header" */
    save_block(head, (size_t)hlen);

    if (h.Special & 8)
        save_block(MIDIDataArea, MIDIDATAAREA_SIZE);
    if (h.Special & 1)
        save_block(IT_MessageData, msglen);

    progress(1, 0);                     /* "Instrument Headers" */
    if (ins)
        save_block(Song.Ins, 554u * (uint32_t)ins);

    /* sample headers: scratch copies with Flags/Cvt/offset patched
     * (provisional offsets assume uncompressed data; the block is
     * rewritten after the data pass with the real positions) */
    progress(2, 0);                     /* "Sample Headers" */
    {
        uint32_t spos = smppos_provisional;
        /* account for the pattern blocks between headers and data */
        for (i = 0; i < pat; i++)
            if (i < MAX_PATTERNS && Song.Patterns[i].PackedData &&
                Song.Patterns[i].DataLength)
                spos += (uint32_t)Song.Patterns[i].DataLength + 8;

        for (i = 0; i < smp; i++) {
            uint8_t *sh = &smphdr[i * 80];
            memcpy(sh, &Song.Smp[i], 80);
            if (sh[0x12] & 1) {         /* sample present */
                uint8_t fl = sh[0x12], cv = 1;
                fl |= 8;                /* compressed */
                if (SaveFormat == 2)
                    fl &= (uint8_t)~8;
                if (SaveFormat == 3)
                    cv |= 4;            /* IT215 double delta */
                sh[0x12] = fl;
                sh[0x2E] = cv;
                wr32(sh + 0x48, spos);
                {
                    uint32_t len = Song.Smp[i].Length;
                    if (fl & 2)
                        len += len;
                    spos += len;
                }
            }
        }
    }
    smphdr_filepos = ftell(SaveFile);
    save_block(smphdr, 80u * (uint32_t)smp);

    /* patterns: 8-byte header (length, rows, 4 reserved) + data */
    for (i = 0; i < pat; i++) {
        const pattern_t *p = (i < MAX_PATTERNS) ? &Song.Patterns[i]
                                                : NULL;
        progress(3, i);                 /* "Pattern n" */
        if (p && p->PackedData && p->DataLength) {
            uint8_t ph[8] = { 0 };
            ph[0] = (uint8_t)p->DataLength;
            ph[1] = (uint8_t)(p->DataLength >> 8);
            ph[2] = (uint8_t)p->Rows;
            ph[3] = (uint8_t)(p->Rows >> 8);
            save_block(ph, 8);
            save_block(p->PackedData, p->DataLength);
        }
    }

    /* sample data: patch each scratch header with the real position
     * first (the original does this for every sample, present or not) */
    for (i = 0; i < smp; i++) {
        const sample_t *s = &Song.Smp[i];
        long pos = ftell(SaveFile);

        wr32(&smphdr[i * 80 + 0x48], (uint32_t)pos);
        if (!(s->Flags & 1) || s->Length == 0 || !s->Data)
            continue;
        progress(4, i + 1);             /* "Sample n" */
        if (SaveFormat == 2)
            save_sample_raw(s);
        else
            save_sample_compressed(s);
        if (NoSaveError)
            break;
    }

    /* WriteITSampleBlock: rewrite the header block with real offsets */
    if (!NoSaveError && smp) {
        if (fseek(SaveFile, smphdr_filepos, SEEK_SET) == 0)
            save_block(smphdr, 80u * (uint32_t)smp);
        else
            NoSaveError = 1;
        fseek(SaveFile, 0, SEEK_END);
    }

    fclose(SaveFile);
    SaveFile = NULL;

    if (NoSaveError) {                  /* D_DeleteIfError */
        remove(path);
        return 0;
    }
    progress(5, 0);                     /* "Done" */
    return 1;
}

/* ===================================================================
 * D_SaveS3M (IT_D_WM.INC 753): Scream Tracker 3 export, SaveFormat 1.
 * Feature 012. Content S3M cannot express fires the original's
 * warnings (IT_DISK.ASM 611..621) through Save_S3MWarning and is
 * dropped/clamped exactly as the original drops it; the save itself
 * never hard-fails on format limits.
 * =================================================================== */

void (*Save_S3MWarning)(int row, const char *msg);
int Save_S3MWarned;                     /* SaveFormatError */

static void s3m_warn(int row, const char *msg)
{
    Save_S3MWarned = 1;
    if (Save_S3MWarning)
        Save_S3MWarning(row, msg);
}

static void wr16(uint8_t *d, uint16_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
}

/* seek to the next 16-byte boundary at/after `pos`, returning it */
static uint32_t s3m_align(uint32_t pos)
{
    pos = (pos + 15) & ~15u;
    fseek(SaveFile, (long)pos, SEEK_SET);
    return pos;
}

int Save_S3MModule(const char *path)
{
    /* header + orders + 199 parapointers (unclamped quirk space) +
     * pan block */
    static uint8_t head[0x60 + 258 + 2 * (99 + 200) + 32];
    /* per-channel translation scratch (D_SaveS3M25 layout):
     * 0 lastmask, 1 note, 2 ins, 3 vol, 4 cmd, 5 cmdval, 6 note2,
     * 7 ins2 */
    static uint8_t cell[64][8];
    /* worst case: every packed input byte is a repeat-mask entry
     * emitting 6 output bytes (packed data is at most 64K) */
    static uint8_t pbuf[6 * 65536 + 1024];
    static uint32_t datapara[100];      /* ES:10000 table */
    uint32_t hdrlen, filepos;
    int ordnum, smpnum, patnum, i, n, panpos;

    SaveFile = fopen(path, "wb");
    if (!SaveFile)
        return 0;
    NoSaveError = 0;
    Save_S3MWarned = 0;

    memset(head, 0, sizeof(head));
    memcpy(head, Song.Header.SongName, 26);
    head[28] = 0x1A;
    head[29] = 16;                      /* ST3 module type */

    /* OrdNum = last non-FF index + 2 (same scan as the .IT writer) */
    n = 0;
    for (i = 255; i > 0; i--)
        if (Song.Orders[i] != 0xFF) {
            n = i;
            break;
        }
    ordnum = n + 2;
    smpnum = count_samples();
    patnum = max_pattern() + 1;

    hdrlen = 0x60u + (uint32_t)ordnum + 2u * (uint32_t)smpnum
                   + 2u * (uint32_t)patnum;  /* incl. the >100 quirk */
    if (patnum > 100) {
        s3m_warn(23, "Warning: Only 100 patterns supported "
                     "in S3M format");
        patnum = 100;
    }
    wr16(head + 0x20, (uint16_t)ordnum);
    wr16(head + 0x22, (uint16_t)smpnum);
    wr16(head + 0x24, (uint16_t)patnum);
    wr16(head + 0x26, (uint16_t)((Song.Header.Flags & 2) << 2));
    wr16(head + 0x28, 0x3000 + 0x217);  /* Cwt/v: Impulse Tracker */
    wr16(head + 0x2A, 2);               /* Ffi: unsigned samples   */
    memcpy(head + 0x2C, "SCRM", 4);

    if (Song.Header.Flags & 4)
        s3m_warn(29, "Warning: Instrument functions unsupported "
                     "in S3M format");

    head[0x30] = (uint8_t)(Song.Header.GV >> 1);
    head[0x31] = Song.Header.IS;
    head[0x32] = Song.Header.IT;
    head[0x33] = (uint8_t)((Song.Header.MV > 0x7F ? 0x7F
                                                  : Song.Header.MV)
                           | ((Song.Header.Flags & 1) ? 0x80 : 0));
    head[0x34] = 0;                     /* uc */
    head[0x35] = 252;                   /* dp: pan block present */

    {                                   /* obfuscated edit timer */
        uint32_t ticks = 0, e;
        if (Save_LoadTime)
            ticks = (uint32_t)((double)(time(NULL) - Save_LoadTime)
                               * 18.2);
        e = ticks + Song.Header.Reserved;
        e ^= 0x4A54484Cu;               /* 'JTHL' */
        e = (e >> 4) | (e << 28);       /* ROR 4  */
        e = 0u - e;                     /* NEG    */
        e = (e << 7) | (e >> 25);       /* ROL 7  */
        e ^= 0x4954524Bu;               /* 'ITRK' */
        wr32(head + 0x38, e);
    }

    /* orders: ordnum-1 filtered entries + one 0FFh terminator */
    for (i = 0; i < ordnum - 1; i++) {
        uint8_t v = Song.Orders[i];
        if (v < 0xFE && v >= 100)
            v = 0xFF;                   /* patterns >= 100 unlisted */
        head[0x60 + i] = v;
    }
    head[0x60 + ordnum - 1] = 0xFF;

    /* channel settings 40h..5Fh + the 32-byte default-pan block at the
     * end of the header area */
    panpos = (int)hdrlen;
    for (i = 0, n = 0; i < 32; i++) {
        uint8_t p = Song.Header.ChnlPan[i];
        if (p & 0x80) {                 /* muted / nonexistent */
            head[0x40 + i] = 0xFF;
            head[panpos + i] = 0;
        } else {
            uint8_t t;
            if (p > 64)
                p = 32;                 /* surround */
            t = (uint8_t)(p >> 1);
            t = (uint8_t)(t ? t - 1 : 0);
            head[panpos + i] = (uint8_t)((t >> 1) | 32);
            head[0x40 + i] = (uint8_t)(((n & 1) << 3) | (n >> 1));
            n++;                        /* alternating L1,R1,L2,R2... */
        }
    }
    hdrlen += 0x20;

    for (i = 0; i < 32; i++)
        if (Song.Header.ChnlVol[i] != 64) {
            s3m_warn(24, "Warning: Channel volumes unsupported "
                         "in S3M format");
            break;
        }
    if (Song.Header.Flags & 8)
        s3m_warn(25, "Warning: Linear slides unsupported "
                     "in S3M format");

    /* ---- sample headers (50h bytes each, 16-byte aligned) ---- */
    progress(2, 0);                     /* "Sample Headers" */
    filepos = hdrlen;
    for (i = 0; i < smpnum; i++) {
        const sample_t *s = &Song.Smp[i];
        uint8_t sh[0x50];
        int k;

        filepos = s3m_align(filepos);
        wr16(head + 0x60 + ordnum + 2 * i, (uint16_t)(filepos >> 4));

        memset(sh, 0, sizeof(sh));
        sh[0] = (uint8_t)(s->Flags & 1);
        memcpy(sh + 1, s->DOSFileName, 12);
        wr32(sh + 16, s->Length);
        wr32(sh + 20, s->LoopBeg);
        wr32(sh + 24, s->LoopEnd);
        sh[28] = s->Vol;
        sh[31] = (uint8_t)(((s->Flags >> 4) & 1) |
                           ((s->Flags << 1) & 4));
        wr32(sh + 32, s->C5Speed);
        for (k = 0; k < 25; k++) {
            char c = s->SampleName[k];
            sh[48 + k] = (uint8_t)(c ? c : ' ');
        }
        memcpy(sh + 76, "SCRS", 4);

        if ((s->Flags & (16 | 32)) &&
            ((s->Flags & 32) || (s->Flags & 64)))
            s3m_warn(27, "Warning: Sustain and Ping Pong loops "
                         "unsupported in S3M format");
        if (s->GvL != 64)
            s3m_warn(26, "Warning: Sample volumes unsupported "
                         "in S3M format");
        if (s->ViD != 0)
            s3m_warn(28, "Warning: Sample vibrato unsupported "
                         "in S3M format");

        save_block(sh, sizeof(sh));
        filepos += 0x50;
    }

    /* ---- patterns (16-byte aligned; 64-row translation) ---- */
    for (n = 0; n < patnum; n++) {
        const pattern_t *pt = &Song.Patterns[n];
        const uint8_t *si, *end;
        uint8_t *di = pbuf + 2;
        int rows = (pt->PackedData && pt->Rows) ? pt->Rows : 64;
        int row;

        progress(3, n);                 /* "Pattern n" */
        filepos = s3m_align(filepos);
        wr16(head + 0x60 + ordnum + 2 * smpnum + 2 * n,
             (uint16_t)(filepos >> 4));

        if (rows != 64)
            s3m_warn(30, "Warning: Pattern lengths other than 64 rows "
                         "unsupported in S3M format");

        /* empty translation cells (D_SaveS3M25) */
        for (i = 0; i < 64; i++) {
            memset(cell[i], 0, 8);
            cell[i][1] = 0xFD;          /* note */
            cell[i][3] = 0xFF;          /* volume */
        }

        si = pt->PackedData;
        end = si ? si + pt->DataLength : NULL;
        for (row = 0; row < rows; row++) {
            for (;;) {
                uint8_t cv, mask, *c;
                int ch, smask;

                cv = (si && si < end) ? *si++ : 0;
                if (cv == 0 || di > pbuf + sizeof(pbuf) - 8) {
                    *di++ = 0;          /* end of row */
                    break;
                }
                ch = (cv & 0x7F) - 1;
                c = cell[ch & 63];
                if (cv & 0x80)
                    c[0] = (si < end) ? *si++ : 0;
                mask = c[0];
                if (mask & 1)
                    c[1] = (si < end) ? *si++ : 0;
                if (mask & 2)
                    c[2] = (si < end) ? *si++ : 0;
                if (mask & 4) {
                    uint8_t v = (si < end) ? *si++ : 0;
                    if (v > 64) {       /* pans / vol effects */
                        s3m_warn(33, "Warning: Extended volume column "
                                 "effects are unsupported "
                                 "in S3M format");
                        v = 0xFF;
                    }
                    c[3] = v;
                }
                if (mask & 8) {
                    c[4] = (si < end) ? *si++ : 0;
                    c[5] = (si < end) ? *si++ : 0;
                }

                if ((cv & 0x7F) > 16) { /* NUMS3MCHANNELS */
                    s3m_warn(31, "Warning: Data outside 16 channels "
                                 "unsupported in S3M format");
                    continue;           /* cell dropped */
                }

                smask = ch;
                if (mask & 0x33) smask |= 32;
                if (mask & 0x44) smask |= 64;
                if (mask & 0x88) smask |= 128;

                /* note remap through the instrument's sample table */
                c[6] = c[1];
                c[7] = c[2];
                if (c[1] < 120 && (mask & 0x11) && (mask & 0x22) &&
                    (Song.Header.Flags & 4) && c[7] >= 1 && c[7] <= 99)
                    c[6] = Song.Ins[c[7] - 1]
                               .NoteSampleTable[2 * c[1]];

                /* range check on the CACHED note whenever the S3M mask
                 * carries the note/ins field -- even a stale note from
                 * an earlier row drops an ins-only cell (ASM quirk,
                 * D_SaveS3M55 tests no IT mask bit here) */
                if (smask & 32) {
                    uint8_t nt = c[6];
                    if (nt < 0xFD && (nt < 12 || nt >= 108)) {
                        s3m_warn(32, "Warning: Notes outside the range "
                                 "C-1 to B-8 are unsupported "
                                 "in S3M format");
                        continue;       /* cell dropped */
                    }
                }

                *di++ = (uint8_t)smask;
                if (smask & 32) {
                    uint8_t nt = c[6], out = 0xFF;
                    if ((mask & 0x11) && nt != 0xFD) {
                        if (nt >= 0xFE)
                            out = 0xFE; /* cut / off -> ST3 ^^ */
                        else
                            out = (uint8_t)((((nt - 12) / 12) << 4) |
                                            ((nt - 12) % 12));
                    }
                    *di++ = out;
                    *di++ = (mask & 0x22) ? c[7] : 0;
                }
                if (smask & 64)
                    *di++ = c[3];
                if (smask & 128) {
                    uint8_t cm = c[4], cv2 = c[5];
                    if (cm == 'S' - '@' && cv2 == 0x91) {
                        cm = 'X' - '@';
                        cv2 = 0xA4;     /* S91 -> XA4 surround */
                    } else if (cm == 'V' - '@' || cm == 'X' - '@') {
                        cv2 >>= 1;
                    } else if (cm == 'C' - '@') {
                        cv2 = (uint8_t)(((cv2 / 10) << 4) | (cv2 % 10));
                    }
                    *di++ = cm;
                    *di++ = cv2;
                }
            }
        }
        for (; row < 64; row++)         /* pad short patterns */
            *di++ = 0;

        i = (int)(di - pbuf);           /* length word includes itself */
        wr16(pbuf, (uint16_t)i);
        save_block(pbuf, (size_t)i);
        filepos += (uint32_t)i;
    }

    /* ---- sample data (unsigned conversion) ---- */
    memset(datapara, 0, sizeof(datapara));
    for (i = 0; i < smpnum; i++) {
        const sample_t *s = &Song.Smp[i];
        uint32_t bytes, k;
        const uint8_t *src;
        uint8_t buf[4096];

        if (!(s->Flags & 1) || !s->Data || s->Length == 0)
            continue;
        progress(4, i + 1);             /* "Sample n" */
        filepos = s3m_align(filepos);
        datapara[i] = filepos >> 4;

        bytes = s->Length << ((s->Flags & 2) ? 1 : 0);
        src = (const uint8_t *)s->Data;
        for (k = 0; k < bytes && !NoSaveError; ) {
            uint32_t c = bytes - k, j;
            if (c > sizeof(buf))
                c = sizeof(buf);
            if (s->Flags & 2)           /* 16-bit: flip sign bytes */
                for (j = 0; j < c; j++)
                    buf[j] = (uint8_t)(src[k + j] ^
                                       ((k + j) & 1 ? 0x80 : 0));
            else                        /* 8-bit */
                for (j = 0; j < c; j++)
                    buf[j] = (uint8_t)(src[k + j] ^ 0x80);
            save_block(buf, c);
            k += c;
        }
        filepos += bytes;
    }

    /* ---- final passes: header, then the 24-bit memseg patches ---- */
    progress(0, 0);                     /* "File Header" */
    if (!NoSaveError) {
        fseek(SaveFile, 0, SEEK_SET);
        save_block(head, hdrlen);
    }
    for (i = 0; i < smpnum && !NoSaveError; i++) {
        uint32_t hp = (uint32_t)(head[0x60 + ordnum + 2 * i] |
                     (head[0x60 + ordnum + 2 * i + 1] << 8)) << 4;
        uint8_t ms[3];
        ms[0] = (uint8_t)(datapara[i] >> 16);
        ms[1] = (uint8_t)datapara[i];
        ms[2] = (uint8_t)(datapara[i] >> 8);
        fseek(SaveFile, (long)(hp + 0x0D), SEEK_SET);
        save_block(ms, 3);
    }

    fclose(SaveFile);
    SaveFile = NULL;

    if (NoSaveError) {                  /* D_DeleteIfError */
        remove(path);
        return 0;
    }
    progress(5, 0);                     /* "Done" */
    return 1;
}
