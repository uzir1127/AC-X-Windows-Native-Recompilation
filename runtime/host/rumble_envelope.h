#ifndef ACX_RUMBLE_ENVELOPE_H
#define ACX_RUMBLE_ENVELOPE_H
#include <stdint.h>
#include <string.h>
/* Wall-time envelopes, independent of render rate and the game's 30/60 mode.
 * Mix per-effect maxima so gun fire cannot shorten a heavy damage pulse. */
typedef struct { uint64_t start, end; unsigned low, high, hold; } RumblePulse;
typedef struct { RumblePulse pulse[4]; } RumbleEnvelope;
static inline void rumble_envelope_clear(RumbleEnvelope *e) { memset(e, 0, sizeof *e); }
static inline void rumble_envelope_event(RumbleEnvelope *e, unsigned mask,
                                         unsigned damage, uint64_t now)
{
    static const unsigned low[4] = {48000, 65535, 65535, 60000};
    static const unsigned high[4] = {60000, 56000, 65535, 62000};
    static const unsigned ms[4] = {140, 320, 420, 650};
    static const unsigned hold[4] = {65, 130, 170, 230};
    for (unsigned i = 0; i < 4; ++i) if (mask & (1u << i)) {
        unsigned l = low[i], h = high[i];
        if (i == 3) {
            unsigned boost = damage > 100 ? 100 : damage;
            l += boost * 90; h += boost * 90;
            if (l > 65535) l = 65535;
            if (h > 65535) h = 65535;
        }
        e->pulse[i] = (RumblePulse){now, now + ms[i], l, h, hold[i]};
    }
}
static inline void rumble_envelope_sample(const RumbleEnvelope *e, uint64_t now,
                                          int strength, uint16_t *low, uint16_t *high)
{
    unsigned l = 0, h = 0;
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;
    for (unsigned i = 0; i < 4; ++i) {
        const RumblePulse *p = &e->pulse[i];
        if (now < p->start || now >= p->end) continue;
        uint64_t remain = p->end - now, duration = p->end - p->start - p->hold;
        if (remain > duration) remain = duration;
        unsigned a = (unsigned)(p->low * remain / duration);
        unsigned b = (unsigned)(p->high * remain / duration);
        if (a > l) l = a;
        if (b > h) h = b;
    }
    *low = (uint16_t)(l * (unsigned)strength / 100);
    *high = (uint16_t)(h * (unsigned)strength / 100);
}
/* Sustained 10 Hz buffet. Low motor follows the requested airspeed strength;
 * callers mix it with weapon/impact envelopes before applying user strength. */
static inline void rumble_envelope_stall(uint64_t now, int strength, unsigned level,
                                         uint16_t *low, uint16_t *high)
{
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;
    unsigned phase = (unsigned)(now % 100);
    unsigned triangle = phase < 50 ? phase : 100 - phase;
    if(level>10000)level=10000;
    unsigned l = (unsigned)(UINT64_C(65535)*level*(unsigned)strength/1000000u);
    unsigned h = (unsigned)((uint64_t)l*(850u+triangle*3u)/1000u);
    if (l > *low) *low = (uint16_t)l;
    if (h > *high) *high = (uint16_t)h;
}
#endif
