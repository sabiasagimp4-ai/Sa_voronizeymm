/*
    SaMath.h -- scalar helpers shared by every Sa_* effect's pure-math header.

    Deliberately free of any After Effects SDK dependency so that the per-effect
    Sa_*Math.h headers (which include this one) can be compiled and unit-tested
    offline with a bare `cl /EHsc test_math.cpp`.

    Everything here is header-only, `static inline`, and operates on float.
*/

#ifndef SA_MATH_H
#define SA_MATH_H

#include <math.h>
#include <stddef.h>     /* size_t, for the plane helpers below */

#ifndef SA_PI
#define SA_PI       3.14159265358979f
#endif
#ifndef SA_TWO_PI
#define SA_TWO_PI   6.28318530717959f
#endif

/* ---------------- clamping / interpolation ---------------- */

static inline float sa_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float sa_clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline int sa_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float sa_lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* Smoothstep with the edges pre-ordered by the caller (e0 < e1). */
static inline float sa_smoothstep(float e0, float e1, float x)
{
    const float d = e1 - e0;
    if (d == 0.0f) return x < e0 ? 0.0f : 1.0f;
    const float t = sa_clamp01((x - e0) / d);
    return t * t * (3.0f - 2.0f * t);
}

/* ---------------- wrapping ---------------- */

/* GLSL mod(): result carries the sign of `m`, so it is always in [0,m) for m>0.
   fmodf() alone would return negatives for negative x. */
static inline float sa_mod(float x, float m)
{
    if (m == 0.0f) return 0.0f;
    const float r = fmodf(x, m);
    return (r < 0.0f) != (m < 0.0f) && r != 0.0f ? r + m : r;
}

/* Non-negative integer modulo (C's % gives a negative result for negative a). */
static inline int sa_modi(int a, int m)
{
    if (m <= 0) return 0;
    const int r = a % m;
    return r < 0 ? r + m : r;
}

/* Mirror-fold an index into [0, n-1]: 0,1,..,n-1,n-1,..,1,0,0,1,.. */
static inline int sa_mirrori(int a, int n)
{
    if (n <= 1) return 0;
    const int period = 2 * n;
    int r = sa_modi(a, period);
    return r < n ? r : period - 1 - r;
}

/* ---------------- scalar planes ---------------- */

/*  The half-open span [*lo, *hi) of x in [0, W) for which x + d is still a
    valid index for every d in [-m, +m]. Inside it a clamped neighbourhood
    read cannot clamp, so the caller can hoist the bounds test out of the loop
    entirely -- which is both the speed win and the reason it is bit-identical
    (inside the span the clamps would not have fired).

    Both ends are clipped to [0, W], so a plane narrower than the neighbourhood
    leaves the span EMPTY rather than inverted. Getting that wrong gives a
    backwards loop that either does nothing or runs off the buffer, and it only
    shows up on the small frames nobody renders -- so it lives here once
    instead of being re-derived at each call site. */
static inline void sa_interior_span(int W, int m, int* lo, int* hi)
{
    int a = m, b = W - m;
    if (a > W) a = W;
    if (b < a) b = a;
    *lo = a; *hi = b;
}

/*  The three rows a 3x3 neighbourhood at row y touches, edge-clamped.

    A tap that moves vertically selects a whole source ROW, so the vertical
    clamp resolves once per row rather than once per pixel. Resolving it here
    turns the eight surrounding pixels into eight constant offsets from three
    pointers, which is what lets the inner loop vectorise. */
typedef struct SaRows3 {
    const float* m;     /* y - 1, clamped */
    const float* c;     /* y              */
    const float* p;     /* y + 1, clamped */
} SaRows3;

static inline SaRows3 sa_rows3(const float* plane, int W, int H, int y)
{
    SaRows3 r;
    r.m = plane + (size_t)sa_clampi(y - 1, 0, H - 1) * (size_t)W;
    r.c = plane + (size_t)y * (size_t)W;
    r.p = plane + (size_t)sa_clampi(y + 1, 0, H - 1) * (size_t)W;
    return r;
}

/* ---------------- colour ---------------- */

/* Rec.709 luma, the same weights AE's own effects use. */
static inline float sa_luminance(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/*  After Effects hands effects PREMULTIPLIED pixels. Any operation on colour
    rather than on coverage -- a false-colour lookup, a posterise, a hue shift --
    has to undo that first, or every semi-transparent pixel is treated as a
    darker colour than it is and the effect darkens towards the edges of soft
    mattes.

    At alpha 0 the colour is unrecoverable (0/0), and the standard answer is the
    only safe one: leave it at zero. Nothing downstream can see it, because it
    is multiplied by 0 again on the way back.

    The threshold is deliberately tiny rather than a "reasonable" epsilon: alpha
    1/255 is a real, visible pixel and must survive, so only true zero (and
    denormal noise at 32 bpc) is special-cased. */
/*  Both of these test for exactly 1.0 first, which is not an approximation:
    x * 1.0f and x / 1.0f return x with every bit intact, so the early-out
    returns what the arithmetic below would have returned anyway. What it buys
    is the DIVIDE -- around fourteen cycles of latency that most footage never
    needed, since opaque is the common case and every effect in the pack calls
    these once or twice per pixel.

    The test fires exactly when the pixel is opaque and never by accident. AE
    reconstructs alpha as level * (1/max), and 255 * (1/255) and
    32768 * (1/32768) are both exactly 1.0f, while no other level of either
    depth lands on 1.0f -- checked over all 255 and all 32768 of them, not
    assumed. At 32 bpc opaque is literally 1.0f. */
static inline void sa_unpremul(float* rgba)
{
    const float a = rgba[3];
    if (a == 1.0f) return;
    if (a <= 1e-6f) { rgba[0] = rgba[1] = rgba[2] = 0.0f; return; }
    const float k = 1.0f / a;
    rgba[0] *= k; rgba[1] *= k; rgba[2] *= k;
}

static inline void sa_premul(float* rgba)
{
    const float a = rgba[3];
    if (a == 1.0f) return;
    rgba[0] *= a; rgba[1] *= a; rgba[2] *= a;
}

/* ---------------- deterministic hashing ---------------- */

/* 32-bit integer avalanche (Wang hash). Deterministic across runs and machines,
   which matters because these effects must render identically on every frame
   and on every thread band. */
static inline unsigned int sa_hash_u32(unsigned int x)
{
    x = (x ^ 61u) ^ (x >> 16);
    x = x + (x << 3);
    x = x ^ (x >> 4);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15);
    return x;
}

static inline unsigned int sa_hash2(int x, int y)
{
    return sa_hash_u32((unsigned int)x * 0x9e3779b9u ^ sa_hash_u32((unsigned int)y));
}

/* Uniform float in [0,1) from an integer lattice point. */
static inline float sa_rand2(int x, int y)
{
    return (float)(sa_hash2(x, y) >> 8) * (1.0f / 16777216.0f);
}

static inline float sa_rand3(int x, int y, int z)
{
    return (float)(sa_hash_u32(sa_hash2(x, y) ^ sa_hash_u32((unsigned int)z * 0x85ebca6bu)) >> 8)
           * (1.0f / 16777216.0f);
}

/* ---------------- 2x2 affine ---------------- */

typedef struct SaMat2 { float a, b, c, d; } SaMat2;   /* [a b; c d], row-major */

static inline SaMat2 sa_mat2_rot_scale(float radians, float sx, float sy)
{
    const float cs = cosf(radians), sn = sinf(radians);
    SaMat2 m;
    m.a =  cs * sx; m.b = -sn * sy;
    m.c =  sn * sx; m.d =  cs * sy;
    return m;
}

/* Inverse of a 2x2. Returns 0 (and leaves *out identity) when singular, so
   callers can fall back instead of producing NaNs. */
static inline int sa_mat2_invert(SaMat2 m, SaMat2* out)
{
    const float det = m.a * m.d - m.b * m.c;
    if (det > -1e-12f && det < 1e-12f) {
        out->a = 1.0f; out->b = 0.0f; out->c = 0.0f; out->d = 1.0f;
        return 0;
    }
    const float inv = 1.0f / det;
    out->a =  m.d * inv; out->b = -m.b * inv;
    out->c = -m.c * inv; out->d =  m.a * inv;
    return 1;
}

static inline void sa_mat2_apply(SaMat2 m, float x, float y, float* ox, float* oy)
{
    *ox = m.a * x + m.b * y;
    *oy = m.c * x + m.d * y;
}

#endif /* SA_MATH_H */
