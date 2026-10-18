/*
 * it_sample.h
 * -----------
 * Feature 019: stereo samples. IT's samples are mono; the IT format's
 * sample flag 0x04 ("stereo", as OpenMPT and Schism Tracker write it)
 * marks a sample with a left and a right channel of Length frames each.
 *
 * In memory a sample is one allocation:
 *   mono    [ channel 0: Length frames ][ padding ]
 *   stereo  [ left: Length frames ][ 4 frames pad ][ right: Length frames ][ pad ]
 * The padding after each channel keeps the cubic interpolator (it reads
 * pos+1 and pos+2) inside its channel. Mono samples are laid out as
 * always, so everything written for mono keeps working on channel 0.
 */
#ifndef IT_SAMPLE_H
#define IT_SAMPLE_H

#include <stddef.h>
#include <stdint.h>
#include "it_structs.h"

#define SMP_STEREO    0x04      /* sample flag: two channels             */
#define SMP_PADFRAMES 4         /* frames of padding after each channel  */
/* it26: the longest sample, in frames per channel -- OpenMPT's and
 * Schism Tracker's MAX_SAMPLE_LENGTH, so every sample it26 keeps opens
 * untruncated there and theirs load here (IT capped at ~4 MB) */
#define SMP_MAXFRAMES 0x10000000u

static inline int smp_channels(const sample_t *s)
{
    return (s->Flags & SMP_STEREO) ? 2 : 1;
}

static inline int smp_width(const sample_t *s)       /* bytes per value */
{
    return (s->Flags & 2) ? 2 : 1;
}

/* the start of channel c (0 = left / mono, 1 = right) */
static inline void *smp_chan(const sample_t *s, int c)
{
    return (uint8_t *)s->Data +
           (size_t)c * ((size_t)s->Length + SMP_PADFRAMES) *
           (size_t)smp_width(s);
}

/* channel c of a buffer laid out for `frames` frames of `width` bytes */
static inline void *smp_chan_at(void *base, uint32_t frames, int width,
                                int c)
{
    return (uint8_t *)base +
           (size_t)c * ((size_t)frames + SMP_PADFRAMES) * (size_t)width;
}

/* audio bytes, all channels, without padding (what a file holds) */
static inline size_t smp_data_bytes(const sample_t *s)
{
    return (size_t)s->Length * (size_t)smp_width(s) *
           (size_t)smp_channels(s);
}

/* bytes to allocate for `frames` frames of `width`-byte values in
 * `channels` channels, padding included */
static inline size_t smp_alloc_bytes(uint32_t frames, int width,
                                     int channels)
{
    return (size_t)channels * ((size_t)frames + SMP_PADFRAMES) *
           (size_t)width;
}

/* the interpolator padding after each channel: the last value twice,
 * then zeros (as the loader has always padded mono samples) */
static inline void smp_pad(sample_t *s)
{
    int c, w = smp_width(s);
    uint32_t n = s->Length;

    if (!s->Data)
        return;
    for (c = 0; c < smp_channels(s); c++) {
        if (w == 2) {
            int16_t *p = (int16_t *)smp_chan(s, c);
            p[n] = p[n + 1] = n ? p[n - 1] : 0;
            p[n + 2] = p[n + 3] = 0;
        } else {
            int8_t *p = (int8_t *)smp_chan(s, c);
            p[n] = p[n + 1] = n ? p[n - 1] : 0;
            p[n + 2] = p[n + 3] = 0;
        }
    }
}

#endif
