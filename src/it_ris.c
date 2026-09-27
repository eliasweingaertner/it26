/*
 * it_ris.c
 * --------
 * Sample / instrument library, ported from IT_D_RIS.INC ("Read
 * Instrument Sample"), IT_D_RI.INC ("Read Instrument") and the
 * record/transfer machinery in IT_DISK.ASM (LSWindow_Enter /
 * LIWindow_Enter / LoadSample / D_SaveSample / D_SaveInstrument).
 * Scanners build one ITS-header record per sample inside a source
 * file; loading a record reads its data with Load_SampleData
 * (D_LoadSampleData). See specs/006-cross-module-sample-load/
 * research.md for the decode notes.
 *
 * Kept quirks: MOD pattern count scans only the first 127 orders and
 * skipped length<=1 samples don't advance the data pointer; FAR uses
 * a hardcoded 256-word pattern-size table and ignores the file's
 * volume byte; MTM/669 chain data offsets through the previous
 * record; PAT takes the loop-end field as the length; the KRZ C5
 * calculation (1e9/period with 22000/44100 fallbacks); XM's
 * clamp-to-4177910 lengths; the instrument transfer overwrites each
 * sample's Cvt/DfP word with 1 (DfP=0); the out-of-slots check uses
 * the count computed when the requester opened.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "it_music.h"
#include "it_ris.h"

#ifdef _WIN32
#include <windows.h>                    /* feature 015: directory listing */
#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif

/* FineTuneTable (IT_DISK.ASM 547): MOD/MTM/XM finetune -> C5 speed */
static const uint16_t FineTuneTable[16] = {
    8363, 8413, 8463, 8529, 8581, 8651, 8723, 8757,
    7895, 7941, 7985, 8046, 8107, 8169, 8232, 8280
};

/* ---- bounded little/big-endian buffer access ------------------- */
static uint8_t b8(const uint8_t *d, size_t n, size_t o)
{
    return (o < n) ? d[o] : 0;
}
static uint16_t b16(const uint8_t *d, size_t n, size_t o)
{
    return (uint16_t)(b8(d, n, o) | (b8(d, n, o + 1) << 8));
}
static uint16_t b16be(const uint8_t *d, size_t n, size_t o)
{
    return (uint16_t)((b8(d, n, o) << 8) | b8(d, n, o + 1));
}
static uint32_t b32(const uint8_t *d, size_t n, size_t o)
{
    return (uint32_t)b16(d, n, o) | ((uint32_t)b16(d, n, o + 2) << 16);
}
static uint32_t b32be(const uint8_t *d, size_t n, size_t o)
{
    return ((uint32_t)b16be(d, n, o) << 16) | b16be(d, n, o + 2);
}

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long sz;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 0x7FFFFFF) {
        fclose(f);
        return NULL;
    }
    d = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!d) {
        fclose(f);
        return NULL;
    }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)sz;
    return d;
}

/* TransferFileName: 'IMPS' + 13 bytes of the source's (DOS) name */
static void transfer_filename(sample_t *s, const char *path)
{
    const char *base = path, *p;
    int i;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    memcpy(&s->ID, "IMPS", 4);
    memset(s->DOSFileName, 0, 12);
    for (i = 0; i < 12 && base[i]; i++) {
        char c = base[i];
        s->DOSFileName[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    s->Zero = 0;
}

static sample_t *ent_init(slibent_t *e, const char *path, uint8_t fmt)
{
    memset(e, 0, sizeof(*e));
    e->Format = fmt;
    snprintf(e->SrcFile, sizeof(e->SrcFile), "%s", path);
    transfer_filename(&e->hdr, path);
    e->hdr.GvL = 64;
    return &e->hdr;
}

/* fixed-width raw name copy (zero-padded to 26) */
static void name_raw(sample_t *s, const uint8_t *d, size_t n, size_t o,
                     int len)
{
    int i;
    memset(s->SampleName, 0, 26);
    for (i = 0; i < len && i < 26; i++)
        s->SampleName[i] = (char)b8(d, n, o + (size_t)i);
}

/* ASCIIZ name copy (stops at NUL, zero-padded to 26) */
static void name_asciiz(sample_t *s, const uint8_t *d, size_t n, size_t o,
                        int maxlen)
{
    int i;
    memset(s->SampleName, 0, 26);
    for (i = 0; i < maxlen && i < 26; i++) {
        uint8_t c = b8(d, n, o + (size_t)i);
        if (!c)
            break;
        s->SampleName[i] = (char)c;
    }
}

/* =================================================================
 * Sample scanners (IT_D_RIS.INC)
 * ================================================================= */

/* LoadMODSamplesInModule: also used for the 15-instrument fallback
 * (with the unchanged 31-instrument offsets, as the original table
 * points both entries at the same routine). */
static int scan_mod(const uint8_t *d, size_t n, const char *path,
                    int channels, slibent_t *ents, int max)
{
    int cnt = 0, i;
    uint32_t patterns = 0, ptr;

    for (i = 0; i < 127; i++) {              /* 127-order quirk */
        uint8_t o = b8(d, n, 952 + (size_t)i);
        if (o > patterns)
            patterns = o;
    }
    patterns++;
    ptr = 1084 + patterns * (uint32_t)channels * 256;

    for (i = 0; i < 31 && cnt < max; i++) {
        size_t off = 20 + (size_t)i * 30;
        uint32_t len = b16be(d, n, off + 22);
        sample_t *s;
        slibent_t *e;

        if (len <= 1)
            continue;                        /* no data-pointer advance */
        e = &ents[cnt++];
        s = ent_init(e, path, 14);
        s->Vol = b8(d, n, off + 25);
        s->Flags = (uint8_t)(1 | (b16be(d, n, off + 28) > 1 ? 0x10 : 0));
        name_raw(s, d, n, off, 22);
        s->Cvt = 1;
        s->DfP = 32;
        s->Length = len * 2;
        e->FileSize = s->Length;
        s->LoopBeg = (uint32_t)b16be(d, n, off + 26) * 2;
        s->LoopEnd = s->LoopBeg + (uint32_t)b16be(d, n, off + 28) * 2;
        s->C5Speed = FineTuneTable[b8(d, n, off + 24) & 15];
        s->OffsetInFile = ptr;
        ptr += s->Length;
    }
    return cnt;
}

static int scan_s3m(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0, i;
    int insnum = b16(d, n, 0x22);
    size_t ptab = 0x60 + b16(d, n, 0x20);

    for (i = 0; i < insnum && cnt < max; i++) {
        size_t off = (size_t)b16(d, n, ptab + (size_t)i * 2) << 4;
        uint8_t flag = b8(d, n, off + 0x1F);
        sample_t *s;
        slibent_t *e;

        if (b8(d, n, off) != 1 || b32(d, n, off + 0x10) == 0)
            continue;
        e = &ents[cnt++];
        s = ent_init(e, path, 3);
        s->Flags = (uint8_t)(1 | ((flag & 1) << 4) | ((flag & 4) >> 1));
        s->Vol = b8(d, n, off + 0x1C);
        name_raw(s, d, n, off + 0x30, 25);
        s->Cvt = 0;                          /* unsigned */
        s->DfP = 32;
        s->Length = b32(d, n, off + 0x10);
        e->FileSize = (s->Flags & 4) ? s->Length * 2 : s->Length;
        s->LoopBeg = b32(d, n, off + 0x14);
        s->LoopEnd = b32(d, n, off + 0x18);
        s->C5Speed = b32(d, n, off + 0x20);
        s->OffsetInFile =
            (((uint32_t)b8(d, n, off + 0x0D) << 16) |
             b16(d, n, off + 0x0E)) << 4;
    }
    return cnt;
}

static int scan_it(const uint8_t *d, size_t n, const char *path,
                   slibent_t *ents, int max)
{
    int cnt = 0, i;
    int insnum = b16(d, n, 0x22), smpnum = b16(d, n, 0x24);
    size_t stab = 0xC0 + b16(d, n, 0x20) + (size_t)insnum * 4;

    for (i = 0; i < smpnum && cnt < max; i++) {
        size_t off = b32(d, n, stab + (size_t)i * 4);
        slibent_t *e;
        sample_t *s;

        if (off + 0x50 > n)
            continue;
        e = &ents[cnt];
        ent_init(e, path, 2);
        s = &e->hdr;
        memcpy(s, d + off, 0x50);            /* header verbatim */
        s->Data = NULL;
        if (!(s->Flags & 1))
            continue;
        e->FileSize = (s->Flags & 2) ? s->Length * 2 : s->Length;
        transfer_filename(s, path);          /* re-stamp ID + filename */
        cnt++;
    }
    return cnt;
}

/* LoadXMHeader: returns the file position of the first instrument */
static size_t xm_first_instrument(const uint8_t *d, size_t n)
{
    size_t pos = 60 + b32(d, n, 60);
    int patterns = b16(d, n, 70), i;

    for (i = 0; i < patterns; i++) {
        uint32_t hlen = b32(d, n, pos);
        uint16_t dlen = b16(d, n, pos + 7);
        pos += hlen + dlen;
    }
    return pos;
}

/* shared with the XI conversion: XM sample header -> ITS fields */
static uint32_t xm_conv_len(uint32_t v, int is16)
{
    if (v >= 4177910)
        v = 4177910;
    return is16 ? v >> 1 : v;
}

static void xm_sample_fields(sample_t *s, const uint8_t *d, size_t n,
                             size_t sh)
{
    uint8_t type = b8(d, n, sh + 14);
    int is16 = (type & 0x10) != 0;
    uint8_t pan = b8(d, n, sh + 15);
    int8_t relnote = (int8_t)b8(d, n, sh + 16);
    int8_t ft = (int8_t)b8(d, n, sh + 13);
    int note = (uint8_t)(relnote + 60);

    s->Vol = b8(d, n, sh + 12);
    s->Flags = (uint8_t)(1 | (is16 ? 2 : 0));
    if ((type & 3) != 0 && b32(d, n, sh + 8) > 1)
        s->Flags |= (uint8_t)(0x10 | (((type & 3) - 1) << 6));
    name_raw(s, d, n, sh + 18, 22);
    s->Cvt = 5;                              /* signed + delta */
    s->DfP = (uint8_t)((pan >> 2) + ((pan >> 1) & 1) + 0x80);
    s->Length = xm_conv_len(b32(d, n, sh), is16);
    s->LoopBeg = xm_conv_len(b32(d, n, sh + 4), is16);
    s->LoopEnd = xm_conv_len(b32(d, n, sh + 4) + b32(d, n, sh + 8), is16);
    if (note > 131)
        note = 131;                          /* PitchTable bound */
    s->C5Speed = (uint32_t)(((uint64_t)PitchTable[note] *
                             FineTuneTable[(ft >> 4) & 15]) >> 16);
}

static int scan_xm(const uint8_t *d, size_t n, const char *path,
                   slibent_t *ents, int max)
{
    int cnt = 0, i, k;
    int instruments = b16(d, n, 72);
    size_t pos = xm_first_instrument(d, n);

    for (i = 0; i < instruments; i++) {
        uint32_t inssize = b32(d, n, pos);
        int nsmp = b16(d, n, pos + 27);
        size_t shdr, datapos;
        uint32_t run = 0;

        if (inssize == 0)
            break;
        shdr = pos + inssize;
        if (nsmp == 0) {
            pos = shdr;
            continue;
        }
        datapos = shdr + (size_t)nsmp * 40;

        for (k = 0; k < nsmp; k++) {
            size_t sh = shdr + (size_t)k * 40;
            uint32_t bytelen = b32(d, n, sh);
            slibent_t *e;
            sample_t *s;

            if (bytelen == 0)
                continue;
            if (cnt >= max)
                return cnt;
            e = &ents[cnt++];
            s = ent_init(e, path, 8);
            xm_sample_fields(s, d, n, sh);
            e->FileSize = bytelen;
            s->OffsetInFile = (uint32_t)(datapos + run);
            run += bytelen;
        }
        {
            uint32_t total = 0;
            for (k = 0; k < nsmp; k++)
                total += b32(d, n, shdr + (size_t)k * 40);
            pos = datapos + total;
        }
    }
    return cnt;
}

static int scan_mtm(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0, i;
    int nos = b8(d, n, 30);
    uint32_t prevptr, prevlen = 0;

    prevptr = 194u + (uint32_t)nos * 37 + b16(d, n, 28) +
              64u * (b8(d, n, 26) + 1) + 192u * b16(d, n, 24);

    for (i = 0; i < nos && cnt < max; i++) {
        size_t off = 66 + (size_t)i * 37;
        uint32_t len = b32(d, n, off + 22);
        slibent_t *e;
        sample_t *s;

        if (len == 0)
            continue;
        e = &ents[cnt++];
        s = ent_init(e, path, 10);
        s->Flags = (uint8_t)((b32(d, n, off + 30) - b32(d, n, off + 26) > 2)
                             ? 0x11 : 1);
        s->Vol = b8(d, n, off + 35);
        name_asciiz(s, d, n, off, 22);
        s->Cvt = 0;                          /* unsigned */
        s->DfP = 32;
        s->Length = len;
        e->FileSize = len;
        s->LoopBeg = b32(d, n, off + 26);
        s->LoopEnd = b32(d, n, off + 30);
        s->C5Speed = FineTuneTable[b8(d, n, off + 34) & 15];
        s->OffsetInFile = prevptr + prevlen; /* record chaining */
        prevptr = s->OffsetInFile;
        prevlen = s->Length;
    }
    return cnt;
}

static int scan_669(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0, i;
    int nos = b8(d, n, 0x6E);
    uint32_t prevptr, prevlen = 0;

    prevptr = 0x1F1u + 0x19u * (uint32_t)nos +
              0x600u * b8(d, n, 0x6F);

    for (i = 0; i < nos && cnt < max; i++) {
        size_t off = 0x1F1 + (size_t)i * 0x19;
        uint32_t len = b32(d, n, off + 13);
        uint32_t lend = b32(d, n, off + 21);
        slibent_t *e;
        sample_t *s;

        if (len == 0)
            continue;
        e = &ents[cnt++];
        s = ent_init(e, path, 11);
        s->Flags = (uint8_t)((lend > len) ? 1 : 0x11);
        s->Vol = 64;
        name_raw(s, d, n, off, 13);
        s->Cvt = 0;                          /* unsigned */
        s->DfP = 32;
        s->Length = len;
        e->FileSize = len;
        s->LoopBeg = b32(d, n, off + 17);
        s->LoopEnd = (s->Flags & 0x10) ? lend : 0;
        s->C5Speed = 8363;
        s->OffsetInFile = prevptr + prevlen;
        prevptr = s->OffsetInFile;
        prevlen = s->Length;
    }
    return cnt;
}

static int scan_far(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0, i, nos = 0;
    size_t textlen = b16(d, n, 96);
    size_t block = 98 + textlen;             /* post-text data */
    uint32_t patbytes = 0;
    uint64_t map;
    size_t pos;

    /* pattern sizes: hardcoded 256 words at overlay offset 357 */
    for (i = 0; i < 256; i++)
        patbytes += b16(d, n, block + (357 - 98) + (size_t)i * 2);
    pos = block + (869 - 98) + patbytes;

    map = (uint64_t)b32(d, n, pos) |
          ((uint64_t)b32(d, n, pos + 4) << 32);
    for (i = 0; i < 64; i++)
        if (map & ((uint64_t)1 << i))
            nos++;
    pos += 8;

    for (i = 0; i < nos; i++) {
        size_t off = pos;
        uint32_t len = b32(d, n, off + 32);
        uint16_t tf = b16(d, n, off + 46);
        slibent_t *e;
        sample_t *s;

        pos += 48;
        if (len == 0)
            continue;                        /* no data follows */
        if (cnt >= max)
            break;
        e = &ents[cnt];
        s = ent_init(e, path, 12);
        s->Flags = (uint8_t)(1 | ((tf & 1) << 1) | (((tf >> 8) & 8) << 1));
        s->Vol = 64;                         /* file's volume ignored */
        name_asciiz(s, d, n, off, 25);
        s->Cvt = 1;
        s->DfP = 32;
        e->FileSize = len;
        {
            int sh = (s->Flags & 2) ? 1 : 0;
            s->Length = len >> sh;
            s->LoopBeg = b32(d, n, off + 38) >> sh;
            s->LoopEnd = b32(d, n, off + 42) >> sh;
        }
        s->C5Speed = 8363;
        s->OffsetInFile = (uint32_t)pos;
        pos += len;
        if (len > 1)                         /* length-1: overwritten */
            cnt++;
    }
    return cnt;
}

static int scan_ptm(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0, i;
    int nos = b16(d, n, 34);

    for (i = 0; i < nos && cnt < max; i++) {
        size_t off = 608 + (size_t)i * 80;
        uint8_t type = b8(d, n, off);
        slibent_t *e;
        sample_t *s;
        int sh;

        if ((type & 3) != 1 || b32(d, n, off + 22) == 0)
            continue;
        e = &ents[cnt++];
        s = ent_init(e, path, 9);
        s->Flags = (uint8_t)(1 | ((type & 4) << 2) | ((type & 0x10) >> 3) |
                             ((type & 8) << 3));
        s->Vol = b8(d, n, off + 13);
        name_raw(s, d, n, off + 0x30, 25);
        s->Cvt = 9;                          /* signed + byte delta */
        s->DfP = 32;
        sh = (s->Flags & 2) ? 1 : 0;
        e->FileSize = b32(d, n, off + 22);
        s->Length = b32(d, n, off + 22) >> sh;
        s->LoopBeg = b32(d, n, off + 26) >> sh;
        s->LoopEnd = b32(d, n, off + 30) >> sh;
        s->C5Speed = b16(d, n, off + 14);
        s->OffsetInFile = b32(d, n, off + 18);
    }
    return cnt;
}

static int scan_krz(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0;
    uint32_t datastart = b32be(d, n, 4);
    uint32_t fofs = 32;

    while (fofs < datastart && cnt < max) {
        size_t blk = fofs;
        uint16_t blksize = b16be(d, n, blk + 6);
        uint8_t type = b8(d, n, blk + 4);
        uint32_t adv = (uint32_t)(blksize + 7) & 0xFFFC;

        if (adv == 0)
            break;                           /* corrupt chain */
        fofs += adv;

        if (type >= 0x98 && type <= 0x9B) {
            size_t hp = blk + b16be(d, n, blk + 8) + 20;
            uint8_t modes = b8(d, n, hp + 1);
            uint32_t start = b32be(d, n, hp + 8);
            uint32_t lbeg = b32be(d, n, hp + 16) - start;
            uint32_t lend = b32be(d, n, hp + 20) - start;
            uint32_t period = b32be(d, n, hp + 28);
            slibent_t *e;
            sample_t *s;

            if (lend < lbeg)
                continue;                    /* abandoned record */
            e = &ents[cnt++];
            s = ent_init(e, path, 15);
            s->Flags = 3;                    /* 16-bit, always */
            if (modes & 2)
                s->Flags |= 0xC0;            /* ping pong */
            if (!(modes & 0x80) &&
                b32be(d, n, hp + 16) != b32be(d, n, hp + 20))
                s->Flags |= 0x10;
            s->Vol = 64;
            name_asciiz(s, d, n, blk + 10, 26);
            s->Cvt = 3;                      /* signed + big endian */
            s->DfP = 32;
            s->Length = lend;                /* end - start */
            e->FileSize = s->Length * 2;
            s->LoopBeg = lbeg;
            s->LoopEnd = lend;
            if (period == 0) {
                s->C5Speed = 22000;
            } else {
                uint32_t q = 1000000000u / period;
                s->C5Speed = q ? q : 44100;
            }
            s->OffsetInFile = datastart + start * 2;
        }
    }
    return cnt;
}

static int scan_pat(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents, int max)
{
    int cnt = 0, i, k;
    int waves = b8(d, n, 129 + 63 + 6);
    size_t pos = 129 + 63 + 47;

    for (i = 0; i < waves && cnt < max; i++) {
        size_t w = pos;
        uint8_t modes = b8(d, n, w + 55);
        int sh = (modes & 1) ? 1 : 0;
        slibent_t *e = &ents[cnt++];
        sample_t *s = ent_init(e, path, 16);
        int p = 0;

        pos += 96;
        s->Flags = (uint8_t)(1 | ((modes & 1) << 1) |
                             ((modes & 4) ? 0x10 : 0) |
                             ((modes & 8) ? 0x40 : 0));
        s->Vol = 64;
        memset(s->SampleName, 0, 26);
        for (k = 0; k < 16; k++) {           /* instrument name */
            uint8_t c = b8(d, n, 129 + 2 + (size_t)k);
            if (!c)
                break;
            s->SampleName[p++] = (char)c;
        }
        s->SampleName[p++] = ':';
        for (k = 0; k < 7 && p < 26; k++) {  /* wave name */
            uint8_t c = b8(d, n, w + (size_t)k);
            if (!c)
                break;
            s->SampleName[p++] = (char)c;
        }
        s->Cvt = (uint8_t)(((modes & 2) >> 1) ^ 1);
        s->DfP = 32;
        e->FileSize = b32(d, n, w + 16);
        s->Length = b32(d, n, w + 16) >> sh; /* loop-end field! */
        s->LoopBeg = b32(d, n, w + 12) >> sh;
        s->LoopEnd = b32(d, n, w + 16) >> sh;
        s->C5Speed = b16(d, n, w + 20);
        s->OffsetInFile = (uint32_t)pos;
        pos += b32(d, n, w + 8);             /* true data size */
    }
    return cnt;
}

/* D_GetSampleInfo8 (IT_D_INF.INC 856..1000): standalone WAV file ->
 * one synthesized ITS record, format 5 (8-bit) / 7 (16-bit).
 * Identification, quirks kept 1:1 (specs/008 research.md R1/R2):
 *  - bytes 8..15 must be "WAVEfmt "; the leading "RIFF" is NOT checked;
 *  - bytes 18..21 must be 00 00 01 00 (fmt-size high word 0, PCM tag);
 *  - the data chunk is found by a bounded walk of at most 3 chunks
 *    from 20 + fmt-size low word, advancing low_word(size) + 8 (the
 *    DOS code skips with a 16-bit read of the 32-bit size);
 *  - only 8/16 bits per sample qualify; nChannels == 2 -> stereo,
 *    every other channel count is treated as mono;
 *  - length = min(size dword, 4177910) bytes -> frames (>>1 per
 *    16-bit and stereo); C5Speed = the rate's low 16 bits only. */
/* D_GetSampleInfo13 (IT_D_INF.INC 1002; feature 013): IFF 'FORM' with
 * '8SVX' (8-bit) or '16SV' (16-bit) -- the "AIFF Sample" record,
 * format code 17. Chunk walk from offset 12 advancing by the SIZE
 * field's LOW word + 8 (original 16-bit arithmetic); NAME fills the
 * sample name (cap 25), VHDR the loop fields + rate, BODY finishes
 * the record. Big-endian values halve for 16-bit samples
 * (D_GetSampleInfoBSwap); the rate does not. 16SV data loads
 * little-endian (Cvt 1 only) -- original quirk kept. */
static int scan_iff(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents)
{
    sample_t *s;
    size_t si = 12;
    uint8_t fl;
    int sh;

    if (n < 12 || memcmp(d, "FORM", 4) != 0)
        return -1;
    if (!memcmp(d + 8, "16SV", 4))
        fl = 3;
    else if (!memcmp(d + 8, "8SVX", 4))
        fl = 1;
    else
        return -1;
    sh = (fl & 2) ? 1 : 0;

    s = ent_init(&ents[0], path, 17);
    s->Flags = fl;

    for (;;) {
        uint32_t adv;

        if (si + 8 > n)
            return -1;
        if (!memcmp(d + si, "NAME", 4)) {
            int l = b16be(d, n, si + 6);
            if (l > 25)
                l = 25;
            name_raw(s, d, n, si + 8, l);
        } else if (!memcmp(d + si, "VHDR", 4)) {
            uint32_t lb = b32be(d, n, si + 0x0C) >> sh;
            uint32_t t  = b32be(d, n, si + 0x10) >> sh;
            if (t)
                s->Flags |= 16;
            s->LoopBeg = lb;
            s->LoopEnd = lb + t;
            s->C5Speed = b16be(d, n, si + 0x14);
        } else if (!memcmp(d + si, "BODY", 4)) {
            s->GvL = 64;
            s->Vol = 64;
            s->Cvt = 1;                 /* signed */
            s->Length = b32be(d, n, si + 4) >> sh;
            s->OffsetInFile = (uint32_t)(si + 8);
            ents[0].FileSize = b32be(d, n, si + 4);
            return 1;
        }
        adv = (uint32_t)b16be(d, n, si + 6) + 8;
        si += adv;
    }
}

/* TXWaveSampleIdentification (IT_D_INF.INC 632; feature 013): Yamaha
 * TX16W wave, format code 13. Byte 16h & 7Fh must be 49h (== 49h
 * exactly also sets the loop flag); 17-bit attack/loop lengths at
 * 18h/1Bh; rate by byte 17h; data at offset 20h; Cvt = 11h
 * (signed | TX 12-bit packed). */
static int scan_txw(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents)
{
    static const uint8_t ident[16] =
        { 'L','M','8','9','5','3',0,0,0,0,0,0,0,0,0,0 };
    sample_t *s;
    uint32_t attack, looplen;
    uint8_t fb;

    if (n < 32 || memcmp(d, ident, 16) != 0)
        return -1;
    fb = b8(d, n, 0x16);
    if ((fb & 0x7F) != 0x49)
        return -1;

    s = ent_init(&ents[0], path, 13);
    memset(s->SampleName, 0, 26);
    memcpy(s->SampleName, s->DOSFileName, 12);
    s->Vol = 64;
    s->Flags = (uint8_t)(1 | 2 | (fb == 0x49 ? 16 : 0));
    s->Cvt = 0x11;
    attack  = b32(d, n, 0x18) & 0x1FFFF;
    looplen = b32(d, n, 0x1B) & 0x1FFFF;
    s->Length  = attack + looplen;
    s->LoopBeg = attack;
    s->LoopEnd = attack + looplen;
    s->C5Speed = (b8(d, n, 0x17) < 2) ? 33000
               : (b8(d, n, 0x17) == 2) ? 50000 : 16000;
    s->OffsetInFile = 0x20;
    ents[0].FileSize = (s->Length * 3 + 1) / 2;     /* packed bytes */
    return 1;
}

static int scan_wav(const uint8_t *d, size_t n, const char *path,
                    slibent_t *ents)
{
    sample_t *s;
    size_t bp;
    uint32_t dlen;
    uint8_t bits;
    int i, is16, st, found = 0;

    if (n < 16 || memcmp(d + 8, "WAVEfmt ", 8) != 0)
        return -1;
    if (b16(d, n, 18) != 0 || b16(d, n, 20) != 1)
        return -1;

    bp = 24 + (size_t)b16(d, n, 16);    /* first chunk's size field */
    for (i = 0; i < 3; i++) {
        if (b32(d, n, bp - 4) == 0x61746164) {  /* 'data' */
            found = 1;
            break;
        }
        bp += (size_t)b16(d, n, bp) + 8;
        if (bp >= n)                    /* JC / ran off the file */
            break;
    }
    if (!found)
        return -1;

    bits = b8(d, n, 34);
    if (bits != 8 && bits != 16)
        return -1;
    is16 = (bits == 16);
    st = (b16(d, n, 22) == 2);

    dlen = b32(d, n, bp);
    if (dlen > 4177910)
        dlen = 4177910;

    s = ent_init(&ents[0], path, (uint8_t)(is16 ? 7 : 5));
    /* name = the DOS filename, as the synthesis copies it */
    memset(s->SampleName, 0, 26);
    memcpy(s->SampleName, s->DOSFileName, 12);
    s->Vol = 64;
    s->Flags = (uint8_t)(1 | (is16 ? 2 : 0) | (st ? 4 : 0));
    s->Cvt = (uint8_t)((is16 ? 1 : 0) | (st ? 32 : 0));
    s->DfP = 0;
    s->Length = dlen >> (is16 ? 1 : 0) >> (st ? 1 : 0);
    s->C5Speed = b16(d, n, 24);
    s->OffsetInFile = (uint32_t)(bp + 4);
    ents[0].FileSize = dlen;
    return 1;
}

/* ---- format sniffing / dispatch --------------------------------- */

static int mod_sig_channels(const uint8_t *d, size_t n)
{
    uint8_t m[4];

    if (n < 1084)
        return 0;
    memcpy(m, d + 1080, 4);
    if (!memcmp(m, "M.K.", 4) || !memcmp(m, "M!K!", 4) ||
        !memcmp(m, "FLT4", 4))
        return 4;
    if (!memcmp(m, "OCTA", 4) || !memcmp(m, "CD81", 4))
        return 8;
    if (m[0] >= '1' && m[0] <= '9' && !memcmp(m + 1, "CHN", 3))
        return m[0] - '0';
    if (m[0] >= '1' && m[0] <= '9' && m[1] >= '0' && m[1] <= '9' &&
        !memcmp(m + 2, "CH", 2))
        return (m[0] - '0') * 10 + (m[1] - '0');
    return 0;
}

static int has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext);
    size_t i;

    if (nl < el)
        return 0;
    for (i = 0; i < el; i++) {
        char a = name[nl - el + i], b = ext[i];
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 32);
        if (a != b)
            return 0;
    }
    return 1;
}

int RIS_ScanModule(const char *path, slibent_t *ents, int max)
{
    size_t n;
    uint8_t *d = load_file(path, &n);
    int cnt = -1, ch;

    if (!d)
        return -1;

    if (n >= 4 && !memcmp(d, "IMPS", 4)) {   /* standalone .ITS */
        slibent_t *e = &ents[0];
        ent_init(e, path, 2);
        memcpy(&e->hdr, d, n < 0x50 ? n : 0x50);
        e->hdr.Data = NULL;
        e->FileSize = (e->hdr.Flags & 2) ? e->hdr.Length * 2
                                         : e->hdr.Length;
        transfer_filename(&e->hdr, path);
        cnt = 1;
    } else if (n >= 4 && !memcmp(d, "IMPM", 4)) {
        cnt = scan_it(d, n, path, ents, max);
    } else if (n >= 17 && !memcmp(d, "Extended Module: ", 17)) {
        cnt = scan_xm(d, n, path, ents, max);
    } else if (n >= 0x30 && !memcmp(d + 0x2C, "SCRM", 4)) {
        cnt = scan_s3m(d, n, path, ents, max);
    } else if (n >= 3 && !memcmp(d, "MTM", 3)) {
        cnt = scan_mtm(d, n, path, ents, max);
    } else if (n >= 2 && (!memcmp(d, "if", 2) || !memcmp(d, "JN", 2))) {
        cnt = scan_669(d, n, path, ents, max);
    } else if (n >= 48 && !memcmp(d + 44, "PTMF", 4)) {
        cnt = scan_ptm(d, n, path, ents, max);
    } else if (n >= 4 && !memcmp(d, "FAR\xFE", 4)) {
        cnt = scan_far(d, n, path, ents, max);
    } else if (n >= 22 && !memcmp(d, "GF1PATCH110\0ID#000002", 22)) {
        cnt = scan_pat(d, n, path, ents, max);
    } else if (n >= 16 && !memcmp(d + 8, "WAVEfmt ", 8)) {
        cnt = scan_wav(d, n, path, ents);
    } else if (n >= 12 && !memcmp(d, "FORM", 4)) {
        cnt = scan_iff(d, n, path, ents);
    } else if (n >= 16 && !memcmp(d, "LM8953", 6)) {
        cnt = scan_txw(d, n, path, ents);
    } else if (has_ext(path, ".KRZ")) {
        cnt = scan_krz(d, n, path, ents, max);
    } else if ((ch = mod_sig_channels(d, n)) != 0) {
        cnt = scan_mod(d, n, path, ch, ents, max);
    } else if (has_ext(path, ".MOD")) {      /* 15-instrument fallback */
        cnt = scan_mod(d, n, path, 4, ents, max);
    }

    free(d);
    return cnt;
}

int RIS_LoadSample(const slibent_t *e, sample_t *dst)
{
    size_t n;
    uint8_t *d;
    sample_t tmp = e->hdr;

    /* the original refuses entries without data (LoadSample6) */
    if (tmp.Length == 0 || !(tmp.Flags & 1))
        return 0;

    tmp.Data = NULL;
    d = load_file(e->SrcFile, &n);
    if (!d)
        return 0;
    if (!Load_SampleData(d, n, &tmp)) {
        free(d);
        return 0;
    }
    free(d);

    /* LoadSample tail: header (0x48 bytes + vibrato) into the slot,
     * sample pointer zeroed */
    free(dst->Data);
    tmp.OffsetInFile = 0;
    *dst = tmp;
    return 1;
}

/* SampleFormatNames (IT_DISK.ASM 474), indexed by the record's type
 * byte, transliterated including its quirk: type 28h (a MOD with an
 * "xxCH" signature) points at XMModule, so IT labels those files
 * "Fast Tracker 2 Module". The table's empty slots 18..1Fh give "". */
const char *RIS_FormatName(uint8_t fmt)
{
    switch (fmt) {
    case 0:  return "Unchecked";
    case 1:  return "Directory";
    case 2:  return "Impulse Tracker Sample";
    case 3:  return "Scream Tracker Sample";
    case 4:  return "Unknown sample format";
    case 5:  return "8 Bit WAV Format";
    case 6:  return "Fast Tracker 2 Sample";
    case 7:  return "16 Bit WAV Format";
    case 8:  return "Fast Tracker 2 Sample";
    case 9:  return "Poly Tracker Sample";
    case 10: return "Multi Tracker Sample";
    case 11: return "Composer 669 Sample";
    case 12: return "Farandole Sample";
    case 13: return "TX Wave Sample";
    case 14: return "MOD Sample";
    case 15: return "KRZ Sample";
    case 16: return "Gravis UltraSound Patch";
    case 17: return "AIFF Sample";
    case 0x20: return "Scream Tracker 3 Module";
    case 0x21: return "Impulse Tracker Module";
    case 0x22: return "Fast Tracker 2 Module";
    case 0x23: return "Poly Tracker Module";
    case 0x24: return "Multi Tracker Module";
    case 0x25: return "Composer 669 Module";
    case 0x26: return "Farandole Module";
    case 0x27: return "MOD Format";
    case 0x28: return "Fast Tracker 2 Module";     /* sic, see above */
    case 0x29: return "Kurzweil Synth File";
    case 0x2A: return "Gravis UltraSound Patch";
    default: return "";
    }
}

/* ---- feature 015: the Load Sample directory listing ---------------- */

/* DirectoryMsg / LibraryMsg (IT_DISK.ASM 413): char 154 is the dotted
 * fill that makes these rows read as "........Directory........" */
static void fill_marker(sample_t *s, int pad, const char *word)
{
    int i, n = 0;

    memset(s->SampleName, 0, sizeof(s->SampleName));
    for (i = 0; i < pad; i++) s->SampleName[n++] = (char)154;
    for (i = 0; word[i]; i++) s->SampleName[n++] = word[i];
    for (i = 0; i < pad; i++) s->SampleName[n++] = (char)154;
}

static void ls_name(slibent_t *e, const char *name)
{
    memset(e->hdr.DOSFileName, 0, sizeof(e->hdr.DOSFileName));
    memcpy(e->hdr.DOSFileName, name,
           strlen(name) < sizeof(e->hdr.DOSFileName)
               ? strlen(name) : sizeof(e->hdr.DOSFileName));
}

/* D_GetSampleInfo (IT_D_INF.INC 353) module signatures -> type byte */
static int ls_module_type(const uint8_t *d, size_t n, const char *path)
{
    if (n >= 4 && !memcmp(d, "IMPM", 4)) return 0x21;
    if (n >= 48 && !memcmp(d + 44, "PTMF", 4)) return 0x23;
    if (n >= 3 && !memcmp(d, "MTM", 3)) return 0x24;
    if (n >= 2 && (!memcmp(d, "if", 2) || !memcmp(d, "JN", 2)))
        return 0x25;
    if (n >= 4 && !memcmp(d, "FAR\xFE", 4)) return 0x26;
    if (n >= 1084) {
        const uint8_t *m = d + 1080;
        if (!memcmp(m, "M.K.", 4) || !memcmp(m, "M!K!", 4) ||
            !memcmp(m, "FLT4", 4) || !memcmp(m, "4CHN", 4) ||
            !memcmp(m, "6CHN", 4) || !memcmp(m, "8CHN", 4) ||
            !memcmp(m, "FLT8", 4))
            return 0x27;
        if (m[2] == 'C' && m[3] == 'H' && m[0] >= '0' && m[0] <= '9' &&
            m[1] >= '0' && m[1] <= '9')
            return 0x28;
    }
    if (n >= 0x30 && !memcmp(d + 0x2C, "SCRM", 4)) return 0x20;
    if (n >= 34 && !memcmp(d, "PRAM", 4) && d[32] == 0xFF && d[33] == 0xFF
        && has_ext(path, ".KRZ"))
        return 0x29;
    if (n >= 22 && !memcmp(d, "GF1PATCH110\0ID#000002", 22)) return 0x2A;
    if (n >= 17 && !memcmp(d, "Extended Module: ", 17)) return 0x22;
    return 0;
}

/* D_GetSampleInfo2 (IT_D_INF.INC 728): a file nothing recognises becomes
 * "Unknown sample format" -- raw 8-bit unsigned data (Cvt 0) from the
 * start of the file, up to 4177910 bytes, C-5 8363, volumes 64, no loop,
 * the filename as the sample name. So IT loads "any old thing" (issue
 * #17). An empty file keeps length 0 and is refused at load time. */
static void ls_unknown(slibent_t *e)
{
    sample_t *h = &e->hdr;
    char keep[12];

    memcpy(keep, h->DOSFileName, sizeof(keep));
    memset(h, 0, sizeof(*h));
    memcpy(h->DOSFileName, keep, sizeof(keep));
    memcpy(&h->ID, "IMPS", 4);
    h->GvL = 64;
    h->Flags = 1;
    h->Vol = 64;
    memcpy(h->SampleName, keep, sizeof(keep));
    h->Length = e->FileSize < 4177910 ? e->FileSize : 4177910;
    h->C5Speed = 8363;
}

/* identify one file into its record (D_LoadSampleHeader+GetSampleInfo) */
static void ls_identify(slibent_t *e)
{
    uint8_t d[1084];
    size_t n = 0;
    FILE *fp = fopen(e->SrcFile, "rb");
    int mt;

    e->Format = 4;                      /* unknown until proven otherwise */
    e->SortPri = 3;
    ls_unknown(e);
    if (!fp)
        return;
    n = fread(d, 1, sizeof(d), fp);
    fclose(fp);

    if ((mt = ls_module_type(d, n, e->SrcFile)) != 0) {
        char keep[12];
        memcpy(keep, e->hdr.DOSFileName, sizeof(keep));
        memset(&e->hdr, 0, sizeof(e->hdr));     /* no raw fallback */
        memcpy(e->hdr.DOSFileName, keep, sizeof(keep));
        e->Format = (uint8_t)mt;
        e->SortPri = 1;
        fill_marker(&e->hdr, 9, "Library");
        return;
    }
    if ((n >= 4 && (!memcmp(d, "IMPS", 4) || !memcmp(d, "RIFF", 4) ||
                    !memcmp(d, "FORM", 4))) ||
        (n >= 6 && !memcmp(d, "LM8953", 6))) {
        slibent_t one;
        if (RIS_ScanModule(e->SrcFile, &one, 1) == 1) {
            char keep[12];
            memcpy(keep, e->hdr.DOSFileName, sizeof(keep));
            e->hdr = one.hdr;
            memcpy(e->hdr.DOSFileName, keep, sizeof(keep));
            e->Format = one.Format;
            e->SortPri = 2;             /* D_GetSampleInfo default */
        }
    }
}

static uint16_t dos_date(int y, int mo, int d)
{
    if (y < 1980) y = 1980;
    return (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
}

static uint16_t dos_time(int h, int mi, int s)
{
    return (uint16_t)((h << 11) | (mi << 5) | (s / 2));
}

static int ls_cmp(const void *a, const void *b)
{
    const slibent_t *x = (const slibent_t *)a, *y = (const slibent_t *)b;
    int c;

    if (x->SortPri != y->SortPri)
        return x->SortPri < y->SortPri ? -1 : 1;
    c = memcmp(x->hdr.DOSFileName, y->hdr.DOSFileName,
               sizeof(x->hdr.DOSFileName));
    return c ? c : strcmp(x->SrcFile, y->SrcFile);  /* long-name ties */
}

static int ls_add(slibent_t *ents, int n, int max, const char *dir,
                  const char *name, int isdir, uint32_t size,
                  uint16_t date, uint16_t time)
{
    slibent_t *e;

    if (n >= max)
        return n;
    if (!strcmp(name, ".") && !isdir)
        return n;
    e = &ents[n];
    memset(e, 0, sizeof(*e));
    snprintf(e->SrcFile, sizeof(e->SrcFile), "%s/%s", dir, name);
    e->FileSize = size;
    e->Date = date;
    e->Time = time;
    if (isdir) {
        ls_name(e, !strcmp(name, ".") ? "\\" : name);
        fill_marker(&e->hdr, 8, "Directory");
        e->Format = 1;
        e->SortPri = 0;
    } else {
        ls_name(e, name);
        ls_identify(e);
    }
    return n + 1;
}

int RIS_ListDirectory(const char *dir, slibent_t *ents, int max)
{
    int n = 0, pass, pinned;

    /* D_LoadSampleFiles: all directories first, then all files */
    for (pass = 0; pass < 2; pass++) {
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        char pat[280];
        HANDLE h;

        snprintf(pat, sizeof(pat), "%s\\*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return pass ? n : -1;
        do {
            int isdir = (fd.dwFileAttributes &
                         FILE_ATTRIBUTE_DIRECTORY) != 0;
            FILETIME lt;
            WORD dd = 0, dt = 0;
            if (isdir != (pass == 0))
                continue;
            if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt))
                FileTimeToDosDateTime(&lt, &dd, &dt);
            n = ls_add(ents, n, max, dir, fd.cFileName, isdir,
                       (uint32_t)fd.nFileSizeLow, dd, dt);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR *dp = opendir(dir);
        struct dirent *de;

        if (!dp)
            return pass ? n : -1;
        while ((de = readdir(dp))) {
            char full[600];
            struct stat st;
            struct tm tmv, *t;
            int isdir;
            snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
            if (stat(full, &st))
                continue;
            isdir = S_ISDIR(st.st_mode);
            if (isdir != (pass == 0))
                continue;
            t = localtime_r(&st.st_mtime, &tmv);
            n = ls_add(ents, n, max, dir, de->d_name, isdir,
                       (uint32_t)st.st_size,
                       t ? dos_date(t->tm_year + 1900, t->tm_mon + 1,
                                    t->tm_mday) : 0,
                       t ? dos_time(t->tm_hour, t->tm_min, t->tm_sec) : 0);
        }
        closedir(dp);
#endif
    }

    /* D_SlowSampleSort: "\" then ".." stay on top, the rest by
     * priority then filename bytes */
    pinned = 0;
    {
        int i;
        for (i = 0; i < n && pinned < 2; i++) {
            const char *nm = ents[i].hdr.DOSFileName;
            if (!strcmp(nm, "\\") || !strcmp(nm, "..")) {
                slibent_t t = ents[pinned];
                ents[pinned] = ents[i];
                ents[i] = t;
                pinned++;
            }
        }
        if (pinned == 2 && !strcmp(ents[0].hdr.DOSFileName, "..")) {
            slibent_t t = ents[0];
            ents[0] = ents[1];
            ents[1] = t;
        }
    }
    qsort(ents + pinned, (size_t)(n - pinned), sizeof(*ents), ls_cmp);
    return n;
}

int RIS_KnownExt(const char *name)
{
    static const char *ext[] = {
        ".IT", ".S3M", ".XM", ".MOD", ".MTM", ".669",
        ".PTM", ".FAR", ".KRZ", ".PAT", ".ITS", ".WAV",
        ".IFF", ".8SV", ".16S", ".TXW", ".W01", NULL
    };
    int i;

    for (i = 0; ext[i]; i++)
        if (has_ext(name, ext[i]))
            return 1;
    return 0;
}

/* =================================================================
 * Instrument library (IT_D_RI.INC + LIWindow_Enter)
 * ================================================================= */

static void ilib_init(ilibent_t *e, const char *path, uint8_t fmt)
{
    memset(e, 0, sizeof(*e));
    e->Format = fmt;
    snprintf(e->SrcFile, sizeof(e->SrcFile), "%s", path);
}

static void ilib_name(ilibent_t *e, const uint8_t *d, size_t n,
                      size_t o, int len)
{
    int i;
    for (i = 0; i < len && i < 26; i++)
        e->Name[i] = (char)b8(d, n, o + (size_t)i);
    e->Name[i] = 0;
}

int RI_ScanModule(const char *path, ilibent_t *ents, int max)
{
    size_t n;
    uint8_t *d = load_file(path, &n);
    int cnt = -1;

    if (!d)
        return -1;

    if (n >= 4 && !memcmp(d, "IMPI", 4)) {           /* .ITI file */
        ilib_init(&ents[0], path, 3);
        ilib_name(&ents[0], d, n, 0x20, 25);
        ents[0].NumSamples = b16(d, n, 0x1E);
        cnt = 1;
    } else if (n >= 21 && !memcmp(d, "Extended Instrument: ", 21)) {
        ilib_init(&ents[0], path, 4);                /* .XI file */
        ilib_name(&ents[0], d, n, 21, 22);
        ents[0].NumSamples = b16(d, n, 296);
        cnt = 1;
    } else if (n >= 4 && !memcmp(d, "IMPM", 4)) {    /* .IT module */
        int insnum = b16(d, n, 0x22), i;
        size_t itab = 0xC0 + b16(d, n, 0x20);

        cnt = 0;
        for (i = 0; i < insnum && cnt < max; i++) {
            size_t off = b32(d, n, itab + (size_t)i * 4);
            uint8_t seen[100];
            int k, nos = 0;

            memset(seen, 0, sizeof(seen));
            for (k = 0; k < 120; k++) {
                uint8_t smp = b8(d, n, off + 0x41 + (size_t)k * 2);
                if (smp < 100)
                    seen[smp] = 1;
            }
            for (k = 99; k >= 1; k--)        /* index 0 never counted */
                if (seen[k])
                    nos++;
            if (!nos)
                continue;
            ilib_init(&ents[cnt], path, 5);
            ilib_name(&ents[cnt], d, n, off + 0x20, 25);
            ents[cnt].NumSamples = (uint16_t)nos;
            ents[cnt].Offset = (uint32_t)off;
            cnt++;
        }
    } else if (n >= 17 && !memcmp(d, "Extended Module: ", 17)) {
        int instruments = b16(d, n, 72), i;
        size_t pos = xm_first_instrument(d, n);

        cnt = 0;
        for (i = 0; i < instruments && cnt < max; i++) {
            size_t off = pos;
            uint32_t inssize = b32(d, n, pos);
            int nsmp = b16(d, n, pos + 27), k;
            uint32_t total = 0;

            if (inssize == 0)
                break;
            pos += inssize;
            if (nsmp == 0)
                continue;
            for (k = 0; k < nsmp; k++)
                total += b32(d, n, pos + (size_t)k * 40);
            pos += (size_t)nsmp * 40 + total;

            ilib_init(&ents[cnt], path, 6);
            ilib_name(&ents[cnt], d, n, off + 4, 22);
            ents[cnt].NumSamples = (uint16_t)nsmp;
            ents[cnt].Offset = (uint32_t)off;
            cnt++;
        }
    }

    free(d);
    return cnt;
}

int RI_UnusedSamples(void)
{
    int i, unused = 99;

    for (i = 0; i < 99; i++)
        if (Song.Smp[i].Flags & 1)
            unused--;
    return unused;
}

/* Music_ReleaseSample (IT_MUSIC.ASM 3110): free data, zero length +
 * pointer, clear the association bit, wipe bytes 14h..4Fh. */
static void ris_release_sample(sample_t *s)
{
    free(s->Data);
    s->Data = NULL;
    s->Length = 0;
    s->OffsetInFile = 0;
    s->Flags &= (uint8_t)~1;
    memset(s->SampleName, 0, 26);
    s->Cvt = 0;
    s->DfP = 0;
    s->LoopBeg = s->LoopEnd = 0;
    s->C5Speed = 0;
    s->SusLoopBeg = s->SusLoopEnd = 0;
    s->ViS = s->ViD = s->ViR = s->ViT = 0;
}

/* LoadXIChain (IT_D_RI.INC 67..): build an IT instrument + ITS sample
 * headers from XI-layout data. `p`/`pn` address the XI file from
 * offset 0; for XM instruments the caller passes the instrument start
 * minus 33 so the XI offsets line up (the original's 63033+4 trick).
 * `name_o` = 21 (XI) or 37 (XM). `shdr_o`/`data_o` are file offsets
 * of the sample headers and first sample's data. */
static void xi_chain(const uint8_t *p, size_t pbase, size_t pn,
                     const uint8_t *file, size_t fn,
                     size_t name_o, int nos,
                     size_t shdr_o, instrument_t *in, sample_t *sh)
{
    int i;
    uint32_t fadeout;
    uint32_t data_o = (uint32_t)(shdr_o + (size_t)nos * 40);

#define XP(k) b8(p, pn, pbase + (size_t)(k))
#define XPW(k) b16(p, pn, pbase + (size_t)(k))

    memset(in, 0, sizeof(*in));
    memcpy(&in->ID, "IMPI", 4);

    fadeout = (uint32_t)(XPW(272) + 15) >> 5;
    if (fadeout > 256)
        fadeout = 256;
    in->FadeOut = (uint16_t)fadeout;
    in->PPS = 0;
    in->PPC = 60;
    in->GbV = 128;
    in->DfP = 32;
    in->NoS = (uint8_t)nos;
    for (i = 0; i < 22; i++)
        in->InstrumentName[i] = (char)XP(name_o + i);
    /* note translation table: 12 empty, 96 mapped, 12 empty */
    for (i = 0; i < 12; i++) {
        in->NoteSampleTable[i * 2] = (uint8_t)i;
        in->NoteSampleTable[i * 2 + 1] = 0;
    }
    for (i = 0; i < 96; i++) {
        in->NoteSampleTable[(12 + i) * 2] = (uint8_t)(12 + i);
        in->NoteSampleTable[(12 + i) * 2 + 1] = (uint8_t)(XP(66 + i) + 1);
    }
    for (i = 0; i < 12; i++) {
        in->NoteSampleTable[(108 + i) * 2] = (uint8_t)(108 + i);
        in->NoteSampleTable[(108 + i) * 2 + 1] = 0;
    }

    /* volume envelope: XI flags 1=on 2=sustain 4=loop */
    {
        uint8_t f = XP(266);
        uint8_t num = XP(258);
        in->VEnvelope.Flags = (uint8_t)((f & 1) | ((f & 2) << 1) |
                                        ((f & 4) >> 1));
        in->VEnvelope.Num = (num < 12) ? num : 12;
        in->VEnvelope.LpB = XP(261);
        in->VEnvelope.LpE = XP(262);
        in->VEnvelope.SLB = XP(260);
        in->VEnvelope.SLE = XP(260);
        for (i = 0; i < 12; i++) {
            uint8_t y = XP(162 + i * 4 + 2);
            if (y > 64)
                y = 64;
            in->VEnvelope.NodePoints[i].Magnitude = (int8_t)y;
            in->VEnvelope.NodePoints[i].Tick = XPW(162 + i * 4);
        }
    }
    /* panning envelope (y - 32) */
    {
        uint8_t f = XP(267);
        uint8_t num = XP(259);
        in->PEnvelope.Flags = (uint8_t)((f & 1) | ((f & 2) << 1) |
                                        ((f & 4) >> 1));
        in->PEnvelope.Num = (num < 12) ? num : 12;
        in->PEnvelope.LpB = XP(264);
        in->PEnvelope.LpE = XP(265);
        in->PEnvelope.SLB = XP(263);
        in->PEnvelope.SLE = XP(263);
        for (i = 0; i < 12; i++) {
            uint8_t y = XP(210 + i * 4 + 2);
            if (y > 64)
                y = 64;
            in->PEnvelope.NodePoints[i].Magnitude = (int8_t)(y - 32);
            in->PEnvelope.NodePoints[i].Tick = XPW(210 + i * 4);
        }
    }
    /* pitch envelope: off, 2 nodes (0,0)-(0,99) */
    in->PtEnvelope.Flags = 0;
    in->PtEnvelope.Num = 2;
    in->PtEnvelope.NodePoints[0].Magnitude = 0;
    in->PtEnvelope.NodePoints[0].Tick = 0;
    in->PtEnvelope.NodePoints[1].Magnitude = 0;
    in->PtEnvelope.NodePoints[1].Tick = 99;

    /* minimum-envelope fixups */
    if (in->VEnvelope.Num < 2) {
        in->VEnvelope.Num = 2;
        in->VEnvelope.NodePoints[0].Magnitude = 64;
        in->VEnvelope.NodePoints[0].Tick = 0;
        in->VEnvelope.NodePoints[1].Magnitude = 64;
        in->VEnvelope.NodePoints[1].Tick = 100;
    }
    if (in->PEnvelope.Num < 2) {
        in->PEnvelope.Num = 2;
        in->PEnvelope.NodePoints[0].Magnitude = 0;
        in->PEnvelope.NodePoints[0].Tick = 0;
        in->PEnvelope.NodePoints[1].Magnitude = 0;
        in->PEnvelope.NodePoints[1].Tick = 100;
    }

    /* sample headers (LoadXISample1..): XM layout -> ITS */
    {
        uint32_t run = data_o;
        for (i = 0; i < nos; i++) {
            size_t so = shdr_o + (size_t)i * 40;
            sample_t *s = &sh[i];

            memset(s, 0, sizeof(*s));
            memcpy(&s->ID, "IMPS", 4);
            s->GvL = 64;
            xm_sample_fields(s, file, fn, so);
            if (b32(file, fn, so) == 0)
                s->Flags = 0;                /* LoadXISample2: no bits */
            s->OffsetInFile = run;
            run += b32(file, fn, so);
        }
    }
#undef XP
#undef XPW
}

int RI_LoadInstrument(const ilibent_t *e, int target)
{
    size_t n;
    uint8_t *d = load_file(e->SrcFile, &n);
    instrument_t ins;
    sample_t sh[100];
    uint8_t map[256];
    int nos = 0, i, k, slot;

    if (!d)
        return RI_ERR_OPEN;

    /* parse the instrument + its sample headers (offsets in-file) */
    switch (e->Format) {
    case 3:                                          /* .ITI file */
        if (b16(d, n, 0x1C) < 0x200 && n >= 554)
            Load_OldInstrument(d, &ins);
        else if (n >= 554)
            memcpy(&ins, d, 554);
        else
            memset(&ins, 0, sizeof(ins));
        nos = ins.NoS;
        if (nos > 100)
            nos = 100;
        for (i = 0; i < nos; i++) {
            size_t off = 554 + (size_t)i * 80;
            memset(&sh[i], 0, sizeof(sh[i]));
            if (off + 0x50 <= n)
                memcpy(&sh[i], d + off, 0x50);
            sh[i].Data = NULL;
        }
        break;

    case 4:                                          /* .XI file */
        nos = b16(d, n, 296);
        if (nos > 100)
            nos = 100;
        xi_chain(d, 0, n, d, n, 21, nos, 298, &ins, sh);
        break;

    case 5: {                                        /* in .IT module */
        int smpnum = b16(d, n, 0x24);
        size_t stab = 0xC0 + b16(d, n, 0x20) +
                      (size_t)b16(d, n, 0x22) * 4;
        uint8_t itab[256];

        if (b16(d, n, 0x2A) < 0x200 &&
            e->Offset + 554 <= n) {                  /* module Cmwt */
            Load_OldInstrument(d + e->Offset, &ins);
        } else if (e->Offset + 554 <= n) {
            memcpy(&ins, d + e->Offset, 554);
        } else {
            memset(&ins, 0, sizeof(ins));
        }

        memset(itab, 0, sizeof(itab));
        for (k = 0; k < 120; k++) {
            int smp = ins.NoteSampleTable[k * 2 + 1];
            size_t soff;

            if (smp == 0 || smp > smpnum || smp >= 100 || itab[smp])
                continue;
            itab[smp] = (uint8_t)(++nos);
            /* module sample-offset table entry (1-based -> -4) */
            soff = b32(d, n, stab + (size_t)(smp - 1) * 4);
            memset(&sh[nos - 1], 0, sizeof(sh[0]));
            if (soff + 0x50 <= n)
                memcpy(&sh[nos - 1], d + soff, 0x50);
            sh[nos - 1].Data = NULL;
        }
        for (k = 0; k < 120; k++)                    /* rewrite table */
            ins.NoteSampleTable[k * 2 + 1] =
                itab[ins.NoteSampleTable[k * 2 + 1]];
        ins.NoS = (uint8_t)nos;
        break; }

    case 6: {                                        /* in .XM module */
        uint32_t inssize = b32(d, n, e->Offset);

        if (e->Offset < 33) {
            free(d);
            return RI_ERR_OPEN;
        }
        nos = b16(d, n, e->Offset + 27);
        if (nos > 100)
            nos = 100;
        xi_chain(d, (size_t)e->Offset - 33, n, d, n,
                 37, nos, e->Offset + inssize, &ins, sh);
        break; }

    default:
        free(d);
        return RI_ERR_OPEN;
    }

    /* release samples used only by the target instrument */
    {
        uint8_t used[256];
        memset(used, 0, sizeof(used));
        for (k = 0; k < 120; k++)
            used[Song.Ins[target].NoteSampleTable[k * 2 + 1]] = 1;
        for (i = 0; i < 99; i++) {
            if (i == target)
                continue;
            for (k = 0; k < 120; k++)
                used[Song.Ins[i].NoteSampleTable[k * 2 + 1]] = 0;
        }
        for (i = 1; i <= 98; i++)
            if (used[i])
                ris_release_sample(&Song.Smp[i - 1]);
    }

    /* allocate free slots (1-based ascending scan) + transfer */
    memset(map, 0, sizeof(map));
    slot = 0;
    for (i = 0; i < nos; i++) {
        do {
            slot++;
        } while (slot <= 99 && (Song.Smp[slot - 1].Flags & 1));
        if (slot > 99) {
            free(d);
            return RI_ERR_SLOTS;             /* guarded by caller */
        }
        map[i + 1] = (uint8_t)slot;
    }

    for (i = 0; i < nos; i++) {
        sample_t *dst = &Song.Smp[map[i + 1] - 1];
        sample_t tmp = sh[i];

        tmp.Data = NULL;
        if ((tmp.Flags & 1) && tmp.Length != 0) {
            if (!Load_SampleData(d, n, &tmp))
                tmp.Length = 0;              /* out of memory: silence */
        }
        free(dst->Data);
        *dst = tmp;
        dst->OffsetInFile = 0;
        dst->Cvt = 1;                        /* word 2Eh = 1: */
        dst->DfP = 0;                        /*   Cvt=1, DfP=0 quirk  */
    }

    /* instrument header + remapped note table into the target */
    {
        instrument_t *di = &Song.Ins[target];
        *di = ins;
        for (k = 0; k < 120; k++)
            di->NoteSampleTable[k * 2 + 1] =
                map[ins.NoteSampleTable[k * 2 + 1]];
    }

    free(d);
    return RI_OK;
}

const char *RI_FormatName(uint8_t fmt)
{
    switch (fmt) {
    case 3:  return "Impulse Tracker Instrument";
    case 4:  return "Fast Tracker 2 Instrument";
    case 5:  return "Impulse Tracker Module";
    case 6:  return "Fast Tracker 2 Module";
    default: return "Unknown format";
    }
}

int RI_KnownExt(const char *name)
{
    return has_ext(name, ".ITI") || has_ext(name, ".XI") ||
           has_ext(name, ".IT") || has_ext(name, ".XM");
}

/* =================================================================
 * Disk saves (D_SaveSample / D_SaveST3Sample / D_SaveRawSample /
 * D_SaveInstrument) -- the Alt-O/T/W ops deferred from feature 005.
 * In-memory data is already signed with Cvt=1, so ITS/16-bit WAV
 * write it verbatim; ST3 and 8-bit WAV convert to unsigned.
 * ================================================================= */

static size_t smp_bytes(const sample_t *s)
{
    return (size_t)s->Length << ((s->Flags & 2) ? 1 : 0);
}

static int write_data_raw(FILE *f, const sample_t *s)
{
    if (!(s->Flags & 1) || !s->Data || s->Length == 0)
        return 1;
    return fwrite(s->Data, 1, smp_bytes(s), f) == smp_bytes(s);
}

static int write_data_unsigned(FILE *f, const sample_t *s)
{
    size_t i, nb = smp_bytes(s);
    const uint8_t *p = (const uint8_t *)s->Data;
    int ok = 1;

    if (!(s->Flags & 1) || !p || s->Length == 0)
        return 1;
    if (s->Flags & 2) {
        for (i = 0; i < s->Length && ok; i++) {
            uint16_t v = (uint16_t)(((const int16_t *)s->Data)[i] ^ 0x8000);
            ok = fwrite(&v, 1, 2, f) == 2;
        }
    } else {
        for (i = 0; i < nb && ok; i++) {
            uint8_t v = (uint8_t)(p[i] ^ 0x80);
            ok = fwrite(&v, 1, 1, f) == 1;
        }
    }
    return ok;
}

int RIS_SaveITS(const sample_t *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    sample_t hdr = *s;
    int ok;

    if (!f)
        return 0;
    hdr.Cvt = 1;
    hdr.Flags &= (uint8_t)~0x0C;
    hdr.OffsetInFile = 80;
    ok = fwrite(&hdr, 1, 80, f) == 80 && write_data_raw(f, s);
    fclose(f);
    return ok;
}

int RIS_SaveST(const sample_t *s, const char *path)
{
    uint8_t h[80];
    FILE *f;
    int ok;

    memset(h, 0, sizeof(h));
    h[0] = 1;                                /* type: sample */
    memcpy(h + 1, s->DOSFileName, 12);       /* 13 bytes incl. Zero */
    h[14] = 5;                               /* memseg word = 5 */
    memcpy(h + 16, &s->Length, 4);
    memcpy(h + 20, &s->LoopBeg, 4);
    memcpy(h + 24, &s->LoopEnd, 4);
    h[28] = s->Vol;
    h[31] = (uint8_t)(((s->Flags & 0x10) >> 4) | ((s->Flags & 2) << 1));
    memcpy(h + 32, &s->C5Speed, 4);
    memcpy(h + 48, s->SampleName, 25);
    memcpy(h + 76, "SCRS", 4);

    f = fopen(path, "wb");
    if (!f)
        return 0;
    ok = fwrite(h, 1, 80, f) == 80 && write_data_unsigned(f, s);
    fclose(f);
    return ok;
}

int RIS_SaveWAV(const sample_t *s, const char *path)
{
    uint8_t h[44];
    uint32_t rate = s->C5Speed, dsz = (uint32_t)smp_bytes(s);
    uint32_t bps = (s->Flags & 2) ? rate * 2 : rate;
    uint16_t align = (s->Flags & 2) ? 2 : 1;
    uint16_t bits = (s->Flags & 2) ? 16 : 8;
    FILE *f;
    int ok;

    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4);
    /* RIFF size never filled in by the original -- kept 0 */
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;
    h[20] = 1;                               /* PCM */
    h[22] = 1;                               /* mono */
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &bps, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &dsz, 4);

    f = fopen(path, "wb");
    if (!f)
        return 0;
    ok = fwrite(h, 1, 44, f) == 44;
    if (ok) {
        if (s->Flags & 2)
            ok = write_data_raw(f, s);       /* 16-bit: signed */
        else
            ok = write_data_unsigned(f, s);  /*  8-bit: unsigned */
    }
    fclose(f);
    return ok;
}

int RI_SaveITI(const instrument_t *in, int insnum1, const char *path)
{
    uint8_t table[100];
    instrument_t hdr = *in;
    int i, k, nos = 0;
    uint32_t ptr;
    FILE *f;
    int ok = 1;

    (void)insnum1;

    /* collect + compact-renumber the samples the note table uses */
    memset(table, 0, sizeof(table));
    for (k = 0; k < 120; k++) {
        uint8_t smp = in->NoteSampleTable[k * 2 + 1];
        if (smp < 100)
            table[smp] = 1;
    }
    table[0] = 0;
    for (i = 1; i <= 99; i++)
        if (table[i])
            table[i] = (uint8_t)++nos;

    hdr.NoS = (uint8_t)nos;
    hdr.TrkVers = 0x0217;
    for (k = 0; k < 120; k++)
        hdr.NoteSampleTable[k * 2 + 1] =
            table[in->NoteSampleTable[k * 2 + 1]];

    f = fopen(path, "wb");
    if (!f)
        return 0;
    ok = fwrite(&hdr, 1, 554, f) == 554;

    /* sample headers with chained data pointers */
    ptr = 554 + (uint32_t)nos * 80;
    for (i = 1; i <= 99 && ok; i++) {
        sample_t sh;
        if (!table[i])
            continue;
        sh = Song.Smp[i - 1];
        sh.Data = NULL;
        sh.Cvt = 1;                      /* memory data is flat signed */
        sh.Flags &= (uint8_t)~0x0C;
        sh.OffsetInFile = ptr;
        if (sh.Flags & 1)
            ptr += (uint32_t)smp_bytes(&Song.Smp[i - 1]);
        ok = fwrite(&sh, 1, 80, f) == 80;
    }
    for (i = 1; i <= 99 && ok; i++) {
        if (!table[i])
            continue;
        ok = write_data_raw(f, &Song.Smp[i - 1]);
    }
    fclose(f);
    return ok;
}
