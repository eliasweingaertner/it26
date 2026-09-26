/*
 * it_import.c
 * -----------
 * Whole-module importers, transliterated from the original editor's
 * IT_D_RM.INC (D_LoadS3M / D_LoadXM / D_LoadMOD / D_LoadMTM /
 * D_Load669) and the pattern converters in PE_TRANS.INC
 * (PE_TranslateS3MPattern, TranslateMODCommand,
 * PE_TranslateMODPattern, PE_TranslateMTMPattern,
 * PE_Translate669Pattern, PE_TranslateXMPattern + the XM note/
 * instrument/volume/effect translators).
 *
 * Patterns are built in an editcell grid and packed with
 * Pattern_Pack (the exact codec, standing in for the original's PE
 * buffer + PEFunction_StorePattern). Authentic quirks are kept and
 * marked: the MOD 127-entry order scan, the XM SmpNum off-by-one,
 * the S3M Dxy dead code, XM note 0 becoming B-0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "it_music.h"
#include "it_pattern.h"
#include "it_save.h"
#include "it_import.h"

int  Music_LoadIT(const char *path);
void Music_FreeIT(void);

/* ---- byte-exact tables from IT_DISK.ASM / IT_PE.ASM / PE_TRANS ---- */

static const uint16_t FineTuneTable[16] = {
    8363, 8413, 8463, 8529, 8581, 8651, 8723, 8757,
    7895, 7941, 7985, 8046, 8107, 8169, 8232, 8280,
};

static const uint16_t MODPeriodTable[72] = {
    1712, 1616, 1525, 1440, 1357, 1281, 1209, 1141, 1077, 1017, 961, 907,
     856,  808,  762,  720,  678,  640,  604,  570,  538,  508, 480, 453,
     428,  404,  381,  360,  339,  320,  302,  285,  269,  254, 240, 226,
     214,  202,  190,  180,  170,  160,  151,  143,  135,  127, 120, 113,
     107,  101,   95,   90,   85,   80,   75,   71,   67,   63,  60,  56,
      53,   50,   47,   45,   42,   40,   37,   35,   33,   31,  30,  28,
};

static const uint8_t PanningPositions[16] = {
    0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64,
};

static const uint8_t XMEffectG[10] = {
    193, 193+4, 193+5, 193+6, 193+6, 193+7, 193+7, 193+8, 193+8, 193+9,
};

/* ---- little/big-endian reader over the whole file ------------------ */

typedef struct rd_t {
    const uint8_t *d;
    size_t size, pos;
} rd_t;

static IT_MAYBE_UNUSED uint32_t rdavail(const rd_t *r)
{
    return (uint32_t)(r->pos < r->size ? r->size - r->pos : 0);
}

static uint8_t rd8(rd_t *r)
{
    return r->pos < r->size ? r->d[r->pos++] : 0;
}

static uint16_t rd16(rd_t *r)
{
    uint16_t v = rd8(r);
    return (uint16_t)(v | (rd8(r) << 8));
}

static IT_MAYBE_UNUSED uint16_t rd16be(rd_t *r)
{
    uint16_t v = rd8(r);
    return (uint16_t)((v << 8) | rd8(r));
}

static uint32_t rd32(rd_t *r)
{
    uint32_t v = rd16(r);
    return v | ((uint32_t)rd16(r) << 16);
}

static void rdseek(rd_t *r, size_t pos)
{
    r->pos = pos <= r->size ? pos : r->size;
}

static void rdbytes(rd_t *r, void *dst, size_t n)
{
    size_t have = r->pos < r->size ? r->size - r->pos : 0;
    if (have > n)
        have = n;
    memcpy(dst, r->d + r->pos, have);
    if (have < n)
        memset((uint8_t *)dst + have, 0, n - have);
    r->pos += have;
}

/* ---- common song setup (D_PreLoadModule) --------------------------- */

static void import_clear(void)
{
    Music_FreeIT();                     /* frees + zeroes Song */
    memset(IT_MessageData, 0, IT_MESSAGELENGTH);
    SetDefaultMIDIDataArea();
    Save_LoadTime = time(NULL);
    Song.Header.ID = 0x4D504D49u;       /* IMPM */
    memset(Song.Orders, 0xFF, sizeof(Song.Orders));
}

/* ---- sample data reader (D_LoadSampleData, uncompressed paths) -----
 * Reads Length samples at the current position, converting per the
 * header's Cvt: bit 0 clear = unsigned -> signed, bit 2 = delta
 * encoded (XM). Afterwards Cvt = 1 and the compressed flag is clear,
 * as the original rewrites them. */
static int import_sample_data(rd_t *r, sample_t *s)
{
    uint32_t len = s->Length;
    int is16 = (s->Flags & 2) != 0;
    uint8_t *data;
    uint32_t i;

    if (!(s->Flags & 1) || len == 0)
        return 1;
    if (len > 0x0FFFFFFF)
        return 0;
    data = (uint8_t *)calloc((size_t)len + 4, is16 ? 2 : 1);
    if (!data)
        return 0;
    s->Data = data;
    rdbytes(r, data, (size_t)len << (is16 ? 1 : 0));

    if (s->Cvt & 4) {                   /* XM delta */
        if (is16) {
            int16_t *p = (int16_t *)data;
            int16_t acc = 0;
            for (i = 0; i < len; i++) {
                acc = (int16_t)(acc + p[i]);
                p[i] = acc;
            }
        } else {
            int8_t *p = (int8_t *)data;
            int8_t acc = 0;
            for (i = 0; i < len; i++) {
                acc = (int8_t)(acc + p[i]);
                p[i] = acc;
            }
        }
    }
    if (!(s->Cvt & 1)) {                /* unsigned -> signed */
        if (is16) {
            uint16_t *p = (uint16_t *)data;
            for (i = 0; i < len; i++)
                p[i] ^= 0x8000;
        } else {
            for (i = 0; i < len; i++)
                data[i] ^= 0x80;
        }
    }
    s->Cvt = 1;
    s->Flags &= (uint8_t)~0x0C;
    return 1;
}

/* ---- pattern grid --------------------------------------------------- */

static editcell_t IGrid[MAX_PATROWS * 64];

static void grid_clear(void)
{
    memset(IGrid, 0, sizeof(IGrid));
}

static editcell_t *gcell(int row, int ch)
{
    return &IGrid[row * 64 + ch];
}

/* raw engine note (0..119 / 254 cut / 255 off) -> grid encoding */
static void put_note(editcell_t *e, int raw)
{
    e->mask |= CM_NOTE;
    e->note = (uint8_t)(raw <= 119 ? raw + 1 : raw);
}

static void put_ins(editcell_t *e, int ins)
{
    if (ins > 0) {
        e->mask |= CM_INS;
        e->ins = (uint8_t)ins;
    }
}

static void put_vol(editcell_t *e, int vol)
{
    if (vol != 0xFF) {
        e->mask |= CM_VOL;
        e->vol = (uint8_t)vol;
    }
}

static void put_cmd(editcell_t *e, int cmd, int val)
{
    if (cmd != 0) {
        e->mask |= CM_CMD;
        e->cmd = (uint8_t)cmd;
        e->cmdval = (uint8_t)val;
    }
}

#define CMD(c) ((c) - '@')

/* ---- TranslateMODCommand (PE_TRANS.INC 873) ------------------------ */

static void mod_command(editcell_t *e, uint8_t fx, uint8_t p)
{
    switch (fx) {
    case 0:
        if (p)
            put_cmd(e, CMD('J'), p);
        break;
    case 1: if (p) put_cmd(e, CMD('F'), p); break;
    case 2: if (p) put_cmd(e, CMD('E'), p); break;
    case 3: put_cmd(e, CMD('G'), p); break;
    case 4: put_cmd(e, CMD('H'), p); break;
    case 5: put_cmd(e, p ? CMD('L') : CMD('G'), p); break;
    case 6: put_cmd(e, p ? CMD('K') : CMD('H'), p); break;
    case 7: put_cmd(e, CMD('R'), p); break;
    case 8:
        if (p == 0xA4)
            put_cmd(e, CMD('S'), 0x91);
        else
            put_cmd(e, CMD('X'), p);
        break;
    case 9: put_cmd(e, CMD('O'), p); break;
    case 10:                            /* both nibbles: keep the high */
        if ((p & 0x0F) && (p & 0xF0))
            p &= 0xF0;
        if (p)
            put_cmd(e, CMD('D'), p);
        break;
    case 11: put_cmd(e, CMD('B'), p); break;
    case 12:                            /* volume column */
        put_vol(e, p > 64 ? 64 : p);
        break;
    case 13:                            /* BCD row -> decimal */
        put_cmd(e, CMD('C'), (p & 0x0F) + (p >> 4) * 10);
        break;
    case 14: {                          /* E-sub commands */
        uint8_t sub = (uint8_t)(p & 0xF0), x = (uint8_t)(p & 0x0F);
        switch (sub) {
        case 0x00: if (x) put_cmd(e, CMD('S'), x); break;
        case 0x10: if (x) put_cmd(e, CMD('F'), x | 0xF0); break;
        case 0x20: if (x) put_cmd(e, CMD('E'), x | 0xF0); break;
        case 0x30: put_cmd(e, CMD('S'), x | 0x10); break;
        case 0x40: put_cmd(e, CMD('S'), x | 0x30); break;
        case 0x50: put_cmd(e, CMD('S'), x | 0x20); break;
        case 0x60: put_cmd(e, CMD('S'), x | 0xB0); break;
        case 0x70: put_cmd(e, CMD('S'), x | 0x40); break;
        case 0x80: put_cmd(e, CMD('S'), x | 0x80); break;
        case 0x90: if (x) put_cmd(e, CMD('Q'), x); break;
        case 0xA0: if (x) put_cmd(e, CMD('D'), (x << 4) | 0x0F); break;
        case 0xB0: if (x) put_cmd(e, CMD('D'), 0xF0 | x); break;
        case 0xC0: put_cmd(e, CMD('S'), x | 0xC0); break;
        case 0xD0: put_cmd(e, CMD('S'), x | 0xD0); break;
        case 0xE0: put_cmd(e, CMD('S'), x | 0xE0); break;
        default:   put_cmd(e, CMD('S'), x | 0xF0); break;
        }
        break;
    }
    default:                            /* 15: set speed / tempo */
        put_cmd(e, p <= 0x20 ? CMD('A') : CMD('T'), p);
        break;
    }
}

/* ================= S3M (D_LoadS3M, research R2) ===================== */

static int load_s3m(rd_t *r)
{
    uint8_t hdr[0x60];
    uint16_t ordnum, insnum, patnum;
    uint16_t inspara[100], patpara[200];
    int i, c;

    rdbytes(r, hdr, 0x60);
    if (memcmp(hdr + 0x2C, "SCRM", 4) != 0)
        return 0;

    import_clear();
    memcpy(Song.Header.SongName, hdr, 25);

    Song.Header.Flags = 0x10;           /* old effects */
    if (hdr[0x33] & 0x80)
        Song.Header.Flags |= 1;         /* stereo */
    if (hdr[0x26] & 8)
        Song.Header.Flags |= 2;         /* vol0 optimisations */
    Song.Header.Special = 0;
    Song.Header.GV = (uint8_t)(hdr[0x30] << 1);
    Song.Header.MV = (uint8_t)(hdr[0x33] & 127);
    Song.Header.IS = hdr[0x31];
    Song.Header.IT = hdr[0x32];
    Song.Header.Sep = 128;
    {
        uint32_t e;
        memcpy(&e, hdr + 0x38, 4);
        if ((hdr[0x28] | (hdr[0x29] << 8)) >= 0x3208) {
            e ^= 0x4954524Bu;           /* 'ITRK' */
            e = (e >> 7) | (e << 25);   /* ROR 7 */
            e = 0u - e;
            e = (e << 4) | (e >> 28);   /* ROL 4 */
            e ^= 0x4A54484Cu;           /* 'JTHL' */
        }
        Song.Header.Reserved = e;
    }
    for (c = 0; c < 32; c++) {          /* channel settings @40h */
        uint8_t v = hdr[0x40 + c], pan;
        if (v >= 128)
            pan = 32 + 128;
        else {
            uint8_t low = (uint8_t)(v & 127);
            pan = (low <= 7) ? 0 : (low <= 15) ? 64 : 32;
            pan |= (uint8_t)(v & 0x80);
        }
        Song.Header.ChnlPan[c] = pan;
    }
    for (c = 32; c < 64; c++)
        Song.Header.ChnlPan[c] = 32 + 128;
    memset(Song.Header.ChnlVol, 64, 64);

    ordnum = hdr[0x20] | (hdr[0x21] << 8);
    insnum = hdr[0x22] | (hdr[0x23] << 8);
    patnum = hdr[0x24] | (hdr[0x25] << 8);
    if (insnum > 99) insnum = 99;
    if (patnum > 200) patnum = 200;
    Song.Header.OrdNum = ordnum;
    Song.Header.InsNum = 0;
    Song.Header.SmpNum = insnum;
    Song.Header.PatNum = patnum;

    rdbytes(r, Song.Orders, ordnum < 256 ? ordnum : 256);

    for (i = 0; i < insnum; i++)
        inspara[i] = rd16(r);
    for (i = 0; i < patnum; i++)
        patpara[i] = rd16(r);

    if (hdr[0x35] == 252) {             /* default pan block */
        uint8_t pb[32];
        rdbytes(r, pb, 32);
        for (c = 0; c < 32; c++)
            if (pb[c] & 0x20)
                Song.Header.ChnlPan[c] =
                    (uint8_t)((Song.Header.ChnlPan[c] & 0x80)
                              | (((pb[c] & 0x0F) << 2) + 2));
    }

    /* sample headers */
    for (i = 0; i < insnum; i++) {
        uint8_t sh[80];
        sample_t *s = &Song.Smp[i];

        rdseek(r, (size_t)inspara[i] << 4);
        rdbytes(r, sh, 80);
        s->ID = 0x53504D49u;            /* IMPS */
        memcpy(s->DOSFileName, sh + 1, 12);
        s->GvL = 64;
        s->Flags = 0;
        if (sh[0] == 1) {               /* type 1 = sample */
            s->Flags = (uint8_t)((sh[0x1F] >> 1) & 2);  /* 16 bit */
            if (sh[0x10] | sh[0x11])    /* length low word != 0 */
                s->Flags |= 1;
        }
        s->Flags |= (uint8_t)((sh[0x1F] & 1) << 4);     /* loop */
        s->Vol = sh[0x1C];
        memcpy(s->SampleName, sh + 0x30, 25);
        s->Cvt = 0;                     /* unsigned */
        s->DfP = 32;
        memcpy(&s->Length,  sh + 0x10, 4);
        memcpy(&s->LoopBeg, sh + 0x14, 4);
        memcpy(&s->LoopEnd, sh + 0x18, 4);
        memcpy(&s->C5Speed, sh + 0x20, 4);
        if (sh[0] == 1)                 /* data parapointer */
            s->OffsetInFile =
                ((uint32_t)sh[0x0D] << 20) |
                ((uint32_t)(sh[0x0E] | (sh[0x0F] << 8)) << 4);
        else
            s->OffsetInFile = 0;
    }
    for (i = 0; i < insnum; i++) {      /* sample data */
        sample_t *s = &Song.Smp[i];
        if (!(s->Flags & 1))
            continue;
        rdseek(r, s->OffsetInFile);
        s->OffsetInFile = 0;
        import_sample_data(r, s);
    }

    /* patterns (PE_TranslateS3MPattern) */
    for (i = 0; i < patnum; i++) {
        int row = 0;
        if (patpara[i] == 0)
            continue;
        rdseek(r, ((size_t)patpara[i] << 4) + 2);   /* skip length word */
        grid_clear();
        while (row < 64) {
            uint8_t b = rd8(r);
            editcell_t *e;
            if (b == 0) {
                row++;
                continue;
            }
            e = gcell(row, b & 31);
            if (b & 32) {
                uint8_t n = rd8(r), ins;
                if (n == 0xFE)
                    put_note(e, 254);
                else if (n <= 0x7F)
                    put_note(e, (n >> 4) * 12 + (n & 0x0F) + 12);
                ins = rd8(r);
                if (ins > 99)
                    ins = 0;
                put_ins(e, ins);
            }
            if (b & 64) {
                uint8_t v = rd8(r);
                if (v != 0xFF && v > 64)
                    v = 64;
                put_vol(e, v);
            }
            if (b & 128) {
                uint8_t cm = rd8(r), p = rd8(r);
                switch (cm) {
                case CMD('C'):          /* BCD break */
                    p = (uint8_t)((p & 0x0F) + (p >> 4) * 10);
                    break;
                case CMD('V'):          /* global volume << 1 */
                    p = (p & 0x80) ? 0xFF : (uint8_t)(p << 1);
                    break;
                case CMD('X'):
                    if (p == 0xA4) {
                        cm = CMD('S');
                        p = 0x91;
                    } else
                        p = (p & 0x80) ? 0xFF : (uint8_t)(p << 1);
                    break;
                case CMD('D'):
                    /* the original intends to zero the high nibble when
                     * both are set, but tests AL (the command) instead
                     * of AH -- dead code, kept as a passthrough */
                    break;
                default:
                    break;
                }
                put_cmd(e, cm, p);
            }
        }
        Pattern_Pack((uint16_t)i, IGrid, 64);
    }
    return 1;
}

/* ================= MOD (D_LoadMOD, research R3) ===================== */

static int load_mod(rd_t *r, int channels, int instruments)
{
    size_t orderoff  = instruments == 31 ? 952 : 472;
    size_t countoff  = instruments == 31 ? 950 : 470;
    size_t patoff    = instruments == 31 ? 1084 : 600;
    int npat = 0, i, c, p;

    import_clear();
    memcpy(Song.Header.SongName, r->d, r->size < 20 ? r->size : 20);

    Song.Header.Flags = 0x31;           /* stereo, old fx, link G */
    Song.Header.GV = 128;
    Song.Header.MV = 48;
    Song.Header.IS = 6;
    Song.Header.IT = 125;
    Song.Header.Sep = 64;

    if (channels > 8) {
        for (c = 0; c < channels && c < 64; c++)
            Song.Header.ChnlPan[c] = 32;
        for (; c < 64; c++)
            Song.Header.ChnlPan[c] = 32 + 128;
    } else {
        static const uint8_t lr[8] = { 0, 64, 64, 0, 0, 64, 64, 0 };
        for (c = 0; c < channels; c++)
            Song.Header.ChnlPan[c] = lr[c & 7];
        for (; c < 64; c++)
            Song.Header.ChnlPan[c] = 32 + 128;
    }
    memset(Song.Header.ChnlVol, 64, 64);

    {
        uint8_t nord = r->d[countoff];
        memcpy(Song.Orders, r->d + orderoff, nord < 128 ? nord : 128);
        Song.Header.OrdNum = nord;
        /* max pattern: the original scans only 127 entries (quirk) */
        for (i = 0; i < 127; i++)
            if (r->d[orderoff + i] > npat)
                npat = r->d[orderoff + i];
        npat++;
    }
    Song.Header.InsNum = 0;
    Song.Header.SmpNum = (uint16_t)instruments;
    Song.Header.PatNum = (uint16_t)npat;

    /* sample headers (30 bytes at offset 20) */
    for (i = 0; i < instruments; i++) {
        const uint8_t *sh = r->d + 20 + (size_t)i * 30;
        sample_t *s = &Song.Smp[i];
        uint32_t len, beg, replen;

        s->ID = 0x53504D49u;
        s->GvL = 64;
        len    = (uint32_t)((sh[22] << 8) | sh[23]) * 2;
        beg    = (uint32_t)((sh[24 + 2] << 8) | sh[24 + 3]);
        replen = (uint32_t)((sh[28] << 8) | sh[29]);
        s->Flags = 0;
        if (replen > 1)
            s->Flags |= 0x10;
        if (len > 1)
            s->Flags |= 1;
        s->Vol = sh[25];
        memcpy(s->SampleName, sh, 22);
        s->Cvt = 1;                     /* signed */
        s->DfP = 32;
        s->Length = len;
        if (instruments == 31) {
            beg *= 2;
            replen *= 2;
        } else
            replen *= 2;                /* end advance always doubled */
        if (beg > len)
            beg = 0;
        s->LoopBeg = beg;
        s->LoopEnd = beg + replen;
        if (s->LoopEnd > len)
            s->LoopEnd = len;
        s->C5Speed = FineTuneTable[sh[24] & 15];
    }

    /* patterns: channels*256 bytes each (PE_TranslateMODPattern) */
    rdseek(r, patoff);
    for (p = 0; p < npat && p < 200; p++) {
        grid_clear();
        for (i = 0; i < 64; i++) {
            for (c = 0; c < channels && c < 64; c++) {
                uint8_t b0 = rd8(r), b1 = rd8(r), b2 = rd8(r),
                        b3 = rd8(r);
                editcell_t *e = gcell(i, c);
                unsigned period = ((b0 & 0x0F) << 8) | b1;
                int smp = (b0 & 0xF0) | (b2 >> 4), k;

                for (k = 0; k < 72; k++)
                    if (period >= MODPeriodTable[k]) {
                        put_note(e, k + 36);
                        break;
                    }
                put_ins(e, smp);
                mod_command(e, (uint8_t)(b2 & 0x0F), b3);
            }
        }
        Pattern_Pack((uint16_t)p, IGrid, 64);
    }

    /* sample data: sequential after the patterns */
    for (i = 0; i < instruments; i++)
        if (Song.Smp[i].Flags & 1)
            import_sample_data(r, &Song.Smp[i]);
    return 1;
}

/* ================= MTM (D_LoadMTM, research R4) ===================== */

static int load_mtm(rd_t *r)
{
    uint8_t hdr[66];
    int ntracks, npat, nsmp, i, c, p;
    size_t trackbase, seqbase;
    static uint8_t trackbuf[32 * 192];

    rdbytes(r, hdr, 66);
    if (memcmp(hdr, "MTM", 3) != 0)
        return 0;

    import_clear();
    memcpy(Song.Header.SongName, hdr + 4, 20);

    ntracks = hdr[24] | (hdr[25] << 8);
    npat = hdr[26] + 1;                 /* last pattern number */
    nsmp = hdr[30];
    if (nsmp > 99) nsmp = 99;

    Song.Header.OrdNum = (uint16_t)(hdr[27] + 1);
    Song.Header.InsNum = 0;
    Song.Header.SmpNum = (uint16_t)nsmp;
    Song.Header.PatNum = (uint16_t)npat;
    Song.Header.Flags = 0x31;
    Song.Header.GV = 128;
    Song.Header.MV = 48;
    Song.Header.IS = 6;
    Song.Header.IT = 125;
    Song.Header.Sep = 128;
    for (c = 0; c < 32; c++)
        Song.Header.ChnlPan[c] = PanningPositions[hdr[34 + c] & 15];
    for (; c < 64; c++)
        Song.Header.ChnlPan[c] = 160;
    memset(Song.Header.ChnlVol, 64, 64);

    /* sample headers (37 bytes each) */
    for (i = 0; i < nsmp; i++) {
        uint8_t sh[37];
        sample_t *s = &Song.Smp[i];
        uint32_t len, beg, end;
        int k;

        rdbytes(r, sh, 37);
        s->ID = 0x53504D49u;
        s->GvL = 0x40;
        memcpy(&len, sh + 22, 4);
        memcpy(&beg, sh + 26, 4);
        memcpy(&end, sh + 30, 4);
        s->Flags = (uint8_t)((sh[36] & 1) << 1);        /* 16 bit */
        if (len)
            s->Flags |= 1;
        if (end - beg > 2)
            s->Flags |= 0x10;
        s->Vol = sh[35];
        for (k = 0; k < 22 && sh[k]; k++)               /* NUL stops */
            s->SampleName[k] = (char)sh[k];
        s->Cvt = 0;                     /* unsigned */
        s->DfP = 32;
        s->Length = len;
        if (s->Flags & 0x10) {
            s->LoopBeg = beg;
            s->LoopEnd = end;
        }
        s->C5Speed = FineTuneTable[sh[34] & 15];
    }

    {                                   /* order list */
        uint8_t ords[128];
        rdbytes(r, ords, 128);
        memcpy(Song.Orders, ords, (size_t)Song.Header.OrdNum < 128
                                  ? Song.Header.OrdNum : 128);
    }

    trackbase = 66 + (size_t)nsmp * 37 + 128;
    seqbase = trackbase + (size_t)ntracks * 192;

    for (p = 0; p < npat && p < 200; p++) {
        uint16_t seq[32];
        rdseek(r, seqbase + (size_t)p * 64);
        for (c = 0; c < 32; c++)
            seq[c] = rd16(r);
        for (c = 0; c < 32; c++) {
            if (seq[c] == 0 || seq[c] > ntracks)
                memset(trackbuf + c * 192, 0, 192);
            else {
                rdseek(r, trackbase + ((size_t)seq[c] - 1) * 192);
                rdbytes(r, trackbuf + c * 192, 192);
            }
        }
        /* PE_TranslateMTMPattern: 3-byte events, channel-major */
        grid_clear();
        for (c = 0; c < 32; c++) {
            const uint8_t *t = trackbuf + c * 192;
            for (i = 0; i < 64; i++) {
                uint8_t b0 = t[i*3], b1 = t[i*3 + 1], b2 = t[i*3 + 2];
                editcell_t *e = gcell(i, c);
                int pitch = b0 >> 2;
                if (pitch)
                    put_note(e, pitch + 36);
                put_ins(e, ((b0 & 3) << 4) | (b1 >> 4));
                mod_command(e, (uint8_t)(b1 & 0x0F), b2);
            }
        }
        Pattern_Pack((uint16_t)p, IGrid, 64);
    }

    /* comment -> song message (NULs to spaces, CR every 40) */
    {
        uint16_t clen = (uint16_t)(hdr[28] | (hdr[29] << 8));
        size_t cbase = seqbase + (size_t)npat * 64;
        int n;
        rdseek(r, cbase);
        if (clen >= IT_MESSAGELENGTH)
            clen = IT_MESSAGELENGTH - 1;
        rdbytes(r, IT_MessageData, clen);
        for (n = 0; n < clen; n++) {
            if (IT_MessageData[n] == 0)
                IT_MessageData[n] = 32;
            if (n % 40 == 39)
                IT_MessageData[n] = 13;
        }
        IT_MessageData[clen] = 0;
    }

    for (i = 0; i < nsmp; i++)          /* sample data sequential */
        if (Song.Smp[i].Flags & 1)
            import_sample_data(r, &Song.Smp[i]);
    return 1;
}

/* ================= 669 (D_Load669, research R5) ===================== */

static int load_669(rd_t *r)
{
    uint8_t hdr[0x1F1];
    int nsmp, npat, i, c, p;

    rdbytes(r, hdr, 0x1F1);
    if (!((hdr[0] == 'i' && hdr[1] == 'f') ||
          (hdr[0] == 'J' && hdr[1] == 'N')))
        return 0;

    import_clear();
    memcpy(Song.Header.SongName, hdr + 2, 25);

    nsmp = hdr[0x6E];
    npat = hdr[0x6F];
    if (nsmp > 99) nsmp = 99;
    if (npat > 200) npat = 200;
    Song.Header.SmpNum = (uint16_t)nsmp;
    Song.Header.PatNum = (uint16_t)npat;
    Song.Header.InsNum = 0;
    Song.Header.OrdNum = 128;
    Song.Header.Flags = 0x19;           /* stereo, linear, old fx */
    Song.Header.Special = 0;
    Song.Header.GV = 128;
    Song.Header.MV = 48;
    Song.Header.IS = 6;
    Song.Header.IT = 78;
    Song.Header.Sep = 64;
    for (c = 0; c < 8; c++)
        Song.Header.ChnlPan[c] = (c & 1) ? 64 : 0;
    for (; c < 64; c++)
        Song.Header.ChnlPan[c] = 32 + 128;
    memset(Song.Header.ChnlVol, 64, 64);
    memcpy(Song.Orders, hdr + 0x71, 128);

    /* samples (25-byte headers) */
    for (i = 0; i < nsmp; i++) {
        uint8_t sh[25];
        sample_t *s = &Song.Smp[i];
        uint32_t len, beg, end;

        rdbytes(r, sh, 25);
        memcpy(&len, sh + 13, 4);
        memcpy(&beg, sh + 17, 4);
        memcpy(&end, sh + 21, 4);
        s->ID = 0x53504D49u;
        s->GvL = 0x40;
        memcpy(s->DOSFileName, sh, 12);
        memcpy(s->SampleName, sh, 13);
        s->Vol = 60;
        s->Flags = 0;
        if (len) {
            s->Flags = 1;
            if (end <= len && end >= beg + 2) {
                s->Flags |= 0x10;
                s->LoopBeg = beg;
                s->LoopEnd = end;
            }
        }
        s->Cvt = 0;                     /* unsigned */
        s->DfP = 32;
        s->Length = len;
        s->C5Speed = 8363;
    }

    /* patterns: 0x600 bytes = 64 rows x 8 channels x 3 bytes */
    for (p = 0; p < npat; p++) {
        uint8_t pd[0x600];
        uint16_t chmem[8];              /* EncodingInfo: repeat memory */
        int rows, brk = hdr[0x171 + p];

        rdbytes(r, pd, 0x600);
        grid_clear();
        memset(chmem, 0, sizeof(chmem));

        if (brk < 31) {
            rows = 32;
            put_cmd(gcell(brk, 8), CMD('C'), 0);
        } else
            rows = brk + 1;
        if (rows > 64)
            rows = 64;
        put_cmd(gcell(0, 8), CMD('A'), hdr[0xF1 + p]);

        for (i = 0; i < rows && i < 64; i++) {
            for (c = 0; c < 8; c++) {
                const uint8_t *ev = pd + ((size_t)i * 8 + c) * 3;
                editcell_t *e = gcell(i, c);
                uint8_t b0 = ev[0], b1 = ev[1], b2 = ev[2];

                if (b0 < 0xFE) {
                    put_note(e, (b0 >> 2) + 36);
                    put_ins(e, (((b0 & 3) << 4) | (b1 >> 4)) + 1);
                    put_vol(e, (b1 & 0x0F) << 2);
                    chmem[c] = 0;       /* new note clears the memory */
                } else if (b0 == 0xFE)
                    put_vol(e, (b1 & 0x0F) << 2);

                if (b2 != 0xFF) {
                    uint8_t sub = (uint8_t)(b2 & 0xF0);
                    uint8_t x = (uint8_t)(b2 & 0x0F);
                    uint16_t cv = 0;
                    switch (sub) {
                    case 0x00: cv = (uint16_t)(CMD('F') | (x << 8));
                               chmem[c] = cv; break;
                    case 0x10: cv = (uint16_t)(CMD('E') | (x << 8));
                               chmem[c] = cv; break;
                    case 0x20: cv = (uint16_t)(CMD('G') | (x << 8));
                               chmem[c] = cv; break;
                    case 0x30: cv = (uint16_t)(CMD('E') | 0xF100);
                               chmem[c] = 0; break;
                    case 0x40: cv = (uint16_t)(CMD('H') | ((x | 0x80) << 8));
                               chmem[c] = cv; break;
                    case 0x50: cv = (uint16_t)(CMD('A') | (x << 8));
                               chmem[c] = 0; break;
                    default:   cv = chmem[c]; break;
                    }
                    put_cmd(e, cv & 0xFF, cv >> 8);
                } else
                    put_cmd(e, chmem[c] & 0xFF, chmem[c] >> 8);
            }
        }
        Pattern_Pack((uint16_t)p, IGrid, (uint16_t)(rows < 32 ? 32 : rows));
    }

    for (i = 0; i < nsmp; i++)
        if (Song.Smp[i].Flags & 1)
            import_sample_data(r, &Song.Smp[i]);
    return 1;
}

/* ================= XM (D_LoadXM, research R6) ======================= */

/* TranslateXMEffectVolume: encode the XM volume byte as an IT effect
 * (used when the effect column frees itself for the volume effect) and
 * set the vol column to `dh` (0xFF = empty). */
static void xm_volswap(editcell_t *e, uint8_t vol, uint8_t dh)
{
    uint8_t x = (uint8_t)(vol & 0x0F);

    e->mask &= (uint8_t)~CM_VOL;
    put_vol(e, dh);
    if (vol < 0x60)
        return;
    if (vol < 0x70)
        put_cmd(e, CMD('D'), x);
    else if (vol < 0x80)
        put_cmd(e, CMD('D'), x << 4);
    else if (vol < 0x90) {
        uint8_t v = x ? (uint8_t)(x | 0xF0) : 0;
        if (v == 0xFF)
            v = 0xFE;
        put_cmd(e, CMD('D'), v);
    } else if (vol < 0xA0)
        put_cmd(e, CMD('D'), x ? (uint8_t)((x << 4) | 0x0F) : 0);
    else if (vol < 0xB0)
        put_cmd(e, CMD('H'), x << 4);
    else if (vol < 0xC0)
        put_cmd(e, CMD('H'), x <= 4 ? x : x - 1);
    else if (vol < 0xD0)
        put_cmd(e, CMD('S'), x | 0x80);
    else if (vol < 0xE0)
        put_cmd(e, CMD('P'), x << 4);
    else if (vol < 0xF0)
        put_cmd(e, CMD('P'), x);
    else
        put_cmd(e, CMD('G'), x << 4);
}

/* TranslateXMVolume: XM volume byte -> IT volume-column value */
static uint8_t xm_volume(uint8_t v)
{
    uint8_t x = (uint8_t)(v & 0x0F);

    if (v < 0x10)  return 0xFF;
    if (v <= 0x50) return (uint8_t)(v - 0x10);
    if (v < 0x60)  return 0xFF;
    if (x > 9)     x = 9;
    if (v < 0x70)  return (uint8_t)(95 + x);    /* volume down  */
    if (v < 0x80)  return (uint8_t)(85 + x);    /* volume up    */
    if (v < 0x90)  return (uint8_t)(75 + x);    /* fine down    */
    if (v < 0xA0)  return (uint8_t)(65 + x);    /* fine up      */
    if (v < 0xB0)  return 0xFF;                 /* vibrato speed */
    if (v < 0xC0) {                             /* vibrato depth */
        uint8_t y = (uint8_t)(v & 0x0F);
        if (y > 3)
            y--;
        return (uint8_t)(203 + y);
    }
    if (v < 0xD0)                               /* panning       */
        return (uint8_t)((((v - 0xC0) << 2) | (v - 0xC0)) + 128);
    if (v < 0xF0)  return 0xFF;                 /* pan slides    */
    return XMEffectG[v & 0x0F];                 /* porta         */
}

/* TranslateXMEffect (given XM effect number + param + raw vol byte) */
static void xm_effect(editcell_t *e, uint8_t fx, uint8_t p, uint8_t vol)
{
    if (fx == 0 && p == 0) {
        if (vol >= 0x60)
            xm_volswap(e, vol, 0xFF);
        return;
    }
    if (fx == 20) {                     /* Kxx: key off */
        if (!(e->mask & CM_NOTE)) {
            put_note(e, 255);
            e->mask &= (uint8_t)~CM_INS;
        }
        if (vol >= 0x60)
            xm_volswap(e, vol, 0xFF);
        return;
    }
    switch (fx) {
    case 0:  put_cmd(e, CMD('J'), p); break;
    case 1:
        if (p == 0 && vol >= 0x60) { xm_volswap(e, vol, 105); break; }
        put_cmd(e, CMD('F'), p);
        break;
    case 2:
        if (p == 0 && vol >= 0x60) { xm_volswap(e, vol, 115); break; }
        put_cmd(e, CMD('E'), p);
        break;
    case 3:
        if (p == 0 && vol >= 0x60) { xm_volswap(e, vol, 193); break; }
        put_cmd(e, CMD('G'), p);
        break;
    case 4: {
        uint8_t hi = (uint8_t)(p & 0xF0), lo = (uint8_t)(p & 0x0F);
        if (lo > 4)
            lo--;
        p = (uint8_t)(hi | lo);
        if (p == 0 && vol >= 0x60) { xm_volswap(e, vol, 203); break; }
        put_cmd(e, CMD('H'), p);
        break;
    }
    case 5:  put_cmd(e, CMD('L'), p); break;
    case 6:  put_cmd(e, CMD('K'), p); break;
    case 7:  put_cmd(e, CMD('R'), p); break;
    case 8:
        if (vol >= 0x60) {
            unsigned dh = ((unsigned)p >> 2) + ((p >> 1) & 1) + 128;
            xm_volswap(e, vol, (uint8_t)dh);
            put_cmd(e, CMD('X'), p);    /* effect stays X */
            break;
        }
        put_cmd(e, CMD('X'), p);
        break;
    case 9:  put_cmd(e, CMD('O'), p); break;
    case 10: put_cmd(e, CMD('D'), p); break;
    case 11: put_cmd(e, CMD('B'), p); break;
    case 12:                            /* set volume -> vol column */
        xm_volswap(e, vol, p > 64 ? 64 : p);
        break;
    case 13:                            /* BCD break */
        put_cmd(e, CMD('C'), (uint8_t)((p & 0x0F) + (p >> 4) * 10));
        break;
    case 14: {                          /* E-sub */
        uint8_t sub = (uint8_t)(p & 0xF0), x = (uint8_t)(p & 0x0F);
        switch (sub) {
        case 0x00: break;
        case 0x10: put_cmd(e, CMD('F'), x ? (uint8_t)(x | 0xF0) : 0);
                   break;
        case 0x20: put_cmd(e, CMD('E'), x ? (uint8_t)(x | 0xF0) : 0);
                   break;
        case 0x30: break;
        case 0x40: put_cmd(e, CMD('S'), x | 0x30); break;
        case 0x50: break;
        case 0x60: put_cmd(e, CMD('S'), x | 0xB0); break;
        case 0x70: put_cmd(e, CMD('S'), x | 0x40); break;
        case 0x80: put_cmd(e, CMD('S'), x | 0x80); break;
        case 0x90:
            if (x)
                put_cmd(e, CMD('Q'), x);
            else if (vol >= 0x60)
                xm_volswap(e, vol, 0xFF);
            break;
        case 0xA0: put_cmd(e, CMD('D'),
                           x ? (uint8_t)((x << 4) | 0x0F) : 0);
                   break;
        case 0xB0: {
            uint8_t v = x ? (uint8_t)(x | 0xF0) : 0;
            if (v == 0xFF)
                v = 0xFE;
            put_cmd(e, CMD('D'), v);
            break;
        }
        case 0xC0: put_cmd(e, CMD('S'), x | 0xC0); break;
        case 0xD0: put_cmd(e, CMD('S'), x | 0xD0); break;
        case 0xE0: put_cmd(e, CMD('S'), x | 0xE0); break;
        default:   break;
        }
        break;
    }
    case 15: put_cmd(e, p <= 0x20 ? CMD('A') : CMD('T'), p); break;
    case 16:                            /* Gxx global volume -> V */
        put_cmd(e, CMD('V'), p >= 0x40 ? 0x80 : (uint8_t)(p << 1));
        break;
    case 17: {                          /* Hxx global slide -> W */
        unsigned hi = (unsigned)(p & 0xF0) << 1;
        unsigned lo = (unsigned)(p & 0x0F) << 1;
        if (lo > 0x0F) lo = 0x0F;
        if (hi > 0xF0) hi = 0xF0;
        put_cmd(e, CMD('W'), (uint8_t)(hi | lo));
        break;
    }
    case 27: put_cmd(e, CMD('Q'), p); break;    /* Rxx retrig */
    case 29: {                          /* Txy tremor -> I */
        uint8_t hi = (uint8_t)(p & 0xF0), lo = (uint8_t)(p & 0x0F);
        if (!hi) hi = 0x10;
        if (!lo) lo = 1;
        put_cmd(e, CMD('I'), hi | lo);
        break;
    }
    case 33: {                          /* X1x/X2x extra fine porta */
        uint8_t sub = (uint8_t)(p & 0xF0);
        uint8_t v = (uint8_t)((p & 0x0F) | 0xE0);
        if (sub == 0x10)
            put_cmd(e, CMD('F'), v);
        else if (sub == 0x20)
            put_cmd(e, CMD('E'), v);
        break;
    }
    case 35: put_cmd(e, CMD('Z'), p); break;
    default:
        if (vol >= 0x60)
            xm_volswap(e, vol, 0xFF);
        break;
    }
}

/* D_InsertOrder: insert `newpat` after every occurrence of `oldpat` */
static void insert_order_after(uint8_t oldpat, uint8_t newpat)
{
    int si;
    for (si = 255; si >= 0; si--) {
        if (Song.Orders[si] != oldpat)
            continue;
        if (si < 254)
            memmove(&Song.Orders[si + 2], &Song.Orders[si + 1],
                    (size_t)(254 - si));
        if (si + 1 < 256)
            Song.Orders[si + 1] = newpat;
    }
}

/* one packed XM pattern half; advances *pos; returns rows packed */
static void xm_pattern_rows(rd_t *r, int rows, int channels, int pat)
{
    int i, c;
    int lastrow = rows - 1;
    int gridrows = rows < 32 ? 32 : rows;

    grid_clear();
    for (i = 0; i < rows && i < MAX_PATROWS; i++) {
        for (c = 0; c < channels && c < 64; c++) {
            editcell_t *e = gcell(i, c);
            uint8_t b = rd8(r);
            uint8_t note = 0, ins = 0, vol = 0, fx = 0, fp = 0;
            int hasnote = 0, hasins = 0, hasvol = 0;

            if (b & 0x80) {
                if (b & 1)  { note = rd8(r); hasnote = 1; }
                if (b & 2)  { ins = rd8(r);  hasins = 1; }
                if (b & 4)  { vol = rd8(r);  hasvol = 1; }
                if (b & 8)  fx = rd8(r);
                if (b & 16) fp = rd8(r);
            } else {
                note = b;    hasnote = 1;
                ins = rd8(r);  hasins = 1;
                vol = rd8(r);  hasvol = 1;
                fx = rd8(r);
                fp = rd8(r);
            }
            if (hasnote) {              /* TranslateXMNote (0 -> B-0!) */
                if (note > 96)
                    put_note(e, 255);
                else
                    put_note(e, note + 11);
            }
            if (hasins) {               /* TranslateXMInstrument */
                if (ins > 99) {
                    if (!(e->mask & CM_NOTE) ||
                        e->note != GNOTE_OFF) {
                        e->mask &= (uint8_t)~CM_NOTE;   /* kill note */
                        e->note = 0;
                    }
                } else if ((e->mask & CM_NOTE) && e->note == GNOTE_OFF) {
                    /* note off: instrument dropped */
                } else
                    put_ins(e, ins);
            }
            if (hasvol)
                put_vol(e, xm_volume(vol));
            xm_effect(e, fx, fp, hasvol ? vol : 0);
        }
    }
    /* rows < 32: pattern padded to 32 with a C00 break on the last
     * data row (first channel with a free effect) */
    if (lastrow < gridrows - 1) {
        for (c = 0; c < 64; c++) {
            editcell_t *e = gcell(lastrow, c);
            if (!(e->mask & CM_CMD)) {
                put_cmd(e, CMD('C'), 0);
                break;
            }
        }
    }
    Pattern_Pack((uint16_t)pat, IGrid, (uint16_t)gridrows);
}

static int load_xm(rd_t *r)
{
    uint8_t hdr[80 + 24];
    uint16_t nch, npat, nins, songlen, xmflags;
    int patcount, i, p, c;
    int smpbase = 1;                    /* authentic SmpNum off-by-one */

    rdbytes(r, hdr, 60);
    if (memcmp(hdr, "Extended Module: ", 17) != 0)
        return 0;
    {
        uint32_t hsize;
        size_t hstart = r->pos;
        hsize = rd32(r);
        songlen = rd16(r);
        rd16(r);                        /* restart position */
        nch     = rd16(r);
        npat    = rd16(r);
        nins    = rd16(r);
        xmflags = rd16(r);

        import_clear();
        memcpy(Song.Header.SongName, hdr + 17, 20);

        Song.Header.InsNum = nins > 99 ? 99 : nins;
        Song.Header.SmpNum = 1;
        Song.Header.PatNum = npat > 200 ? 200 : npat;
        Song.Header.Flags =
            (uint16_t)(((xmflags & 1) << 3) | 0x35);
        Song.Header.GV = 128;
        Song.Header.MV = 48;
        {
            uint8_t spd = (uint8_t)rd16(r);
            Song.Header.IS = spd ? spd : 6;
            Song.Header.IT = (uint8_t)rd16(r);
        }
        Song.Header.Sep = 128;
        for (c = 0; c < nch && c < 64; c++)
            Song.Header.ChnlPan[c] = 32;
        for (; c < 64; c++)
            Song.Header.ChnlPan[c] = 128 + 32;
        memset(Song.Header.ChnlVol, 64, 64);

        patcount = Song.Header.PatNum;
        Song.Header.OrdNum = songlen;
        for (i = 0; i < songlen && i < 256; i++) {
            uint8_t o = rd8(r);
            if (o >= 200)
                o = 0xFF;
            else if (o >= patcount)
                patcount = o + 1;
            Song.Orders[i] = o;
        }
        rdseek(r, hstart + hsize);      /* end of header */
    }

    /* patterns */
    for (p = 0; p < npat; p++) {
        uint32_t phlen;
        uint16_t rows, dlen;
        size_t pstart;

        phlen = rd32(r);
        pstart = r->pos - 4;
        rd8(r);                         /* packing type */
        rows = rd16(r);
        dlen = rd16(r);
        rdseek(r, pstart + phlen);
        if (dlen == 0)
            continue;
        {
            size_t dstart = r->pos;
            if (p < 200) {
                if (rows <= 200)
                    xm_pattern_rows(r, rows, nch, p);
                else {                  /* split into two patterns */
                    xm_pattern_rows(r, rows >> 1, nch, p);
                    if (patcount < 200) {
                        int np = patcount++;
                        insert_order_after((uint8_t)p, (uint8_t)np);
                        xm_pattern_rows(r, (rows + 1) >> 1, nch, np);
                    }
                }
            }
            rdseek(r, dstart + dlen);
        }
    }
    if (patcount > Song.Header.PatNum)
        Song.Header.PatNum = (uint16_t)patcount;

    /* instruments */
    for (i = 0; i < nins && i < 99; i++) {
        static uint8_t ib[2048];
        uint32_t isz;
        size_t istart = r->pos;
        instrument_t *ins = &Song.Ins[i];
        uint16_t numsmp, shsize16;

        isz = rd32(r);
        if (isz < 4 || isz > sizeof(ib))
            isz = isz < 4 ? 4 : sizeof(ib);
        rdseek(r, istart);
        rdbytes(r, ib, isz > sizeof(ib) ? sizeof(ib) : isz);
        rdseek(r, istart + (ib[0] | (ib[1] << 8) |
                            ((uint32_t)ib[2] << 16) |
                            ((uint32_t)ib[3] << 24)));

        memcpy(ins->InstrumentName, ib + 4, 22);
        numsmp = (uint16_t)(ib[27] | (ib[28] << 8));
        if (numsmp == 0)
            continue;
        shsize16 = (uint16_t)(ib[29] | (ib[30] << 8));
        if (shsize16 == 0)
            shsize16 = 40;

        /* fadeout */
        {
            unsigned f = (unsigned)(ib[239] | (ib[240] << 8));
            f = (f + 15) >> 5;
            if (f > 256)
                f = 256;
            ins->FadeOut = (uint16_t)f;
        }
        ins->GbV = 128;
        ins->DfP = 32 + 128;
        ins->NNA = 0;

        /* note translation table */
        for (c = 0; c < 12; c++) {
            ins->NoteSampleTable[c * 2] = (uint8_t)c;
            ins->NoteSampleTable[c * 2 + 1] = 0;
        }
        for (c = 0; c < 96; c++) {
            unsigned smp = (unsigned)ib[33 + c] + smpbase;
            if (smp > 99)
                smp = 99;
            ins->NoteSampleTable[(12 + c) * 2] = (uint8_t)(12 + c);
            ins->NoteSampleTable[(12 + c) * 2 + 1] = (uint8_t)smp;
        }
        for (c = 108; c < 120; c++) {
            ins->NoteSampleTable[c * 2] = (uint8_t)c;
            ins->NoteSampleTable[c * 2 + 1] = 0;
        }

        /* volume envelope */
        {
            env_t *e = &ins->VEnvelope;
            uint8_t xf = ib[233];
            if (!(xf & 1)) {            /* off: keyoff-cut emulation */
                e->Flags = 0x05;
                e->Num = 2;
                e->LpB = e->LpE = 0;
                e->SLB = e->SLE = 0;
                e->NodePoints[0].Magnitude = 64;
                e->NodePoints[0].Tick = 0;
                e->NodePoints[1].Magnitude = 0;
                e->NodePoints[1].Tick = 1;
            } else {
                uint8_t nn = ib[225];
                e->Flags = (uint8_t)(((xf & 2) << 1) | 3);
                e->Num = nn < 12 ? nn : 12;
                if (xf & 4) {
                    e->LpB = ib[228];
                    e->LpE = ib[229];
                } else
                    e->LpB = e->LpE = (uint8_t)(nn - 1);
                e->SLB = e->SLE = ib[227];
                if (xf == 7 && e->SLB >= ib[229])
                    e->Flags &= (uint8_t)~4;
                if (!(xf & 2)) {        /* no sustain: hold at end */
                    e->SLB = e->SLE = (uint8_t)(nn - 1);
                    e->Flags |= 4;
                }
                for (c = 0; c < 12; c++) {
                    uint8_t y = ib[129 + c * 4 + 2];
                    if (y > 64)
                        y = 64;
                    e->NodePoints[c].Magnitude = (int8_t)y;
                    e->NodePoints[c].Tick =
                        (uint16_t)(ib[129 + c * 4] |
                                   (ib[129 + c * 4 + 1] << 8));
                }
            }
        }
        /* panning envelope */
        {
            env_t *e = &ins->PEnvelope;
            uint8_t xf = ib[234];
            uint8_t nn = ib[226];
            e->Flags = (uint8_t)((xf & 1) | ((xf & 4) >> 1)
                                 | ((xf & 2) << 1));
            e->Num = nn < 12 ? nn : 12;
            e->LpB = ib[231];
            e->LpE = ib[232];
            e->SLB = e->SLE = ib[230];
            if ((xf & 2) && e->SLB >= ib[232])
                e->Flags &= (uint8_t)~4;
            for (c = 0; c < 12; c++) {
                uint8_t y = ib[177 + c * 4 + 2];
                if (y > 64)
                    y = 64;
                e->NodePoints[c].Magnitude = (int8_t)(y - 32);
                e->NodePoints[c].Tick =
                    (uint16_t)(ib[177 + c * 4] |
                               (ib[177 + c * 4 + 1] << 8));
            }
        }

        /* sample headers, then all data sequentially */
        {
            static uint8_t sh[64 * 40];
            int ns = numsmp > 64 ? 64 : numsmp;
            int s;
            for (s = 0; s < ns; s++) {
                rdbytes(r, sh + s * 40,
                        shsize16 < 40 ? shsize16 : 40);
                if (shsize16 > 40)
                    rdseek(r, r->pos + shsize16 - 40);
            }
            for (s = 0; s < ns; s++) {
                const uint8_t *h = sh + s * 40;
                int slot = smpbase - 1 + s;
                sample_t *sm;
                uint32_t len, lbeg, llen;
                int is16 = (h[14] & 0x10) != 0;

                if (slot >= 99)
                    break;
                sm = &Song.Smp[slot];
                sm->ID = 0x53504D49u;
                sm->GvL = 64;
                memcpy(&len, h, 4);
                memcpy(&lbeg, h + 4, 4);
                memcpy(&llen, h + 8, 4);
                sm->Vol = h[12];
                sm->Flags = 0;
                if (len)
                    sm->Flags |= 1;
                if (is16)
                    sm->Flags |= 2;
                if ((h[14] & 3) && llen > 1)
                    sm->Flags |= (uint8_t)(0x10 |
                                           (((h[14] & 3) - 1) << 6));
                memcpy(sm->SampleName, h + 18, 22);
                sm->Cvt = 5;            /* signed + delta */
                sm->DfP = (uint8_t)(((h[15] >> 2) +
                                     ((h[15] >> 1) & 1)) | 0x80);
                {
                    uint32_t a = len, b = lbeg, cc = lbeg + llen;
                    if (a > 4177910) a = 4177910;
                    if (b > 4177910) b = 4177910;
                    if (cc > 4177910) cc = 4177910;
                    if (is16) { a >>= 1; b >>= 1; cc >>= 1; }
                    sm->Length = a;
                    sm->LoopBeg = b;
                    sm->LoopEnd = cc;
                }
                {
                    int rel = (int8_t)h[16] + 60;
                    int ft = ((int8_t)h[13] >> 4) & 0x0F;
                    uint32_t pt;
                    if (rel < 0) rel = 0;           /* port safety */
                    if (rel > 131) rel = 131;
                    pt = PitchTable[rel];
                    sm->C5Speed = (uint32_t)
                        (((uint64_t)FineTuneTable[ft] * pt) >> 16);
                }
                sm->ViS = ib[238];
                sm->ViD = ib[237];
                sm->ViR = (uint8_t)(0x100 - ib[236]);
                sm->ViT = ib[235];
                sm->OffsetInFile = (uint32_t)r->pos;
                rdseek(r, r->pos + len);            /* skip data */
            }
            smpbase += ns;
            Song.Header.SmpNum = (uint16_t)smpbase; /* 1 + total */
        }
    }

    /* all sample data at the end (delta-decoded via Cvt 5) */
    for (i = 0; i < 99; i++) {
        sample_t *s = &Song.Smp[i];
        if (!(s->Flags & 1) || !s->OffsetInFile)
            continue;
        rdseek(r, s->OffsetInFile);
        s->OffsetInFile = 0;
        import_sample_data(r, s);
    }
    return 1;
}

/* ---- format sniffing + dispatch (D_LoadFile*Module) ---------------- */

static int ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return 0;
    dot++;
    while (*dot && *ext) {
        char a = *dot++, b = *ext++;
        if (a >= 'a' && a <= 'z')
            a -= 32;
        if (a != b)
            return 0;
    }
    return *dot == 0 && *ext == 0;
}

int Import_KnownExt(const char *name)
{
    return ext_is(name, "IT") || ext_is(name, "S3M") ||
           ext_is(name, "XM") || ext_is(name, "MOD") ||
           ext_is(name, "MTM") || ext_is(name, "669");
}

/* MOD channel count from the 1080 signature; 0 = not a 31-instr MOD */
static int mod_channels(const uint8_t *sig)
{
    if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) ||
        !memcmp(sig, "FLT4", 4))
        return 4;
    if (sig[1] == 'C' && sig[2] == 'H' && sig[3] == 'N' &&
        sig[0] >= '1' && sig[0] <= '9')
        return sig[0] - '0';
    if (sig[2] == 'C' && sig[3] == 'H' &&
        sig[0] >= '0' && sig[0] <= '9' &&
        sig[1] >= '0' && sig[1] <= '9')
        return (sig[0] - '0') * 10 + (sig[1] - '0');
    if (!memcmp(sig, "OCTA", 4) || !memcmp(sig, "CD81", 4))
        return 8;
    return 0;
}

int Import_LoadModule(const char *path)
{
    FILE *fp;
    long size;
    uint8_t *buf;
    rd_t rr, *r = &rr;
    int ok = 0;

    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 64) {
        fclose(fp);
        return 0;
    }
    if (size >= 4) {                    /* IMPM -> the native loader */
        uint8_t sig[4];
        if (fread(sig, 1, 4, fp) == 4 && !memcmp(sig, "IMPM", 4)) {
            fclose(fp);
            return Music_LoadIT(path);
        }
        fseek(fp, 0, SEEK_SET);
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    r->d = buf;
    r->size = (size_t)size;
    r->pos = 0;

    if (size >= 60 && !memcmp(buf, "Extended Module: ", 17))
        ok = load_xm(r);
    else if (size >= 0x60 && !memcmp(buf + 0x2C, "SCRM", 4))
        ok = load_s3m(r);
    else if (!memcmp(buf, "MTM", 3))
        ok = load_mtm(r);
    else if ((buf[0] == 'i' && buf[1] == 'f') ||
             (buf[0] == 'J' && buf[1] == 'N'))
        ok = load_669(r);
    else if (size > 1084) {
        int ch = mod_channels(buf + 1080);
        if (ch)
            ok = load_mod(r, ch, 31);
        else if (ext_is(path, "MOD"))
            ok = load_mod(r, 4, 15);    /* 15-instrument SoundTracker */
    }

    free(buf);
    if (ok)
        Music_StampBlankSamples();  /* empty slots = SampleHeader template */
    return ok;
}
