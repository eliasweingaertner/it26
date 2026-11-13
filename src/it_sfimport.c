/*
 * it_sfimport.c
 * -------------
 * it26 extension (feature 020): SFZ and SoundFont 2 instruments.
 *
 * Both formats are read into one list of regions (sfi_region_t). For
 * every IT note one region is chosen (the loudest velocity layer, the
 * first round robin, controllers at their defaults, the default key
 * switch), the chosen regions become IT samples (equal ones shared,
 * root key + tuning baked into C5Speed), the set is thinned to the free
 * sample slots when it does not fit, the audio is decoded and an IT
 * instrument (note table, volume envelope from the amplitude envelope,
 * filter from the low-pass) is built. All decisions and formulas:
 * specs/020-sfz-sf2-import/research.md (R1..R12).
 *
 * Editor-side; the engine is untouched. The caller (it_ris.c) puts the
 * result into the song exactly like an .ITI/.XI load.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1       /* strtok_r / strcasecmp under -std=c11 */
#endif
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "it_sfimport.h"
#include "it_ris.h"
#include "it_sample.h"

#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#endif

#define TICKS_PER_SEC 50.0          /* IT's default tempo 125 */

/* =================================================================
 * Regions
 * ================================================================= */

typedef struct sfi_env_t {
    double delay, attack, hold, decay, sustain, release;
} sfi_env_t;

typedef struct sfi_region_t {
    int     lokey, hikey, lovel, hivel;
    int     root;                   /* -1 = the sample file's unity note */
    double  tune;                   /* cents                              */
    char   *path;                   /* SFZ: sample file (owned)           */
    int     relofs;                 /* SFZ: where the part relative to the
                                       root .sfz's directory starts       */
    int     smp, link, stype;       /* SF2: shdr index, partner, type     */
    uint32_t rate;                  /* SF2: sample rate                   */
    int64_t start, end;             /* frames; end exclusive, -1 = all    */
    int     loopmode;               /* -1 file, 0 none, 1 loop, 2 sustain,
                                       3 ping-pong                        */
    int64_t loopstart, loopend;     /* -1 = from the file                 */
    double  gain_db, pan;           /* pan -1..1                          */
    sfi_env_t env;
    double  cutoff, reso;           /* Hz (0 = none), dB                  */
    int     sw;                     /* SFZ sw_last, -1 none               */
    int     ccmiss;                 /* SFZ: a CC condition fails at the
                                       default controller values          */
    int     second;                 /* SFZ: release / legato trigger      */
    int     checked, bad;           /* SFZ: sample file looked up / missing */
    int     seq;                    /* SFZ seq_position (1 = first)       */
    double  rnd;                    /* SFZ lorand                         */
    int     order;
} sfi_region_t;

typedef struct rlist_t {
    sfi_region_t *r;
    int n, cap;
} rlist_t;

static void rl_free(rlist_t *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->r[i].path);
    free(l->r);
    memset(l, 0, sizeof(*l));
}

static int rl_push(rlist_t *l, const sfi_region_t *r)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        sfi_region_t *p = (sfi_region_t *)realloc(l->r,
                                                  (size_t)nc * sizeof(*p));
        if (!p)
            return 0;
        l->r = p;
        l->cap = nc;
    }
    l->r[l->n] = *r;
    l->r[l->n].order = l->n;
    l->n++;
    return 1;
}

static void region_defaults(sfi_region_t *r)
{
    memset(r, 0, sizeof(*r));
    r->lokey = 0;
    r->hikey = 127;
    r->lovel = 0;
    r->hivel = 127;
    r->root = 60;
    r->smp = r->link = -1;
    r->end = -1;
    r->loopmode = -1;
    r->loopstart = r->loopend = -1;
    r->env.sustain = 1.0;
    r->env.release = 0.001;
    r->sw = -1;
    r->seq = 1;
}

/* =================================================================
 * Small helpers
 * ================================================================= */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void set_why(char *why, size_t n, const char *fmt, const char *arg)
{
    if (why && !why[0])
        snprintf(why, n, fmt, arg);
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

/* file name without directory and extension, at most n-1 chars */
static void stem(const char *path, char *out, size_t n)
{
    const char *b = base_name(path);
    const char *dot = strrchr(b, '.');
    size_t len = dot && dot != b ? (size_t)(dot - b) : strlen(b);
    if (len >= n)
        len = n - 1;
    memcpy(out, b, len);
    out[len] = 0;
}

/* RIFF offsets are 32-bit unsigned (files up to 4 GB); long is 32-bit
 * on Windows */
static int seek64(FILE *f, int64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *t;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 64L * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    t = (char *)malloc((size_t)sz + 1);
    if (t && fread(t, 1, (size_t)sz, f) != (size_t)sz) {
        free(t);
        t = NULL;
    }
    if (t)
        t[sz] = 0;
    fclose(f);
    return t;
}

#ifndef _WIN32
/* case-insensitive resolution, one path component at a time (SFZ
 * libraries are mostly authored on Windows / macOS) */
static int resolve_ci(const char *in, char *out, size_t n)
{
    char tmp[1024], cur[1024];
    char *tok, *save = NULL;

    if (strlen(in) >= sizeof(tmp))
        return 0;
    strcpy(tmp, in);
    cur[0] = 0;
    if (tmp[0] == '/')
        strcpy(cur, "/");
    for (tok = strtok_r(tmp, "/", &save); tok;
         tok = strtok_r(NULL, "/", &save)) {
        char probe[1024];
        DIR *d;
        struct dirent *de;
        int found = 0;

        snprintf(probe, sizeof(probe), "%s%s%s", cur,
                 (cur[0] && cur[strlen(cur) - 1] != '/') ? "/" : "", tok);
        if (file_exists(probe) || !strcmp(tok, ".") || !strcmp(tok, "..")) {
            snprintf(cur, sizeof(cur), "%s", probe);
            continue;
        }
        d = opendir(cur[0] ? cur : ".");
        if (!d)
            return 0;
        while ((de = readdir(d))) {
            if (!strcasecmp(de->d_name, tok)) {
                snprintf(probe, sizeof(probe), "%s%s%s", cur,
                         (cur[0] && cur[strlen(cur) - 1] != '/') ? "/" : "",
                         de->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found)
            return 0;
        snprintf(cur, sizeof(cur), "%s", probe);
    }
    snprintf(out, n, "%s", cur);
    return 1;
}
#endif

/* =================================================================
 * SFZ parser (research R5)
 * ================================================================= */

#define SFZ_MAXDEF 64

/* the first #include file of the last parse that could not be read */
static char SfzMissingInclude[64];

typedef struct kv_t {
    char  k[40];
    char *v;
} kv_t;

typedef struct kvlist_t {
    kv_t *a;
    int n, cap;
} kvlist_t;

typedef struct sfz_t {
    char     dir[1024];             /* the root .sfz's directory, '/' end */
    char     default_path[512];
    int      note_offset, octave_offset;
    uint8_t  ccdef[128];
    int      sw_default;            /* -1 none */
    int      level;                 /* 0 control 1 global 2 master 3 group
                                       4 region 5 other                    */
    kvlist_t lv[5];                 /* index 1..4 used                     */
    char     defn[SFZ_MAXDEF][32];
    char    *defv[SFZ_MAXDEF];
    int      ndef;
    rlist_t *out;
    int      fail;
} sfz_t;

static void kv_clear(kvlist_t *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->a[i].v);
    l->n = 0;
}

static void kv_add(kvlist_t *l, const char *k, const char *v)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        kv_t *p = (kv_t *)realloc(l->a, (size_t)nc * sizeof(*p));
        if (!p)
            return;
        l->a = p;
        l->cap = nc;
    }
    snprintf(l->a[l->n].k, sizeof(l->a[l->n].k), "%s", k);
    l->a[l->n].v = xstrdup(v);
    if (l->a[l->n].v)
        l->n++;
}

/* $NAME substitution, longest define first */
static void sfz_subst(const sfz_t *c, const char *in, char *out, size_t n)
{
    size_t o = 0;

    while (*in && o + 1 < n) {
        if (*in == '$') {
            int i, best = -1;
            size_t bl = 0;
            for (i = 0; i < c->ndef; i++) {
                size_t l = strlen(c->defn[i]);
                if (l > bl && !strncmp(in, c->defn[i], l)) {
                    best = i;
                    bl = l;
                }
            }
            if (best >= 0) {
                const char *v = c->defv[best];
                while (*v && o + 1 < n)
                    out[o++] = *v++;
                in += bl;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

/* note number: integer or name (c4 = 60, c-1 = 0); INT_MIN if invalid */
static int sfz_note(const char *v)
{
    static const int semi[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* a..g */
    int s, oct, neg = 0;

    while (*v == ' ')
        v++;
    if (*v == '-' || isdigit((unsigned char)*v))
        return atoi(v);
    if (tolower((unsigned char)*v) < 'a' || tolower((unsigned char)*v) > 'g')
        return INT_MIN;
    s = semi[tolower((unsigned char)*v) - 'a'];
    v++;
    if (*v == '#') {
        s++;
        v++;
    } else if (*v == 'b' && (v[1] == '-' || isdigit((unsigned char)v[1]))) {
        s--;
        v++;
    }
    if (*v == '-') {
        neg = 1;
        v++;
    }
    if (!isdigit((unsigned char)*v))
        return INT_MIN;
    oct = atoi(v);
    if (neg)
        oct = -oct;
    return (oct + 1) * 12 + s;
}

/* per-region scratch while the inherited opcodes are applied */
typedef struct sfz_cond_t {
    int  seq, trig_bad, sw_bad, cc_trig, end_zero, transpose;
    double lorand;
    int  ccset[128];
    int  cclo[128], cchi[128];
    double volume, amp;
    int  lowpass;
    int  disabled;
    char sample[1024];
} sfz_cond_t;

static int sfz_key(const sfz_t *c, const char *v)
{
    int k = sfz_note(v);
    if (k == INT_MIN)
        return INT_MIN;
    return k + c->note_offset + 12 * c->octave_offset;
}

static void sfz_apply(sfz_t *c, sfi_region_t *r, sfz_cond_t *q,
                      const char *k, const char *v)
{
    int key;

    if (!strcmp(k, "sample")) {
        snprintf(q->sample, sizeof(q->sample), "%s", v);
    } else if (!strcmp(k, "lokey")) {
        if ((key = sfz_key(c, v)) != INT_MIN) r->lokey = key;
    } else if (!strcmp(k, "hikey")) {
        if ((key = sfz_key(c, v)) != INT_MIN) r->hikey = key;
    } else if (!strcmp(k, "key")) {
        if ((key = sfz_key(c, v)) != INT_MIN)
            r->lokey = r->hikey = r->root = key;
    } else if (!strcmp(k, "pitch_keycenter")) {
        if (!strcmp(v, "sample"))
            r->root = -1;
        else if ((key = sfz_key(c, v)) != INT_MIN)
            r->root = key;
    } else if (!strcmp(k, "lovel")) {
        r->lovel = atoi(v);
    } else if (!strcmp(k, "hivel")) {
        r->hivel = atoi(v);
    } else if (!strcmp(k, "tune") || !strcmp(k, "pitch")) {
        r->tune = atof(v);                  /* transpose added afterwards */
    } else if (!strcmp(k, "transpose")) {
        q->transpose = atoi(v);
    } else if (!strcmp(k, "offset")) {
        r->start = atoll(v);
    } else if (!strcmp(k, "end")) {
        long long e = atoll(v);
        if (e <= 0)
            q->end_zero = 1;
        else
            r->end = e + 1;
    } else if (!strcmp(k, "loop_mode") || !strcmp(k, "loopmode")) {
        if (!strcmp(v, "no_loop") || !strcmp(v, "one_shot"))
            r->loopmode = 0;
        else if (!strcmp(v, "loop_continuous"))
            r->loopmode = 1;
        else if (!strcmp(v, "loop_sustain"))
            r->loopmode = 2;
    } else if (!strcmp(k, "loop_start") || !strcmp(k, "loopstart")) {
        r->loopstart = atoll(v);
    } else if (!strcmp(k, "loop_end") || !strcmp(k, "loopend")) {
        r->loopend = atoll(v) + 1;
    } else if (!strcmp(k, "volume")) {
        q->volume = atof(v);
    } else if (!strcmp(k, "amplitude")) {
        q->amp = atof(v);
    } else if (!strcmp(k, "pan")) {
        r->pan = atof(v) / 100.0;
    } else if (!strcmp(k, "cutoff")) {
        r->cutoff = atof(v);
    } else if (!strcmp(k, "resonance")) {
        r->reso = atof(v);
    } else if (!strcmp(k, "fil_type") || !strcmp(k, "filtype")) {
        q->lowpass = !strncmp(v, "lpf", 3);
    } else if (!strcmp(k, "ampeg_delay")) {
        r->env.delay = atof(v);
    } else if (!strcmp(k, "ampeg_attack")) {
        r->env.attack = atof(v);
    } else if (!strcmp(k, "ampeg_hold")) {
        r->env.hold = atof(v);
    } else if (!strcmp(k, "ampeg_decay")) {
        r->env.decay = atof(v);
    } else if (!strcmp(k, "ampeg_sustain")) {
        r->env.sustain = atof(v) / 100.0;
    } else if (!strcmp(k, "ampeg_release")) {
        r->env.release = atof(v);
    } else if (!strcmp(k, "seq_position")) {
        q->seq = atoi(v);
    } else if (!strcmp(k, "lorand")) {
        q->lorand = atof(v);
    } else if (!strcmp(k, "trigger")) {
        q->trig_bad = strcmp(v, "attack") && strcmp(v, "first");
    } else if (!strcmp(k, "sw_last")) {
        if ((key = sfz_key(c, v)) != INT_MIN) r->sw = key;
    } else if (!strcmp(k, "sw_down") || !strcmp(k, "sw_up") ||
               !strcmp(k, "sw_previous")) {
        q->sw_bad = 1;
    } else if (!strcmp(k, "sw_default")) {
        if (c->sw_default < 0 && (key = sfz_key(c, v)) != INT_MIN)
            c->sw_default = key;
    } else if (!strncmp(k, "on_locc", 7) || !strncmp(k, "on_hicc", 7)) {
        q->cc_trig = 1;
    } else if ((!strncmp(k, "locc", 4) || !strncmp(k, "hicc", 4)) &&
               isdigit((unsigned char)k[4])) {
        int n = atoi(k + 4);
        if (n >= 0 && n < 128) {
            if (!q->ccset[n]) {
                q->ccset[n] = 1;
                q->cclo[n] = 0;
                q->cchi[n] = 127;
            }
            if (k[0] == 'l')
                q->cclo[n] = atoi(v);
            else
                q->cchi[n] = atoi(v);
        }
    } else if (!strncmp(k, "set_cc", 6) && isdigit((unsigned char)k[6])) {
        int n = atoi(k + 6);
        if (n >= 0 && n < 128)
            c->ccdef[n] = (uint8_t)atoi(v);
    }
}

static void sfz_make_region(sfz_t *c)
{
    sfi_region_t r;
    sfz_cond_t *q = (sfz_cond_t *)calloc(1, sizeof(*q));
    int lv, i;
    char full[1600];

    if (!q)
        return;
    region_defaults(&r);
    q->seq = 1;
    q->amp = 100.0;
    q->lowpass = 1;
    for (lv = 1; lv <= 4; lv++)
        for (i = 0; i < c->lv[lv].n; i++)
            sfz_apply(c, &r, q, c->lv[lv].a[i].k, c->lv[lv].a[i].v);

    r.tune += q->transpose * 100.0;
    if (!q->sample[0] || q->sample[0] == '*' || q->sw_bad || q->cc_trig ||
        q->end_zero || q->amp <= 0.0 ||
        r.lokey < 0 || r.hikey < 0 || r.lokey > r.hikey ||
        r.hivel < r.lovel)
        goto done;
    /* soft conditions: preferences in plan_select, fallbacks per file */
    r.seq = q->seq;
    r.rnd = q->lorand;
    r.second = q->trig_bad;
    for (i = 0; i < 128; i++)
        if (q->ccset[i] &&
            (c->ccdef[i] < q->cclo[i] || c->ccdef[i] > q->cchi[i]))
            r.ccmiss = 1;
    if (!q->lowpass)
        r.cutoff = 0;
    r.gain_db = q->volume + 20.0 * log10(q->amp / 100.0);

    snprintf(full, sizeof(full), "%s%s%s", c->dir, c->default_path,
             q->sample);
    for (i = 0; full[i]; i++)
        if (full[i] == '\\')
            full[i] = '/';
    r.relofs = (int)strlen(c->dir);
    r.path = xstrdup(full);
    if (!r.path || !rl_push(c->out, &r)) {
        free(r.path);
        c->fail = 1;
    }
done:
    free(q);
}

static void sfz_header(sfz_t *c, const char *name, size_t len)
{
    int lv;

    if (c->level == 4)
        sfz_make_region(c);
#define HDR(s) (len == sizeof(s) - 1 && !memcmp(name, s, len))
    if (HDR("control"))
        lv = 0;
    else if (HDR("global"))
        lv = 1;
    else if (HDR("master"))
        lv = 2;
    else if (HDR("group"))
        lv = 3;
    else if (HDR("region"))
        lv = 4;
    else
        lv = 5;
#undef HDR
    if (lv >= 1 && lv <= 4) {
        int k;
        for (k = lv; k <= 4; k++)
            kv_clear(&c->lv[k]);
    }
    c->level = lv;
}

static void sfz_opcode(sfz_t *c, const char *k, const char *rawv)
{
    char v[1024];

    sfz_subst(c, rawv, v, sizeof(v));
    if (c->level == 0) {
        if (!strcmp(k, "default_path")) {
            size_t i;
            snprintf(c->default_path, sizeof(c->default_path), "%s", v);
            for (i = 0; c->default_path[i]; i++)
                if (c->default_path[i] == '\\')
                    c->default_path[i] = '/';
            i = strlen(c->default_path);
            if (i && c->default_path[i - 1] != '/' &&
                i + 1 < sizeof(c->default_path))
                strcat(c->default_path, "/");
        } else if (!strcmp(k, "note_offset")) {
            c->note_offset = atoi(v);
        } else if (!strcmp(k, "octave_offset")) {
            c->octave_offset = atoi(v);
        } else if (!strncmp(k, "set_cc", 6) && isdigit((unsigned char)k[6])) {
            int n = atoi(k + 6);
            if (n >= 0 && n < 128)
                c->ccdef[n] = (uint8_t)atoi(v);
        }
        return;
    }
    if (c->level >= 1 && c->level <= 4) {
        if (!strncmp(k, "set_cc", 6) && isdigit((unsigned char)k[6])) {
            int n = atoi(k + 6);
            if (n >= 0 && n < 128)
                c->ccdef[n] = (uint8_t)atoi(v);
        }
        kv_add(&c->lv[c->level], k, v);
    }
}

static int is_opc_char(char ch)
{
    return isalnum((unsigned char)ch) || ch == '_' || ch == '$';
}

/* does an opcode name followed by '=' start at p? */
static int opcode_at(const char *p)
{
    const char *s = p;
    while (is_opc_char(*p))
        p++;
    return p > s && *p == '=';
}

static void sfz_parse(sfz_t *c, const char *t, int depth)
{
    const char *p = t;

    while (*p && !c->fail) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e)
                break;
            sfz_header(c, p + 1, (size_t)(e - p - 1));
            p = e + 1;
            continue;
        }
        if (*p == '#') {
            if (!strncmp(p, "#define", 7) && isspace((unsigned char)p[7])) {
                const char *n0, *v0, *e;
                p += 7;
                while (*p == ' ' || *p == '\t')
                    p++;
                n0 = p;
                while (*p && !isspace((unsigned char)*p))
                    p++;
                if (c->ndef < SFZ_MAXDEF && p - n0 > 0 &&
                    (size_t)(p - n0) < sizeof(c->defn[0])) {
                    size_t vl;
                    memcpy(c->defn[c->ndef], n0, (size_t)(p - n0));
                    c->defn[c->ndef][p - n0] = 0;
                    while (*p == ' ' || *p == '\t')
                        p++;
                    v0 = p;
                    while (*p && *p != '\n' && *p != '\r' &&
                           !(p[0] == '/' && p[1] == '/'))
                        p++;
                    e = p;
                    while (e > v0 && isspace((unsigned char)e[-1]))
                        e--;
                    vl = (size_t)(e - v0);
                    c->defv[c->ndef] = (char *)malloc(vl + 1);
                    if (c->defv[c->ndef]) {
                        memcpy(c->defv[c->ndef], v0, vl);
                        c->defv[c->ndef][vl] = 0;
                        c->ndef++;
                    }
                }
                continue;
            }
            if (!strncmp(p, "#include", 8)) {
                const char *q0, *q1;
                p += 8;
                q0 = strchr(p, '"');
                q1 = q0 ? strchr(q0 + 1, '"') : NULL;
                if (q0 && q1 && depth < 16) {
                    char raw[512], inc[512], full[1600];
                    size_t l = (size_t)(q1 - q0 - 1);
                    char *txt;
                    size_t i;
                    if (l >= sizeof(raw))
                        l = sizeof(raw) - 1;
                    memcpy(raw, q0 + 1, l);
                    raw[l] = 0;
                    sfz_subst(c, raw, inc, sizeof(inc));
                    snprintf(full, sizeof(full), "%s%s", c->dir, inc);
                    for (i = 0; full[i]; i++)
                        if (full[i] == '\\')
                            full[i] = '/';
                    txt = read_text(full);
#ifndef _WIN32
                    if (!txt) {
                        char ci[1024];
                        if (resolve_ci(full, ci, sizeof(ci)))
                            txt = read_text(ci);
                    }
#endif
                    if (txt) {
                        sfz_parse(c, txt, depth + 1);
                        free(txt);
                    } else if (!SfzMissingInclude[0]) {
                        snprintf(SfzMissingInclude, sizeof(SfzMissingInclude),
                                 "%.60s", base_name(inc));
                    }
                    p = q1 + 1;
                } else {
                    while (*p && *p != '\n')
                        p++;
                }
                continue;
            }
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (opcode_at(p)) {
            char k[40], v[1024];
            const char *k0 = p, *v0, *e;
            size_t kl, vl;

            while (*p != '=')
                p++;
            kl = (size_t)(p - k0);
            if (kl >= sizeof(k))
                kl = sizeof(k) - 1;
            memcpy(k, k0, kl);
            k[kl] = 0;
            v0 = ++p;
            /* the value runs to the end of the line, a header, a comment
             * or the next opcode (so sample paths may contain spaces) */
            while (*p && *p != '\n' && *p != '\r' && *p != '<' &&
                   !(p[0] == '/' && p[1] == '/') &&
                   !(p > v0 && isspace((unsigned char)p[-1]) && opcode_at(p)))
                p++;
            e = p;
            while (e > v0 && isspace((unsigned char)e[-1]))
                e--;
            vl = (size_t)(e - v0);
            if (vl >= sizeof(v))
                vl = sizeof(v) - 1;
            memcpy(v, v0, vl);
            v[vl] = 0;
            sfz_opcode(c, k, v);
            continue;
        }
        p++;                            /* stray character */
    }
}

/* all regions of an .sfz, inheritance applied, key switch filtered */
static int sfz_load_regions(const char *path, rlist_t *out)
{
    sfz_t *c = (sfz_t *)calloc(1, sizeof(*c));
    char *txt;
    int i, lv, active;

    SfzMissingInclude[0] = 0;
    if (!c)
        return 0;
    txt = read_text(path);
    if (!txt) {
        free(c);
        return 0;
    }
    snprintf(c->dir, sizeof(c->dir), "%s", path);
    for (i = 0; c->dir[i]; i++)
        if (c->dir[i] == '\\')
            c->dir[i] = '/';
    {
        char *slash = strrchr(c->dir, '/');
        if (slash)
            slash[1] = 0;
        else
            c->dir[0] = 0;
    }
    c->sw_default = -1;
    c->out = out;
    c->level = 5;                       /* opcodes before any header */
    sfz_parse(c, txt, 0);
    if (c->level == 4)
        sfz_make_region(c);
    free(txt);

    /* key switch: sw_default, else (or when no region answers to the
     * default -- an articulation file of a bigger set) the lowest switch
     * used (R1) */
    active = c->sw_default;
    if (active >= 0) {
        int hit = 0;
        for (i = 0; i < out->n && !hit; i++)
            hit = out->r[i].sw < 0 || out->r[i].sw == active;
        if (!hit)
            active = -1;
    }
    if (active < 0)
        for (i = 0; i < out->n; i++)
            if (out->r[i].sw >= 0 && (active < 0 || out->r[i].sw < active))
                active = out->r[i].sw;
    if (active >= 0) {
        int w = 0;
        for (i = 0; i < out->n; i++) {
            if (out->r[i].sw >= 0 && out->r[i].sw != active) {
                free(out->r[i].path);
                continue;
            }
            out->r[w++] = out->r[i];
        }
        out->n = w;
    }

    /* CC conditions are judged at the default controller values, and
     * release / legato regions are left out -- unless that would leave
     * nothing: a file gated as a whole on a controller, or holding only
     * release noises, is an articulation someone loads on purpose */
    {
        int pass, w, f;
        for (f = 0; f < 2; f++) {
            pass = 0;
            for (i = 0; i < out->n; i++)
                if (!(f ? out->r[i].second : out->r[i].ccmiss))
                    pass = 1;
            if (!pass)
                continue;
            for (i = w = 0; i < out->n; i++) {
                if (f ? out->r[i].second : out->r[i].ccmiss) {
                    free(out->r[i].path);
                    continue;
                }
                out->r[w++] = out->r[i];
            }
            out->n = w;
        }
    }

    for (lv = 0; lv < 5; lv++) {
        kv_clear(&c->lv[lv]);
        free(c->lv[lv].a);
    }
    for (i = 0; i < c->ndef; i++)
        free(c->defv[i]);
    i = !c->fail;
    free(c);
    return i;
}

/* =================================================================
 * SoundFont 2 (research R4)
 * ================================================================= */

typedef struct sf_phdr_t { char name[21]; uint16_t preset, bank, bag; } sf_phdr_t;
typedef struct sf_bag_t  { uint16_t gen; } sf_bag_t;
typedef struct sf_gen_t  { uint16_t op; int16_t amt; } sf_gen_t;
typedef struct sf_inst_t { char name[21]; uint16_t bag; } sf_inst_t;
typedef struct sf_shdr_t {
    char name[21];
    uint32_t start, end, ls, le, rate;
    uint8_t pitch;
    int8_t corr;
    uint16_t link, type;
} sf_shdr_t;

typedef struct sf2_t {
    int        major;
    int64_t    smpl_off;            /* file offset of the 16-bit data */
    uint32_t   smpl_frames;
    sf_phdr_t *ph; int nph;
    sf_bag_t  *pb; int npb;
    sf_gen_t  *pg; int npg;
    sf_inst_t *in; int nin;
    sf_bag_t  *ib; int nib;
    sf_gen_t  *ig; int nig;
    sf_shdr_t *sh; int nsh;
} sf2_t;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sf2_free(sf2_t *s)
{
    free(s->ph); free(s->pb); free(s->pg); free(s->in);
    free(s->ib); free(s->ig); free(s->sh);
    memset(s, 0, sizeof(*s));
}

int SFI_IsSF2(const uint8_t *d, size_t n)
{
    return n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "sfbk", 4);
}

static void sf_name(char *dst, const uint8_t *src)
{
    int i;
    for (i = 0; i < 20 && src[i]; i++)
        dst[i] = (char)((src[i] >= 32 && src[i] < 127) ? src[i] : ' ');
    dst[i] = 0;
    while (i > 0 && dst[i - 1] == ' ')
        dst[--i] = 0;
}

static void sf2_pdta(sf2_t *s, const uint8_t *d, size_t n)
{
    size_t o = 0;

    while (o + 8 <= n) {
        const uint8_t *id = d + o;
        uint32_t sz = le32(d + o + 4);
        const uint8_t *b = d + o + 8;
        int i, cnt;

        if (sz > n - o - 8)
            sz = (uint32_t)(n - o - 8);
#define CHUNK(tag, rec, arr, num)                                         \
        if (!memcmp(id, tag, 4)) {                                        \
            cnt = (int)(sz / (rec));                                      \
            free(arr);                                                    \
            arr = calloc((size_t)(cnt ? cnt : 1), sizeof(*arr));          \
            num = arr ? cnt : 0;
        CHUNK("phdr", 38, s->ph, s->nph)
            for (i = 0; i < s->nph; i++) {
                const uint8_t *r = b + i * 38;
                sf_name(s->ph[i].name, r);
                s->ph[i].preset = le16(r + 20);
                s->ph[i].bank = le16(r + 22);
                s->ph[i].bag = le16(r + 24);
            }
        }
        CHUNK("pbag", 4, s->pb, s->npb)
            for (i = 0; i < s->npb; i++)
                s->pb[i].gen = le16(b + i * 4);
        }
        CHUNK("pgen", 4, s->pg, s->npg)
            for (i = 0; i < s->npg; i++) {
                s->pg[i].op = le16(b + i * 4);
                s->pg[i].amt = (int16_t)le16(b + i * 4 + 2);
            }
        }
        CHUNK("inst", 22, s->in, s->nin)
            for (i = 0; i < s->nin; i++) {
                sf_name(s->in[i].name, b + i * 22);
                s->in[i].bag = le16(b + i * 22 + 20);
            }
        }
        CHUNK("ibag", 4, s->ib, s->nib)
            for (i = 0; i < s->nib; i++)
                s->ib[i].gen = le16(b + i * 4);
        }
        CHUNK("igen", 4, s->ig, s->nig)
            for (i = 0; i < s->nig; i++) {
                s->ig[i].op = le16(b + i * 4);
                s->ig[i].amt = (int16_t)le16(b + i * 4 + 2);
            }
        }
        CHUNK("shdr", 46, s->sh, s->nsh)
            for (i = 0; i < s->nsh; i++) {
                const uint8_t *r = b + i * 46;
                sf_name(s->sh[i].name, r);
                s->sh[i].start = le32(r + 20);
                s->sh[i].end = le32(r + 24);
                s->sh[i].ls = le32(r + 28);
                s->sh[i].le = le32(r + 32);
                s->sh[i].rate = le32(r + 36);
                s->sh[i].pitch = r[40];
                s->sh[i].corr = (int8_t)r[41];
                s->sh[i].link = le16(r + 42);
                s->sh[i].type = le16(r + 44);
            }
        }
#undef CHUNK
        o += 8 + sz + (sz & 1);
    }
}

/* walk the RIFF with fseek; reads only INFO/ifil and pdta */
static int sf2_open(const char *path, sf2_t *s)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12];
    int64_t pos, endpos;

    memset(s, 0, sizeof(*s));
    s->major = 2;
    if (!f)
        return 0;
    if (fread(h, 1, 12, f) != 12 || !SFI_IsSF2(h, 12)) {
        fclose(f);
        return 0;
    }
    endpos = 8 + (int64_t)le32(h + 4);
    pos = 12;
    while (pos + 8 <= endpos) {
        uint8_t ch[12];
        uint32_t sz;

        if (seek64(f, pos) || fread(ch, 1, 8, f) != 8)
            break;
        sz = le32(ch + 4);
        if (!memcmp(ch, "LIST", 4) && sz >= 4 && fread(ch + 8, 1, 4, f) == 4) {
            int64_t lo = pos + 12, le = pos + 8 + (int64_t)sz;
            if (!memcmp(ch + 8, "pdta", 4)) {
                size_t n = (size_t)(le - lo);
                uint8_t *d;
                if (n > 64u * 1024 * 1024)      /* corrupt size */
                    n = 64u * 1024 * 1024;
                d = (uint8_t *)malloc(n ? n : 1);
                if (d)                  /* truncated file: what is there */
                    sf2_pdta(s, d, fread(d, 1, n, f));
                free(d);
            } else {
                /* INFO / sdta: their sub-chunks */
                while (lo + 8 <= le) {
                    uint8_t sc[8];
                    uint32_t ssz;
                    if (seek64(f, lo) || fread(sc, 1, 8, f) != 8)
                        break;
                    ssz = le32(sc + 4);
                    if (!memcmp(sc, "ifil", 4) && ssz >= 4) {
                        uint8_t v[2];
                        if (fread(v, 1, 2, f) == 2)
                            s->major = le16(v);
                    } else if (!memcmp(sc, "smpl", 4)) {
                        s->smpl_off = lo + 8;
                        s->smpl_frames = ssz / 2;
                    }
                    lo += 8 + (int64_t)ssz + (int64_t)(ssz & 1);
                }
            }
        }
        pos += 8 + (int64_t)sz + (int64_t)(sz & 1);
    }
    fclose(f);
    if (s->nph < 2 || s->npb < 1 || s->nin < 1 || s->nib < 1 || s->nsh < 1) {
        sf2_free(s);
        return 0;
    }
    return 1;
}

/* generator set: v[op] for op < 64, `set` bitmask */
typedef struct gset_t { int v[64]; uint64_t set; } gset_t;

static void gs_apply(gset_t *g, const sf_gen_t *gen, int lo, int hi, int ngen)
{
    int i;
    if (lo < 0) lo = 0;
    if (hi > ngen) hi = ngen;
    for (i = lo; i < hi; i++) {
        if (gen[i].op >= 64)
            continue;
        g->v[gen[i].op] = gen[i].amt;
        g->set |= (uint64_t)1 << gen[i].op;
    }
}

static int gs_has(const gset_t *g, int op) { return (g->set >> op) & 1; }

static void gs_range(const gset_t *g, int op, int *lo, int *hi)
{
    *lo = 0;
    *hi = 127;
    if (gs_has(g, op)) {
        *lo = g->v[op] & 0xFF;
        *hi = (g->v[op] >> 8) & 0xFF;
    }
}

static double tc_seconds(int tc)
{
    return tc <= -12000 ? 0.0 : pow(2.0, tc / 1200.0);
}

/* the zones of preset p as regions */
static int sf2_regions(const sf2_t *s, int p, rlist_t *out, int *rom)
{
    int b0, b1, b;
    gset_t pglob;

    memset(&pglob, 0, sizeof(pglob));
    if (p < 0 || p >= s->nph - 1)
        return 0;
    b0 = s->ph[p].bag;
    b1 = s->ph[p + 1].bag;
    if (b1 > s->npb - 1)
        b1 = s->npb - 1;
    for (b = b0; b < b1; b++) {
        gset_t pz = pglob;
        gset_t pzl;
        int inst, i0, i1, ib, plo, phi, pvlo, pvhi;
        gset_t iglob;

        memset(&pzl, 0, sizeof(pzl));
        gs_apply(&pzl, s->pg, s->pb[b].gen, s->pb[b + 1].gen, s->npg);
        if (!gs_has(&pzl, 41)) {
            if (b == b0)
                pglob = pzl;            /* preset global zone */
            continue;
        }
        /* local preset zone overrides the preset global zone */
        {
            int op;
            for (op = 0; op < 64; op++)
                if (gs_has(&pzl, op)) {
                    pz.v[op] = pzl.v[op];
                    pz.set |= (uint64_t)1 << op;
                }
        }
        inst = pz.v[41];
        if (inst < 0 || inst >= s->nin - 1)
            continue;
        gs_range(&pz, 43, &plo, &phi);
        gs_range(&pz, 44, &pvlo, &pvhi);

        i0 = s->in[inst].bag;
        i1 = s->in[inst + 1].bag;
        if (i1 > s->nib - 1)
            i1 = s->nib - 1;
        memset(&iglob, 0, sizeof(iglob));
        for (ib = i0; ib < i1; ib++) {
            gset_t iz = iglob, izl;
            sfi_region_t r;
            const sf_shdr_t *sh;
            int op, klo, khi, vlo, vhi, smp;
            int I[64], P[64];

            memset(&izl, 0, sizeof(izl));
            gs_apply(&izl, s->ig, s->ib[ib].gen, s->ib[ib + 1].gen, s->nig);
            if (!gs_has(&izl, 53)) {
                if (ib == i0)
                    iglob = izl;
                continue;
            }
            for (op = 0; op < 64; op++)
                if (gs_has(&izl, op)) {
                    iz.v[op] = izl.v[op];
                    iz.set |= (uint64_t)1 << op;
                }
            smp = iz.v[53];
            if (smp < 0 || smp >= s->nsh - 1)
                continue;
            sh = &s->sh[smp];
            if (sh->type & 0x8000) {        /* the sound card's ROM */
                if (rom)
                    (*rom)++;
                continue;
            }

            /* instrument values over the SF2 defaults; preset values
             * are added */
            for (op = 0; op < 64; op++) {
                I[op] = gs_has(&iz, op) ? iz.v[op] : 0;
                P[op] = gs_has(&pz, op) ? pz.v[op] : 0;
            }
            if (!gs_has(&iz, 8)) I[8] = 13500;
            for (op = 33; op <= 38; op++)
                if (op != 37 && !gs_has(&iz, op))
                    I[op] = -12000;
            if (!gs_has(&iz, 58)) I[58] = -1;

            gs_range(&iz, 43, &klo, &khi);
            gs_range(&iz, 44, &vlo, &vhi);
            if (plo > klo) klo = plo;
            if (phi < khi) khi = phi;
            if (pvlo > vlo) vlo = pvlo;
            if (pvhi < vhi) vhi = pvhi;
            if (klo > khi || vlo > vhi)
                continue;

            region_defaults(&r);
            r.lokey = klo;
            r.hikey = khi;
            r.lovel = vlo;
            r.hivel = vhi;
            r.smp = smp;
            r.stype = sh->type;
            r.rate = sh->rate;
            if ((sh->type & 6) && sh->link < s->nsh - 1 && sh->link != smp)
                r.link = sh->link;
            r.start = (int64_t)sh->start + I[0] + 32768LL * I[4];
            r.end = (int64_t)sh->end + I[1] + 32768LL * I[12];
            r.loopstart = (int64_t)sh->ls + I[2] + 32768LL * I[45];
            r.loopend = (int64_t)sh->le + I[3] + 32768LL * I[50];
            if (r.end > (int64_t)s->smpl_frames)
                r.end = s->smpl_frames;
            if (r.start < 0)
                r.start = 0;
            if (r.end <= r.start)
                continue;
            switch (I[54] & 3) {
            case 1:  r.loopmode = 1; break;
            case 3:  r.loopmode = 2; break;
            default: r.loopmode = 0; break;
            }
            r.root = (I[58] >= 0 && I[58] <= 127) ? I[58]
                   : (sh->pitch <= 127 ? sh->pitch : 60);
            r.tune = (I[51] + P[51]) * 100.0 + I[52] + P[52] + sh->corr;
            r.gain_db = -0.04 * (I[48] + P[48]);
            r.pan = (I[17] + P[17]) / 500.0;
            if (r.pan < -1) r.pan = -1;
            if (r.pan > 1) r.pan = 1;
            r.env.delay = tc_seconds(I[33] + P[33]);
            r.env.attack = tc_seconds(I[34] + P[34]);
            r.env.hold = tc_seconds(I[35] + P[35]);
            r.env.decay = tc_seconds(I[36] + P[36]);
            {
                int cb = I[37] + P[37];
                r.env.sustain = cb <= 0 ? 1.0
                              : cb >= 1440 ? 0.0 : pow(10.0, -cb / 200.0);
            }
            r.env.release = tc_seconds(I[38] + P[38]);
            {
                int fc = I[8] + P[8];
                r.cutoff = fc >= 13500 ? 0.0 : 8.176 * pow(2.0, fc / 1200.0);
                r.reso = (I[9] + P[9]) / 10.0;
            }
            if (!rl_push(out, &r))
                return 0;
        }
    }
    return 1;
}

/* =================================================================
 * Selection, voices, reduction (R1, R2, R7)
 * ================================================================= */

#define MAXV 120

typedef struct voice_t {
    int reg, partner;               /* region indices; partner -1 */
    int alive;
} voice_t;

typedef struct plan_t {
    voice_t v[MAXV];
    int nv;
    int keyv[120];                  /* voice per IT note, -1 none */
} plan_t;

static int region_same(const sfi_region_t *a, const sfi_region_t *b)
{
    if (a->path || b->path) {
        if (!a->path || !b->path || strcmp(a->path, b->path))
            return 0;
    } else if (a->smp != b->smp) {
        return 0;
    }
    return a->start == b->start && a->end == b->end &&
           a->loopmode == b->loopmode && a->loopstart == b->loopstart &&
           a->loopend == b->loopend && a->root == b->root &&
           a->tune == b->tune && a->gain_db == b->gain_db &&
           a->pan == b->pan && a->rate == b->rate;
}

static void plan_select(const rlist_t *l, plan_t *pl)
{
    int k, i;

    memset(pl, 0, sizeof(*pl));
    for (k = 0; k < 120; k++) {
        int best = -1, maxvel = -1, partner = -1, vi;

        pl->keyv[k] = -1;
        /* the loudest layer, then the first round robin, then the first
         * random alternative, then file order (R1, R2) */
        for (i = 0; i < l->n; i++) {
            const sfi_region_t *r = &l->r[i], *b;
            if (k < r->lokey || k > r->hikey || r->bad)
                continue;
            b = best >= 0 ? &l->r[best] : NULL;
            if (!b || r->hivel > b->hivel ||
                (r->hivel == b->hivel &&
                 (r->seq < b->seq ||
                  (r->seq == b->seq && r->rnd < b->rnd)))) {
                best = i;
                maxvel = r->hivel;
            }
        }
        if (best < 0)
            continue;
        if (l->r[best].link >= 0) {         /* SF2 stereo pair */
            for (i = 0; i < l->n; i++) {
                const sfi_region_t *r = &l->r[i];
                if (i != best && r->smp == l->r[best].link &&
                    k >= r->lokey && k <= r->hikey &&
                    maxvel >= r->lovel && maxvel <= r->hivel) {
                    partner = i;
                    break;
                }
            }
        }
        for (vi = 0; vi < pl->nv; vi++) {
            const voice_t *v = &pl->v[vi];
            if (region_same(&l->r[v->reg], &l->r[best]) &&
                ((v->partner < 0 && partner < 0) ||
                 (v->partner >= 0 && partner >= 0 &&
                  l->r[v->partner].smp == l->r[partner].smp)))
                break;
        }
        if (vi == pl->nv) {
            if (pl->nv >= MAXV)
                continue;
            pl->v[vi].reg = best;
            pl->v[vi].partner = partner;
            pl->v[vi].alive = 1;
            pl->nv++;
        }
        pl->keyv[k] = vi;
    }
}

static int voice_root(const rlist_t *l, const voice_t *v)
{
    int r = l->r[v->reg].root;
    return r < 0 ? 60 : r;
}

/* keys of dead voices go to the alive voice whose root is nearest */
static void plan_reassign(const rlist_t *l, plan_t *pl)
{
    int k, vi;

    for (k = 0; k < 120; k++) {
        int best = -1, bd = INT_MAX;
        if (pl->keyv[k] < 0 || pl->v[pl->keyv[k]].alive)
            continue;
        for (vi = 0; vi < pl->nv; vi++) {
            int d, rt;
            if (!pl->v[vi].alive)
                continue;
            rt = voice_root(l, &pl->v[vi]);
            d = abs(rt - k);
            if (d < bd || (d == bd && rt < voice_root(l, &pl->v[best]))) {
                bd = d;
                best = vi;
            }
        }
        pl->keyv[k] = best;
    }
}

static const rlist_t *SortList;
static int cmp_voice_root(const void *a, const void *b)
{
    const voice_t *x = (const voice_t *)a, *y = (const voice_t *)b;
    int rx = voice_root(SortList, x), ry = voice_root(SortList, y);
    if (rx != ry)
        return rx < ry ? -1 : 1;
    return x->reg - y->reg;
}

/* keep an evenly spread subset of `keep` voices (by root key) */
static void plan_reduce(const rlist_t *l, plan_t *pl, int keep)
{
    int i, n = pl->nv;
    voice_t sorted[MAXV];

    if (keep >= n || keep < 1)
        return;
    memcpy(sorted, pl->v, sizeof(voice_t) * (size_t)n);
    SortList = l;
    /* sort a copy whose `alive` field carries the original index */
    {
        int j;
        for (i = 0; i < n; i++)
            sorted[i].alive = i;
        qsort(sorted, (size_t)n, sizeof(voice_t), cmp_voice_root);
        for (i = 0; i < n; i++)
            pl->v[i].alive = 0;
        if (keep == 1) {
            int best = 0, bd = INT_MAX;
            for (j = 0; j < n; j++) {
                int d = abs(voice_root(l, &sorted[j]) - 60);
                if (d < bd) {
                    bd = d;
                    best = j;
                }
            }
            pl->v[sorted[best].alive].alive = 1;
        } else {
            for (i = 0; i < keep; i++) {
                j = (int)floor((double)i * (n - 1) / (keep - 1) + 0.5);
                pl->v[sorted[j].alive].alive = 1;
            }
        }
    }
    plan_reassign(l, pl);
}

/* =================================================================
 * Audio
 * ================================================================= */

typedef struct audio_t {
    char     path[1024];
    int16_t *pcm;                   /* interleaved */
    uint64_t frames;
    int      ch;
    uint32_t rate;
    int      loopmode;              /* 0 none, 1 loop, 3 ping-pong */
    int64_t  ls, le;
    int      unity;                 /* -1 unknown */
} audio_t;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* AIFF sustain loop from the MARK + INST chunks (dr_wav doesn't
 * report AIFF markers) */
static void aiff_loop(const char *path, audio_t *a)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12];
    long pos, endpos;
    uint16_t mid[64];
    uint32_t mpos[64];
    int nm = 0, mode = 0, beg = -1, end = -1, i;

    if (!f)
        return;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "FORM", 4)) {
        fclose(f);
        return;
    }
    endpos = 8 + (long)be32(h + 4);
    pos = 12;
    while (pos + 8 <= endpos) {
        uint8_t c[8];
        uint32_t sz;
        if (fseek(f, pos, SEEK_SET) || fread(c, 1, 8, f) != 8)
            break;
        sz = be32(c + 4);
        if (!memcmp(c, "MARK", 4) && sz >= 2 && sz < 65536) {
            uint8_t *d = (uint8_t *)malloc(sz);
            if (d && fread(d, 1, sz, f) == sz) {
                uint32_t o = 2;
                int cnt = be16(d);
                /* marker: id, position, pstring padded to an even size */
                for (i = 0; i < cnt && nm < 64 && o + 7 <= sz; i++) {
                    uint32_t plen = 1u + d[o + 6];
                    mid[nm] = be16(d + o);
                    mpos[nm] = be32(d + o + 2);
                    nm++;
                    o += 6 + plen + (plen & 1);
                }
            }
            free(d);
        } else if (!memcmp(c, "INST", 4) && sz >= 20) {
            uint8_t d[20];
            if (fread(d, 1, 20, f) == 20) {
                mode = (int16_t)be16(d + 8);
                beg = be16(d + 10);
                end = be16(d + 12);
            }
        }
        pos += 8 + (long)sz + (long)(sz & 1);
    }
    fclose(f);
    if (mode == 1 || mode == 2) {
        int64_t b = -1, e = -1;
        for (i = 0; i < nm; i++) {
            if (mid[i] == beg) b = mpos[i];
            if (mid[i] == end) e = mpos[i];
        }
        if (b >= 0 && e > b) {
            a->loopmode = mode == 2 ? 3 : 1;
            a->ls = b;
            a->le = e;
        }
    }
}

static void audio_free(audio_t *a)
{
    free(a->pcm);
    memset(a, 0, sizeof(*a));
}

static int path_found(const char *path, char *real, size_t n)
{
    snprintf(real, n, "%s", path);
    if (file_exists(real))
        return 1;
#ifndef _WIN32
    if (resolve_ci(path, real, n))
        return 1;
#endif
    return 0;
}

/* `path` = the root .sfz's directory (relofs bytes) + the sample's
 * relative path. Not found there, the parent directories are tried:
 * multi-program libraries (ARIA banks) keep their programs in a
 * subfolder while the paths are relative to the library's root. */
static int find_sample(const char *path, int relofs, char *real, size_t n)
{
    char dir[1024], alt[1600];
    int up;
    size_t dl;

    if (path_found(path, real, n))
        return 1;
    dl = (size_t)relofs < sizeof(dir) ? (size_t)relofs : sizeof(dir) - 1;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    for (up = 0; up < 3; up++) {
        char *s;
        size_t l = strlen(dir);
        if (l && dir[l - 1] == '/')
            dir[--l] = 0;
        s = strrchr(dir, '/');
        if (!s)
            break;
        s[1] = 0;
        snprintf(alt, sizeof(alt), "%s%s", dir, path + relofs);
        if (path_found(alt, real, n))
            return 1;
    }
    return 0;
}

static int decode_file(const char *path, int relofs, audio_t *a,
                       char *why, size_t wn)
{
    char real[1024];
    sfi_pcm_t pc;
    int r;

    audio_free(a);
    snprintf(a->path, sizeof(a->path), "%s", path);
    a->unity = -1;
    if (!find_sample(path, relofs, real, sizeof(real))) {
        set_why(why, wn, "missing sample: %.60s", base_name(path));
        return 0;
    }
    r = SFI_DecodeAudio(real, &pc, (uint64_t)SMP_MAXFRAMES * 4);
    if (r <= 0) {
        const char *dot = strrchr(base_name(path), '.');
        free(pc.pcm);
        if (r < 0)
            set_why(why, wn, "empty, too long or no memory: %.50s",
                    base_name(path));
        else
            set_why(why, wn, "unsupported sample format: %.50s",
                    dot ? dot : base_name(path));
        return 0;
    }
    a->pcm = pc.pcm;
    a->frames = pc.frames;
    a->ch = pc.ch;
    a->rate = pc.rate;
    a->loopmode = pc.loopmode;
    a->ls = pc.ls;
    a->le = pc.le;
    a->unity = pc.unity;
    if (pc.aiff)
        aiff_loop(real, a);
    return 1;
}

/* =================================================================
 * IT conversion (R3, R8, R9, R10)
 * ================================================================= */

static uint32_t c5speed(double rate, int root, double tune)
{
    double c = rate * pow(2.0, (60 - root) / 12.0 + tune / 1200.0);
    if (c < 1.0) c = 1.0;
    if (c > 9999999.0) c = 9999999.0;
    return (uint32_t)floor(c + 0.5);
}

static uint8_t gvl_from_db(double db)
{
    double g = 64.0 * pow(10.0, db / 20.0);
    if (g > 64.0) g = 64.0;
    if (g < 0.0) g = 0.0;
    return (uint8_t)floor(g + 0.5);
}

static uint8_t dfp_from_pan(double pan)
{
    int p;
    if (fabs(pan) < 0.01)
        return 0;
    p = (int)floor(32.0 + 32.0 * pan + 0.5);
    if (p < 0) p = 0;
    if (p > 64) p = 64;
    return (uint8_t)(0x80 | p);
}

/* fill sample `s` (header + data) from interleaved 16-bit frames */
static int make_sample(sample_t *s, const int16_t *l, int lstride,
                       const int16_t *r, int rstride, uint32_t len)
{
    int ch = r ? 2 : 1, c;
    uint32_t i;

    s->Data = malloc(smp_alloc_bytes(len, 2, ch));
    if (!s->Data)
        return 0;
    s->Length = len;
    s->Flags = (uint8_t)(1 | 2 | (ch == 2 ? SMP_STEREO : 0));
    for (c = 0; c < ch; c++) {
        int16_t *d = (int16_t *)smp_chan(s, c);
        const int16_t *src = c ? r : l;
        int st = c ? rstride : lstride;
        for (i = 0; i < len; i++)
            d[i] = src[(size_t)i * (size_t)st];
    }
    smp_pad(s);
    return 1;
}

static void set_loop(sample_t *s, int mode, int64_t ls, int64_t le)
{
    if (mode <= 0)
        return;
    if (ls < 0) ls = 0;
    if (le > (int64_t)s->Length) le = s->Length;
    if (le - ls < 1)
        return;
    if (mode == 2) {
        s->Flags |= 0x20;
        s->SusLoopBeg = (uint32_t)ls;
        s->SusLoopEnd = (uint32_t)le;
    } else {
        s->Flags |= 0x10 | (mode == 3 ? 0x40 : 0);
        s->LoopBeg = (uint32_t)ls;
        s->LoopEnd = (uint32_t)le;
    }
}

static void sample_names(sample_t *s, const char *name)
{
    size_t n = strlen(name);
    memcpy(&s->ID, "IMPS", 4);
    memset(s->SampleName, 0, sizeof(s->SampleName));
    memcpy(s->SampleName, name, n > 25 ? 25 : n);
    memset(s->DOSFileName, 0, sizeof(s->DOSFileName));
    memcpy(s->DOSFileName, name, n > 12 ? 12 : n);
}

/* SFZ voice: slice the decoded file */
static int voice_from_audio(sample_t *s, const sfi_region_t *r,
                            const audio_t *a)
{
    int64_t start = r->start, end = r->end;
    int mode = r->loopmode;
    int64_t ls = r->loopstart, le = r->loopend;
    uint32_t len;
    int root;
    char nm[64];

    if (start < 0) start = 0;
    if (end < 0 || end > (int64_t)a->frames) end = (int64_t)a->frames;
    if (end <= start)
        return 0;
    if (end - start > (int64_t)SMP_MAXFRAMES)
        end = start + SMP_MAXFRAMES;
    len = (uint32_t)(end - start);
    if (!make_sample(s, a->pcm + (size_t)start * (size_t)a->ch, a->ch,
                     a->ch >= 2 ? a->pcm + (size_t)start * (size_t)a->ch + 1
                                : NULL, a->ch, len))
        return 0;

    if (mode < 0) {                         /* from the file (R5) */
        mode = a->loopmode;
        ls = a->ls;
        le = a->le;
    } else if (mode > 0) {
        if (ls < 0) ls = a->loopmode ? a->ls : 0;
        if (le < 0) le = a->loopmode ? a->le : (int64_t)a->frames;
    }
    set_loop(s, mode, ls - start, le - start);

    root = r->root >= 0 ? r->root : (a->unity >= 0 ? a->unity : 60);
    s->C5Speed = c5speed(a->rate, root, r->tune);
    s->GvL = gvl_from_db(r->gain_db);
    s->Vol = 64;
    s->DfP = dfp_from_pan(r->pan);
    s->Cvt = 1;
    stem(a->path, nm, sizeof(nm));
    sample_names(s, nm);
    return 1;
}

/* SF2 voice: read the frames (and the linked partner's) from the file */
static int voice_from_sf2(sample_t *s, const sf2_t *sf, FILE *f,
                          const sfi_region_t *r, const sfi_region_t *pr)
{
    int64_t len64 = r->end - r->start;
    uint32_t len;
    int16_t *buf[2] = { NULL, NULL };
    const sfi_region_t *chan[2];
    int c, ok = 0;

    if (len64 <= 0)
        return 0;
    if (pr) {
        int64_t pl = pr->end - pr->start;
        if (pl < len64)
            len64 = pl;
        /* left = the left-type sample (type 4) */
        chan[0] = (r->stype & 4) ? r : pr;
        chan[1] = (r->stype & 4) ? pr : r;
    } else {
        chan[0] = r;
        chan[1] = NULL;
    }
    if (len64 > (int64_t)SMP_MAXFRAMES)
        len64 = SMP_MAXFRAMES;
    len = (uint32_t)len64;
    for (c = 0; c < (pr ? 2 : 1); c++) {
        uint32_t i;
        uint8_t *raw;
        buf[c] = (int16_t *)malloc((size_t)len * 2);
        raw = (uint8_t *)buf[c];
        if (!buf[c] ||
            seek64(f, sf->smpl_off + chan[c]->start * 2) ||
            fread(raw, 2, len, f) != len)
            goto out;
        for (i = 0; i < len; i++)           /* little-endian on disk */
            buf[c][i] = (int16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8));
    }
    if (!make_sample(s, buf[0], 1, pr ? buf[1] : NULL, 1, len))
        goto out;
    set_loop(s, r->loopmode, r->loopstart - r->start, r->loopend - r->start);
    s->C5Speed = c5speed(r->rate, r->root, r->tune);
    s->GvL = gvl_from_db(r->gain_db);
    s->Vol = 64;
    s->DfP = pr ? 0 : dfp_from_pan(r->pan);
    s->Cvt = 1;
    sample_names(s, sf->sh[r->smp].name);
    ok = 1;
out:
    free(buf[0]);
    free(buf[1]);
    return ok;
}

static int ticks(double sec)
{
    double t = sec * TICKS_PER_SEC;
    if (t > 9999.0) t = 9999.0;
    if (t < 0.0) t = 0.0;
    return (int)floor(t + 0.5);
}

typedef struct envb_t { envnode_t *n; int cnt; } envb_t;

static void env_add(envb_t *b, int t, double level)
{
    int m = (int)floor(level * 64.0 + 0.5);
    if (m < 0) m = 0;
    if (m > 64) m = 64;
    if (t > 9999) t = 9999;
    if (b->cnt > 0 && t <= b->n[b->cnt - 1].Tick) {
        b->n[b->cnt - 1].Magnitude = (int8_t)m;     /* zero-length step */
        return;
    }
    if (b->cnt >= 25)
        return;
    b->n[b->cnt].Tick = (uint16_t)t;
    b->n[b->cnt].Magnitude = (int8_t)m;
    b->cnt++;
}

static uint16_t fadeout_for(double release)
{
    double t = release * TICKS_PER_SEC;
    int f;
    if (t < 1.0) t = 1.0;
    f = (int)ceil(1024.0 / t);
    if (f < 1) f = 1;
    if (f > 256) f = 256;
    return (uint16_t)f;
}

/* amplitude envelope -> volume envelope (R8) */
static void build_env(instrument_t *in, const sfi_env_t *e)
{
    static const double xs[4] = { 0.1, 0.3, 0.6, 1.0 };
    envb_t b;
    double S = e->sustain;
    int t, ts, i;

    if (S < 0) S = 0;
    if (S > 1) S = 1;
    in->FadeOut = fadeout_for(e->release);
    if (e->delay < 0.005 && e->attack < 0.005 && S >= 0.999 &&
        e->release < 0.05)
        return;                             /* flat: envelope stays off */

    b.n = in->VEnvelope.NodePoints;
    b.cnt = 0;
    env_add(&b, 0, (e->delay > 0 || e->attack > 0) ? 0.0 : 1.0);
    t = ticks(e->delay);
    if (e->delay > 0)
        env_add(&b, t, 0.0);
    t = ticks(e->delay + e->attack);
    env_add(&b, t, 1.0);
    if (e->hold > 0) {
        t = ticks(e->delay + e->attack + e->hold);
        env_add(&b, t, 1.0);
    }
    if (S < 0.999) {
        for (i = 0; i < 4; i++) {
            double lvl = i == 3 ? S : S + (1.0 - S) * pow(10.0, -3.0 * xs[i]);
            env_add(&b, t + ticks(xs[i] * e->decay), lvl);
        }
    }
    in->VEnvelope.SLB = in->VEnvelope.SLE = (uint8_t)(b.cnt - 1);
    ts = b.n[b.cnt - 1].Tick;
    for (i = 0; i < 4; i++) {
        double lvl = i == 3 ? 0.0 : S * pow(10.0, -3.0 * xs[i]);
        int tt = ts + ticks(xs[i] * e->release);
        if (tt <= ts)
            tt = ts + 1 + i;
        env_add(&b, tt, lvl);
    }
    in->VEnvelope.Num = (uint8_t)b.cnt;
    in->VEnvelope.Flags = 1 | 4;            /* on, sustain loop */
}

static void build_filter(instrument_t *in, double hz, double reso_db)
{
    int c, r;

    if (hz <= 0.0)
        return;
    c = (int)floor(24.0 * log2(hz / 130.81) + 0.5);
    r = (int)floor(reso_db * 128.0 / 24.0 + 0.5);
    if (r < 0) r = 0;
    if (r > 127) r = 127;
    if (c >= 127 && r == 0)
        return;                             /* above IT's range: off */
    if (c < 0) c = 0;
    if (c > 127) c = 127;
    in->IFC = (uint8_t)(0x80 | c);
    if (r > 0)
        in->IFR = (uint8_t)(0x80 | r);
}

static void build_instrument(instrument_t *in, const char *name)
{
    size_t n = strlen(name);
    int i;

    memset(in, 0, sizeof(*in));
    memcpy(&in->ID, "IMPI", 4);
    memcpy(in->InstrumentName, name, n > 25 ? 25 : n);
    memcpy(in->DOSFileName, name, n > 12 ? 12 : n);
    in->GbV = 128;
    in->DfP = 0x80 | 32;
    in->PPC = 60;
    in->MPr = 0xFF;
    in->MIDIBnk = 0xFFFF;
    in->VEnvelope.Num = 2;
    in->VEnvelope.NodePoints[0].Magnitude = 64;
    in->VEnvelope.NodePoints[1].Magnitude = 64;
    in->VEnvelope.NodePoints[1].Tick = 100;
    in->PEnvelope.Num = 2;
    in->PEnvelope.NodePoints[1].Tick = 100;
    in->PtEnvelope.Num = 2;
    in->PtEnvelope.NodePoints[1].Tick = 100;
    for (i = 0; i < 120; i++)
        in->NoteSampleTable[i * 2] = (uint8_t)i;
}

/* =================================================================
 * Public entry points
 * ================================================================= */

static int load_regions(const ilibent_t *e, rlist_t *l, sf2_t *sf,
                        char *name, size_t nn, int *rom)
{
    if (e->Format == RI_FMT_SFZ) {
        stem(e->SrcFile, name, nn);
        return sfz_load_regions(e->SrcFile, l);
    }
    if (e->Format == RI_FMT_SF2PRESET) {
        if (!sf2_open(e->SrcFile, sf))
            return 0;
        if ((int)e->Offset >= sf->nph - 1)
            return 0;
        snprintf(name, nn, "%s", sf->ph[e->Offset].name);
        return sf2_regions(sf, (int)e->Offset, l, rom);
    }
    return 0;
}

int SFI_CountSFZ(const char *path, char name[27])
{
    rlist_t l;
    plan_t *pl;
    int n;

    memset(&l, 0, sizeof(l));
    stem(path, name, 27);
    if (!sfz_load_regions(path, &l)) {
        rl_free(&l);
        return -1;
    }
    pl = (plan_t *)malloc(sizeof(*pl));
    if (!pl) {
        rl_free(&l);
        return -1;
    }
    plan_select(&l, pl);
    n = pl->nv;
    free(pl);
    rl_free(&l);
    return n;
}

static int cmp_preset(const void *a, const void *b)
{
    const ilibent_t *x = (const ilibent_t *)a, *y = (const ilibent_t *)b;
    return strcmp(x->Name, y->Name);    /* "bbb:ppp name" sorts by number */
}

int SFI_ScanSF2(const char *path, ilibent_t *ents, int max)
{
    sf2_t sf;
    int p, cnt = 0;
    plan_t *pl;

    if (!sf2_open(path, &sf))
        return -1;
    pl = (plan_t *)malloc(sizeof(*pl));
    if (!pl) {
        sf2_free(&sf);
        return -1;
    }
    for (p = 0; p < sf.nph - 1 && cnt < max; p++) {
        rlist_t l;
        ilibent_t *e = &ents[cnt];

        memset(&l, 0, sizeof(l));
        memset(e, 0, sizeof(*e));
        sf2_regions(&sf, p, &l, NULL);
        plan_select(&l, pl);
        e->Format = RI_FMT_SF2PRESET;
        snprintf(e->Name, sizeof(e->Name), "%03u:%03u %s",
                 sf.ph[p].bank % 1000, sf.ph[p].preset % 1000, sf.ph[p].name);
        e->NumSamples = (uint16_t)pl->nv;
        e->Offset = (uint32_t)p;
        snprintf(e->SrcFile, sizeof(e->SrcFile), "%s", path);
        rl_free(&l);
        cnt++;
    }
    free(pl);
    sf2_free(&sf);
    qsort(ents, (size_t)cnt, sizeof(*ents), cmp_preset);
    return cnt;
}

int SFI_Build(const ilibent_t *e, int maxsmp, instrument_t *ins,
              sample_t *sh, sfi_report_t *rep)
{
    rlist_t l;
    sf2_t sf;
    plan_t *pl = NULL;
    audio_t *au = NULL;
    FILE *f = NULL;
    char name[64];
    int vi, nos = 0, k, rep_reg = -1, rom = 0;
    int slot[MAXV];

    memset(rep, 0, sizeof(*rep));
    memset(&l, 0, sizeof(l));
    memset(&sf, 0, sizeof(sf));
    name[0] = 0;
    if (maxsmp > 99)
        maxsmp = 99;

    if (!load_regions(e, &l, &sf, name, sizeof(name), &rom)) {
        snprintf(rep->why, sizeof(rep->why), "unreadable instrument file");
        goto fail;
    }
    if (e->Format == RI_FMT_SF2PRESET && sf.major >= 3) {
        snprintf(rep->why, sizeof(rep->why),
                 "SF3 (compressed SoundFont) not supported");
        goto fail;
    }
    pl = (plan_t *)malloc(sizeof(*pl));
    au = (audio_t *)calloc(1, sizeof(*au));
    if (!pl || !au)
        goto fail;
    plan_select(&l, pl);
    /* SFZ: a region whose sample file is missing gives way to the next
     * region on its keys (a missing layer must not hide the instrument
     * under it); each missing file counts once */
    for (k = 0; k < 64 && e->Format == RI_FMT_SFZ; k++) {
        int changed = 0, j;
        for (vi = 0; vi < pl->nv; vi++) {
            sfi_region_t *r = &l.r[pl->v[vi].reg];
            char real[1024];
            if (r->checked)
                continue;
            r->checked = 1;
            if (find_sample(r->path, r->relofs, real, sizeof(real)))
                continue;
            set_why(rep->why, sizeof(rep->why), "missing sample: %.60s",
                    base_name(r->path));
            rep->skipped++;
            for (j = 0; j < l.n; j++)
                if (!strcmp(l.r[j].path, r->path))
                    l.r[j].bad = l.r[j].checked = 1;
            changed = 1;
        }
        if (!changed)
            break;
        plan_select(&l, pl);
    }
    rep->needed = pl->nv;
    if (pl->nv == 0) {
        if (e->Format == RI_FMT_SFZ && SfzMissingInclude[0])
            set_why(rep->why, sizeof(rep->why), "missing include file: %s",
                    SfzMissingInclude);
        if (!rep->why[0])
            snprintf(rep->why, sizeof(rep->why), rom
                     ? "uses the sound card's ROM samples (not in the file)"
                     : "no playable region");
        goto fail;
    }
    if (maxsmp < 1) {
        snprintf(rep->why, sizeof(rep->why), "no free sample slot");
        goto fail;
    }
    plan_reduce(&l, pl, maxsmp);

    if (e->Format == RI_FMT_SF2PRESET) {
        f = fopen(e->SrcFile, "rb");
        if (!f) {
            snprintf(rep->why, sizeof(rep->why), "cannot open file");
            goto fail;
        }
    }

    /* load each kept voice; failures die and their keys move on */
    for (vi = 0; vi < pl->nv; vi++) {
        const voice_t *v = &pl->v[vi];
        const sfi_region_t *r = &l.r[v->reg];
        sample_t *s = &sh[nos];
        int ok;

        slot[vi] = 0;
        if (!v->alive)
            continue;
        memset(s, 0, sizeof(*s));
        if (r->path) {
            ok = (au->pcm && !strcmp(au->path, r->path)) ||
                 decode_file(r->path, r->relofs, au, rep->why,
                             sizeof(rep->why));
            ok = ok && voice_from_audio(s, r, au);
        } else {
            ok = voice_from_sf2(s, &sf, f, r,
                                v->partner >= 0 ? &l.r[v->partner] : NULL);
            if (!ok)
                set_why(rep->why, sizeof(rep->why), "bad sample data: %s",
                        sf.sh[r->smp].name);
        }
        if (!ok) {
            free(s->Data);
            s->Data = NULL;
            pl->v[vi].alive = 0;
            rep->skipped++;
            continue;
        }
        slot[vi] = ++nos;
    }
    if (nos == 0)
        goto fail;
    plan_reassign(&l, pl);

    build_instrument(ins, name);
    for (k = 0; k < 120; k++) {
        int v = pl->keyv[k];
        ins->NoteSampleTable[k * 2 + 1] =
            (uint8_t)(v >= 0 && pl->v[v].alive ? slot[v] : 0);
    }
    if (pl->keyv[60] >= 0 && pl->v[pl->keyv[60]].alive)
        rep_reg = pl->v[pl->keyv[60]].reg;
    for (k = 0; k < 120 && rep_reg < 0; k++)
        if (pl->keyv[k] >= 0 && pl->v[pl->keyv[k]].alive)
            rep_reg = pl->v[pl->keyv[k]].reg;
    build_env(ins, &l.r[rep_reg].env);
    build_filter(ins, l.r[rep_reg].cutoff, l.r[rep_reg].reso);
    ins->NoS = (uint8_t)nos;
    rep->loaded = nos;

    if (f) fclose(f);
    audio_free(au);
    free(au);
    free(pl);
    rl_free(&l);
    sf2_free(&sf);
    return nos;

fail:
    for (k = 0; k < nos; k++) {
        free(sh[k].Data);
        sh[k].Data = NULL;
    }
    if (!rep->why[0])
        snprintf(rep->why, sizeof(rep->why), "no sample could be read");
    if (f) fclose(f);
    if (au) {
        audio_free(au);
        free(au);
    }
    free(pl);
    rl_free(&l);
    sf2_free(&sf);
    return -1;
}

/* =================================================================
 * Batch test: ited --sfi-test <file-or-dir>... (contracts/sfimport-api.md)
 * Every .sfz and every preset of every .sf2 below the path is loaded
 * into instrument 1 of an emptied song. No window, no audio device.
 * ================================================================= */

#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

typedef struct sfi_tally_t { int ok, fail, files; double worst_ms; } sfi_tally_t;

static void bt_reset_song(void)
{
    int i;
    for (i = 0; i < 99; i++) {
        free(Song.Smp[i].Data);
        memset(&Song.Smp[i], 0, sizeof(Song.Smp[i]));
    }
    memset(&Song.Ins[0], 0, sizeof(Song.Ins[0]));
}

static void bt_one(const ilibent_t *e, const char *label, sfi_tally_t *t)
{
    clock_t c0 = clock();
    int r, k, mapped = 0;
    double ms;

    bt_reset_song();
    r = RI_LoadInstrument(e, 0);
    ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    if (ms > t->worst_ms)
        t->worst_ms = ms;
    for (k = 0; k < 120; k++)
        if (Song.Ins[0].NoteSampleTable[k * 2 + 1])
            mapped++;
    if (r == RI_OK && mapped > 0) {
        t->ok++;
        printf("OK   %3d/%3d/%3d %6.0fms %s %s\n", RI_LastReport.needed,
               RI_LastReport.loaded, RI_LastReport.skipped, ms, label,
               RI_LastReport.skipped ? RI_LastReport.why : "");
    } else {
        t->fail++;
        printf("FAIL %3d/%3d/%3d %6.0fms %s %s\n", RI_LastReport.needed,
               RI_LastReport.loaded, RI_LastReport.skipped, ms, label,
               RI_LastReport.why[0] ? RI_LastReport.why : "load failed");
    }
    fflush(stdout);
}

static int bt_has_ext(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext), i;
    if (a < b)
        return 0;
    for (i = 0; i < b; i++)
        if (tolower((unsigned char)n[a - b + i]) != ext[i])
            return 0;
    return 1;
}

static void bt_file(const char *path, sfi_tally_t *t)
{
    static ilibent_t ents[512];
    char label[1200];
    int n, i;

    if (bt_has_ext(path, ".sfz")) {
        t->files++;
        if (RI_IdentifyFile(path, &ents[0]) != RI_FMT_SFZ) {
            t->fail++;
            printf("FAIL   0/  0/  0      0ms %s unreadable\n", path);
            return;
        }
        bt_one(&ents[0], path, t);
    } else if (bt_has_ext(path, ".sf2") || bt_has_ext(path, ".sf3")) {
        t->files++;
        n = RI_ScanModule(path, ents, 512);
        if (n <= 0) {
            t->fail++;
            printf("FAIL   0/  0/  0      0ms %s no presets\n", path);
            return;
        }
        for (i = 0; i < n; i++) {
            snprintf(label, sizeof(label), "%s#%s", path, ents[i].Name);
            bt_one(&ents[i], label, t);
        }
    }
}

static void bt_walk(const char *path, sfi_tally_t *t, int depth)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[1200];
    DWORD at = GetFileAttributesA(path);

    if (at == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(at & FILE_ATTRIBUTE_DIRECTORY)) {
        bt_file(path, t);
        return;
    }
    if (depth > 12)
        return;
    snprintf(pat, sizeof(pat), "%s/*", path);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char sub[1200];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, fd.cFileName);
        bt_walk(sub, t, depth + 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    struct stat st;
    DIR *d;
    struct dirent *de;

    if (stat(path, &st))
        return;
    if (!S_ISDIR(st.st_mode)) {
        bt_file(path, t);
        return;
    }
    if (depth > 12 || !(d = opendir(path)))
        return;
    while ((de = readdir(d))) {
        char sub[1200];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        bt_walk(sub, t, depth + 1);
    }
    closedir(d);
#endif
}

int SFI_BatchTest(int argc, const char *const *paths)
{
    sfi_tally_t t;
    int i;

    memset(&t, 0, sizeof(t));
    for (i = 0; i < argc; i++)
        bt_walk(paths[i], &t, 0);
    bt_reset_song();
    printf("SUMMARY files %d  items %d  OK %d  FAIL %d  worst %.0f ms\n",
           t.files, t.ok + t.fail, t.ok, t.fail, t.worst_ms);
    return 0;
}
