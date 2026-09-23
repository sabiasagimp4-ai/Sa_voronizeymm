/*
    Sa_voronizeMath.h -- jittered-grid Voronoi cells, plus image-adaptive
    density and the pure formulas behind the effect's dropdown modes.

    No After Effects SDK dependency; exercised by test/test_math.cpp.

    The grid, site placement and 3x3 nearest-site search are the same proven
    scheme as Sa_voronoi (see Sa_voronoiMath.h for the correctness argument:
    jitter capped at half a cell keeps the 3x3 search exact, and the worst-case
    reach from a query pixel to its winning site is size*sqrt(2)).

    What is new here over Sa_voronoi
    ---------------------------------
    Image-adaptive density ("Detail" / "Luminance" / "Darkness" mode) does NOT
    add per-site existence coin-flips the way the browser prototype in
    tweak_voronoi_browser_prototype/ does. A per-site coin flip makes the
    search radius needed for a correct nearest-site answer unbounded in the
    worst case (a long unlucky run of misses), which is exactly the kind of
    claim this pack insists on proving rather than assuming (see the "Margins"
    note in Sa_voronize.cpp).

    Density does add per-CANDIDATE existence coin-flips now (Sa_voronize.cpp's
    scatter gather), but bounded, not unbounded like the prototype's per-site
    flip: every ANALYSIS BLOCK (see SA_VZ_DENSITY_BLOCK_MULT) is subdivided
    into SA_VZ_SCATTER_SUBDIV x SA_VZ_SCATTER_SUBDIV candidate subcells, each
    surviving independently with the probability sa_voronize_density_probability
    gives its own bilinearly-interpolated analysis value -- so brighter (or
    however the mode reads "dense") areas keep more of their subcells and
    genuinely scatter more seed points, not just shrink a shared cell size.
    The bound comes from never letting a whole analysis block go empty: if
    every one of its subcells is rejected, the single highest-probability
    subcell is kept anyway. That guarantees at least one seed inside every
    block-sized square exactly like the old single-multiplier scheme did, so
    the SAME worst-case reach argument applies with "block" standing in for
    "cell": every surviving (or forced-fallback) seed is confined to its own
    block, so the nearest one to any query point is within one block step and
    a border-relevant one within two -- sa_voronize_edge_from_sites is handed
    a 5x5-block gather (SUBDIV^2 candidates per block) instead of the fixed
    25-site hash lattice, but the bisector maths is unchanged. The PreRender
    checkout margin for this path is 3.5 * SA_VZ_DENSITY_BLOCK_MULT * size
    (not 1.5x, unlike the Uniform-mode margin) -- see the "Margins" note in
    Sa_voronize.cpp for why the block-gather-plus-bilinear-lookup chain reaches
    further than a single block step.
*/

#ifndef SA_VORONIZE_MATH_H
#define SA_VORONIZE_MATH_H

#include "SaMath.h"
#include <float.h>
#include <string.h>

#define SA_VZ_SIZE_MIN      1.0f
#define SA_VZ_SIZE_DFLT     24.0f
#define SA_VZ_MIN_DENSITY_MIN  0.0f

/* Half a cell. Past this the 3x3 search stops being exact. Expressed as a
   fraction of the cell size. */
#define SA_VZ_JITTER_MAX    0.5f
#define SA_VZ_JITTER_DFLT   0.45f

/* Hard cap on the density-adaptive cell-size multiplier. Fixed, not derived
   from the live Min Density / Density Strength parameters, so PreRender can
   size the checkout margin from this one constant regardless of how the user
   has those sliders set -- see the note in Sa_voronize.cpp. */
#define SA_VZ_DENSITY_BLOCK_MULT   4.0f

/* Candidate subcells per side of an analysis block for the scatter search
   (see the header note above). Equal to SA_VZ_DENSITY_BLOCK_MULT so that at
   probability 1 (Density Strength 0, or an analysis value the mode reads as
   maximally "dense") every subcell survives and the scatter reduces to an
   ordinary jittered grid at exactly the base Cell Size -- the same look as
   Uniform mode, just reached through the scatter path instead of bypassing
   it. Not derived from the live sliders for the same PreRender-margin reason
   SA_VZ_DENSITY_BLOCK_MULT itself is fixed. */
#define SA_VZ_SCATTER_SUBDIV   4

/* Upper bound on scatter candidates gathered for one query: a 5x5 block
   neighbourhood (see the header note above for why 5x5, not 3x3), each block
   contributing at most SA_VZ_SCATTER_SUBDIV^2 accepted subcells. */
#define SA_VZ_SCATTER_MAXC     (25 * SA_VZ_SCATTER_SUBDIV * SA_VZ_SCATTER_SUBDIV)

/* Fixed spatial index for the adaptive path's 5x5 analysis-block gather.
   Each block contains SCATTER_SUBDIV^2 subcells, so the gathered window is a
   20x20 bin grid at the current constants. A bin contains at most one kept
   candidate; a block with no kept candidate stores its single fallback in
   the fallback candidate's own bin. */
#define SA_VZ_SCATTER_GRID_SIDE  (5 * SA_VZ_SCATTER_SUBDIV)
#define SA_VZ_SCATTER_GRID_BINS  (SA_VZ_SCATTER_GRID_SIDE * SA_VZ_SCATTER_GRID_SIDE)

typedef struct SaVoronizeScatterTile {
    float sx[SA_VZ_SCATTER_MAXC];
    float sy[SA_VZ_SCATTER_MAXC];
    int   bin_to_candidate[SA_VZ_SCATTER_GRID_BINS];
    int   count;
    int   origin_gx;
    int   origin_gy;
    float cell;
} SaVoronizeScatterTile;

static inline void sa_voronize_scatter_tile_init(SaVoronizeScatterTile* tile,
                                                 int query_bx, int query_by,
                                                 float cell)
{
    for (int i = 0; i < SA_VZ_SCATTER_GRID_BINS; ++i)
        tile->bin_to_candidate[i] = -1;
    tile->count = 0;
    tile->origin_gx = (query_bx - 2) * SA_VZ_SCATTER_SUBDIV;
    tile->origin_gy = (query_by - 2) * SA_VZ_SCATTER_SUBDIV;
    tile->cell = cell;
}

static inline void sa_voronize_scatter_tile_add(SaVoronizeScatterTile* tile,
                                                int gx, int gy,
                                                float sx, float sy)
{
    const int local_x = gx - tile->origin_gx;
    const int local_y = gy - tile->origin_gy;
    if (local_x < 0 || local_x >= SA_VZ_SCATTER_GRID_SIDE ||
        local_y < 0 || local_y >= SA_VZ_SCATTER_GRID_SIDE ||
        tile->count >= SA_VZ_SCATTER_MAXC)
        return;

    const int k = tile->count++;
    tile->sx[k] = sx;
    tile->sy[k] = sy;
    tile->bin_to_candidate[local_y * SA_VZ_SCATTER_GRID_SIDE + local_x] = k;
}

typedef struct SaVoronize {
    float size;         /* grid pitch in pixels */
    float inv_size;
    float jitter;        /* 0 = regular grid, 0.5 = site anywhere in its square */
    int   seed;
} SaVoronize;

#define SA_VZ_UNIFORM_TILE_SIDE   5
#define SA_VZ_UNIFORM_TILE_SITES  (SA_VZ_UNIFORM_TILE_SIDE * SA_VZ_UNIFORM_TILE_SIDE)

/* The fixed 5x5 site neighbourhood for one Uniform query cell. Site order is
   deliberately the same row-major order sa_voronize_edge used to generate
   per pixel, because first-in-array wins exact-distance ties. */
typedef struct SaVoronizeUniformTile {
    float sx[SA_VZ_UNIFORM_TILE_SITES];
    float sy[SA_VZ_UNIFORM_TILE_SITES];
    int   origin_x;
    int   origin_y;
    float cell;
} SaVoronizeUniformTile;

/* Independent-size Uniform mode evaluates nearest sites in normalized grid
   space and preserves physical coordinates only for sampling and distances. */
typedef struct SaVoronizeMetric {
    float size_x, size_y;
    float inv_size_x, inv_size_y;
    float jitter;
    int   seed;
} SaVoronizeMetric;

typedef struct SaVoronizeMetricUniformTile {
    float ux[SA_VZ_UNIFORM_TILE_SITES];
    float uy[SA_VZ_UNIFORM_TILE_SITES];
    float px[SA_VZ_UNIFORM_TILE_SITES];
    float py[SA_VZ_UNIFORM_TILE_SITES];
    int origin_x, origin_y;
} SaVoronizeMetricUniformTile;

/* Scatter uses the same candidate order as the scalar tile, while retaining
   both coordinate systems: normalized coordinates choose cells; physical
   coordinates feed sampling and final pixel distances. */
typedef struct SaVoronizeMetricScatterTile {
    float ux[SA_VZ_SCATTER_MAXC], uy[SA_VZ_SCATTER_MAXC];
    float px[SA_VZ_SCATTER_MAXC], py[SA_VZ_SCATTER_MAXC];
    int bin_to_candidate[SA_VZ_SCATTER_GRID_BINS];
    int count, origin_gx, origin_gy;
    float cell_x, cell_y;
} SaVoronizeMetricScatterTile;

typedef struct SaVoronizeUniformColorTile {
    float rgba[SA_VZ_UNIFORM_TILE_SITES][4];
} SaVoronizeUniformColorTile;

static inline float sa_voronize_safe_axis(float value)
{
    if (!(value >= SA_VZ_SIZE_MIN) || !(value <= FLT_MAX))
        return SA_VZ_SIZE_MIN;
    return value;
}

static inline float sa_voronize_clamp_render_axis(float value,
                                                  float maximum)
{
    value = sa_voronize_safe_axis(value);
    maximum = sa_voronize_safe_axis(maximum);
    return value > maximum ? maximum : value;
}

static inline void sa_voronize_resolve_cell_size(float size,
                                                 int independent_size,
                                                 float width, float height,
                                                 float* size_x, float* size_y)
{
    *size_x = sa_voronize_safe_axis(independent_size ? width : size);
    *size_y = sa_voronize_safe_axis(independent_size ? height : size);
}

static inline int sa_voronize_use_metric_path(int independent_size,
                                              float /*size_x*/, float /*size_y*/)
{
    return independent_size != 0;
}

static inline void sa_voronize_init(SaVoronize* v, float size, float jitter, int seed)
{
    if (!(size >= SA_VZ_SIZE_MIN)) size = SA_VZ_SIZE_MIN;   /* also catches NaN */
    v->size     = size;
    v->inv_size = 1.0f / size;
    v->jitter   = sa_clampf(jitter, 0.0f, SA_VZ_JITTER_MAX);
    v->seed     = seed;
}

static inline void sa_voronize_metric_init(SaVoronizeMetric* v,
                                           float size_x, float size_y,
                                           float jitter, int seed)
{
    v->size_x = sa_voronize_safe_axis(size_x);
    v->size_y = sa_voronize_safe_axis(size_y);
    v->inv_size_x = 1.0f / v->size_x;
    v->inv_size_y = 1.0f / v->size_y;
    v->jitter = sa_clampf(jitter, 0.0f, SA_VZ_JITTER_MAX);
    v->seed = seed;
}

/*  Position of the site owned by grid cell (ix, iy), in pixels.

    Hashed, not stored: it has to be identical on every thread and every frame,
    and hashing the cell index is both of those without any state. */
static inline void sa_voronize_site(const SaVoronize* v, int ix, int iy,
                                    float* sx, float* sy)
{
    const float jx = sa_rand3(ix, iy, v->seed)       - 0.5f;   /* -0.5 .. 0.5 */
    const float jy = sa_rand3(ix, iy, v->seed + 977) - 0.5f;
    *sx = ((float)ix + 0.5f + jx * (v->jitter * 2.0f)) * v->size;
    *sy = ((float)iy + 0.5f + jy * (v->jitter * 2.0f)) * v->size;
}

static inline void sa_voronize_uniform_tile_init(SaVoronizeUniformTile* tile,
                                                 const SaVoronize* v,
                                                 int query_x, int query_y)
{
    tile->origin_x = query_x - 2;
    tile->origin_y = query_y - 2;
    tile->cell = v->size;
    int k = 0;
    for (int j = 0; j < SA_VZ_UNIFORM_TILE_SIDE; ++j)
        for (int i = 0; i < SA_VZ_UNIFORM_TILE_SIDE; ++i, ++k)
            sa_voronize_site(v, tile->origin_x + i, tile->origin_y + j,
                             &tile->sx[k], &tile->sy[k]);
}

static inline void sa_voronize_metric_uniform_tile_init(
    SaVoronizeMetricUniformTile* tile, const SaVoronizeMetric* v,
    int query_x, int query_y)
{
    tile->origin_x = query_x - 2;
    tile->origin_y = query_y - 2;
    int k = 0;
    for (int j = 0; j < SA_VZ_UNIFORM_TILE_SIDE; ++j) {
        for (int i = 0; i < SA_VZ_UNIFORM_TILE_SIDE; ++i, ++k) {
            const int ix = tile->origin_x + i, iy = tile->origin_y + j;
            const float jx = (sa_rand3(ix, iy, v->seed) - 0.5f) * 2.0f * v->jitter;
            const float jy = (sa_rand3(ix, iy, v->seed + 977) - 0.5f) * 2.0f * v->jitter;
            tile->ux[k] = (float)ix + 0.5f + jx;
            tile->uy[k] = (float)iy + 0.5f + jy;
            tile->px[k] = tile->ux[k] * v->size_x;
            tile->py[k] = tile->uy[k] * v->size_y;
        }
    }
}

static inline void sa_voronize_metric_scatter_tile_init(
    SaVoronizeMetricScatterTile* tile, int query_bx, int query_by,
    float cell_x, float cell_y)
{
    for (int i = 0; i < SA_VZ_SCATTER_GRID_BINS; ++i)
        tile->bin_to_candidate[i] = -1;
    tile->count = 0;
    tile->origin_gx = (query_bx - 2) * SA_VZ_SCATTER_SUBDIV;
    tile->origin_gy = (query_by - 2) * SA_VZ_SCATTER_SUBDIV;
    tile->cell_x = sa_voronize_safe_axis(cell_x);
    tile->cell_y = sa_voronize_safe_axis(cell_y);
}

/* `ux`/`uy` are candidate positions in scatter-subcell units.  The add order
   remains caller-owned row-major order, exactly like the legacy scatter tile. */
static inline void sa_voronize_metric_scatter_tile_add(
    SaVoronizeMetricScatterTile* tile, int gx, int gy, float ux, float uy)
{
    const int local_x = gx - tile->origin_gx;
    const int local_y = gy - tile->origin_gy;
    if (local_x < 0 || local_x >= SA_VZ_SCATTER_GRID_SIDE ||
        local_y < 0 || local_y >= SA_VZ_SCATTER_GRID_SIDE ||
        tile->count >= SA_VZ_SCATTER_MAXC)
        return;

    const int k = tile->count++;
    tile->ux[k] = ux;
    tile->uy[k] = uy;
    tile->px[k] = ux * tile->cell_x;
    tile->py[k] = uy * tile->cell_y;
    tile->bin_to_candidate[local_y * SA_VZ_SCATTER_GRID_SIDE + local_x] = k;
}

template <class SampleSite>
static inline void sa_voronize_uniform_color_tile_fill(
    SaVoronizeUniformColorTile* colours,
    const SaVoronizeUniformTile* tile,
    const SampleSite& sample_site)
{
    for (int k = 0; k < SA_VZ_UNIFORM_TILE_SITES; ++k)
        sample_site(tile->sx[k], tile->sy[k], colours->rgba[k]);
}

static inline void sa_voronize_uniform_color_tile_copy(
    const SaVoronizeUniformColorTile* colours, int index, float* rgba)
{
    memcpy(rgba, colours->rgba[index], sizeof(colours->rgba[index]));
}

/*  Nearest and second-nearest site to (px, py). See Sa_voronoiMath.h for why
    the 3x3 neighbourhood is exact under the jitter cap above. */
static void sa_voronize_nearest(const SaVoronize* v, float px, float py,
                                int* ix, int* iy, float* sx, float* sy,
                                float* d1, float* d2)
{
    const int cx = (int)floorf(px * v->inv_size);
    const int cy = (int)floorf(py * v->inv_size);

    float best = 1e30f, second = 1e30f;
    int   bx = cx, by = cy;
    float bsx = px, bsy = py;

    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const int gx = cx + i, gy = cy + j;
            float qx, qy;
            sa_voronize_site(v, gx, gy, &qx, &qy);
            const float dx = qx - px, dy = qy - py;
            const float d  = dx * dx + dy * dy;
            if (d < best) {
                second = best;
                best = d; bx = gx; by = gy; bsx = qx; bsy = qy;
            } else if (d < second) {
                second = d;
            }
        }
    }

    if (ix) *ix = bx;
    if (iy) *iy = by;
    if (sx) *sx = bsx;
    if (sy) *sy = bsy;
    if (d1) *d1 = sqrtf(best);
    if (d2) *d2 = sqrtf(second);
}

/*  True distance from (px, py) to the nearest edge of its own Voronoi cell,
    via Inigo Quilez's perpendicular-bisector construction
    (https://iquilezles.org/articles/voronoilines/), NOT the cheaper d2-d1
    (second-nearest minus nearest) shortcut sa_voronize_nearest also offers.

    d2-d1 is only exact away from a cell VERTEX. Near a vertex where three or
    more cells meet, the identity of the "second nearest" site changes as you
    sweep past the vertex, and during that hand-off d2-d1 can noticeably
    overestimate the true distance to the edges that are still visually right
    there -- rendered as a border that thins to a point and vanishes at sharp
    cell corners instead of staying a constant width up to the vertex.

    This instead takes the minimum, over every OTHER site that could share an
    edge with the nearest one, of the perpendicular distance from (px, py) to
    the bisector plane between the nearest site and that other site. A
    bisector distance is correct along the ENTIRE length of the edge it
    belongs to, including right up to a vertex (where multiple bisectors
    simultaneously go to zero, which is the geometrically correct answer: a
    vertex is exactly where several edges meet).

    Searched over a 5x5 neighbourhood of the query point's own cell, wider
    than sa_voronize_nearest's 3x3: with jitter capped at 0.5, the winning
    site's own cell can already sit one grid step away from the query's cell
    (see sa_voronize_nearest's proof), and that same one-step margin applies
    again to reach ITS Voronoi neighbours, so a site up to two grid steps from
    the query's cell can still define a relevant bisector. 5x5 gives headroom
    over that two-step bound and is checked against an exhaustive 9x9 search
    in test_math.cpp. Every site here is a pure hash (sa_voronize_site reads
    no pixels), so unlike the density-adaptive block sampling, widening this
    search does not enlarge the PreRender checkout margin. */
/*  Same bisector search as sa_voronize_edge, but over a caller-supplied list
    of candidate site positions instead of the 5x5 hash lattice -- shared by
    sa_voronize_edge (25 fixed hash sites) and the image-adaptive scatter
    search in Sa_voronize.cpp (a variable-length list of accepted/fallback
    subcell seeds, all still confined to a bounded neighbourhood -- see the
    "Scatter density" note below for why the same bisector maths applies to
    either kind of list unchanged).

    `out_nx`/`out_ny` (optional) receive the position of the OTHER site that
    defines the winning (nearest) border -- i.e. the neighbour on the far
    side of the cell edge closest to (px, py). Callers use this for the
    outline's island-masking (see finish_pixel's draw_edge block); the
    cell-seam antialiasing that used to also read it here was replaced by
    sa_voronize_corner_smooth, a whole-frame pass that doesn't need it. */
static inline void sa_voronize_edge_from_sites(const float* sxs, const float* sys, int n,
                                               float px, float py,
                                               float* out_d1, float* out_border,
                                               float* out_nx, float* out_ny,
                                               float* out_sx, float* out_sy)
{
    int   bi = 0;
    float best = 1e30f;
    for (int k = 0; k < n; ++k) {
        const float dx = sxs[k] - px, dy = sys[k] - py;
        const float d = dx * dx + dy * dy;
        if (d < best) { best = d; bi = k; }
    }

    const float mrx = sxs[bi] - px, mry = sys[bi] - py;   /* point -> nearest site */
    int   bo = bi;
    float minBorder = 1e30f;
    for (int k = 0; k < n; ++k) {
        if (k == bi) continue;
        const float rx = sxs[k] - px, ry = sys[k] - py;   /* point -> other site */
        const float ex = rx - mrx, ey = ry - mry;
        const float elen2 = ex * ex + ey * ey;
        if (elen2 < 1e-8f) continue;   /* two sites landed on the same point */
        const float elen = sqrtf(elen2);
        const float d = ((mrx + rx) * 0.5f * ex + (mry + ry) * 0.5f * ey) / elen;
        if (d < minBorder) { minBorder = d; bo = k; }
    }

    if (out_d1)     *out_d1     = sqrtf(best);
    if (out_border) *out_border = (minBorder < 1e30f) ? minBorder : 0.0f;
    if (out_nx)     *out_nx     = sxs[bo];
    if (out_ny)     *out_ny     = sys[bo];
    if (out_sx)     *out_sx     = sxs[bi];
    if (out_sy)     *out_sy     = sys[bi];
}

template <class Fn>
static inline void sa_voronize_uniform_visit_ring(int radius, const Fn& visit)
{
    const int x0 = 2 - radius, x1 = 2 + radius;
    const int y0 = 2 - radius, y1 = 2 + radius;
    if (radius == 0) {
        visit(2 * SA_VZ_UNIFORM_TILE_SIDE + 2);
        return;
    }
    for (int x = x0; x <= x1; ++x) {
        visit(y0 * SA_VZ_UNIFORM_TILE_SIDE + x);
        visit(y1 * SA_VZ_UNIFORM_TILE_SIDE + x);
    }
    for (int y = y0 + 1; y < y1; ++y) {
        visit(y * SA_VZ_UNIFORM_TILE_SIDE + x0);
        visit(y * SA_VZ_UNIFORM_TILE_SIDE + x1);
    }
}

/* Lower bound on the distance from the query point to any site in an
   unvisited Uniform cell. Jitter never moves a site outside its own cell, so
   the nearest unvisited cell boundary is a conservative site-distance bound. */
static inline float sa_voronize_uniform_outside_lower(
    const SaVoronizeUniformTile* tile, int radius,
    float px, float py, float pad)
{
    if (radius >= 2) return 1e30f;
    const int x0 = 2 - radius, x1 = 2 + radius;
    const int y0 = 2 - radius, y1 = 2 + radius;
    const float left = px - (float)(tile->origin_x + x0) * tile->cell - pad;
    const float right = (float)(tile->origin_x + x1 + 1) * tile->cell - px - pad;
    const float top = py - (float)(tile->origin_y + y0) * tile->cell - pad;
    const float bottom = (float)(tile->origin_y + y1 + 1) * tile->cell - py - pad;
    const float lr = left < right ? left : right;
    const float tb = top < bottom ? top : bottom;
    const float lower = lr < tb ? lr : tb;
    return lower > 0.0f ? lower : 0.0f;
}

/* Exact-result spatial search for a Uniform 5x5 tile. The two returned
   indices let the renderer reuse colours sampled once for these same sites.
   Index ties reproduce sa_voronize_edge_from_sites' first-in-array rule. */
static inline void sa_voronize_edge_from_uniform_tile(
    const SaVoronizeUniformTile* tile, float px, float py,
    float* out_d1, float* out_border,
    float* out_nx, float* out_ny,
    float* out_sx, float* out_sy,
    int* out_site_index, int* out_neighbour_index)
{
    const float bound_pad = 32.0f * 1.192092896e-7f *
                            (fabsf(px) + fabsf(py) + tile->cell + 1.0f)
                          + tile->cell * 1.0e-6f;
    int bi = -1;
    float best = 1e30f;
    for (int radius = 0; radius <= 2; ++radius) {
        sa_voronize_uniform_visit_ring(radius, [&](int k) {
            const float dx = tile->sx[k] - px, dy = tile->sy[k] - py;
            const float d = dx * dx + dy * dy;
            if (d < best || (d == best && (bi < 0 || k < bi))) {
                best = d;
                bi = k;
            }
        });
        const float lower = sa_voronize_uniform_outside_lower(
            tile, radius, px, py, bound_pad);
        if (lower >= 1e30f || lower * lower > best) break;
    }

    const float mrx = tile->sx[bi] - px, mry = tile->sy[bi] - py;
    const float nearest = sqrtf(best);
    int bo = bi;
    float min_border = 1e30f;
    for (int radius = 0; radius <= 2; ++radius) {
        sa_voronize_uniform_visit_ring(radius, [&](int k) {
            if (k == bi) return;
            const float rx = tile->sx[k] - px, ry = tile->sy[k] - py;
            const float ex = rx - mrx, ey = ry - mry;
            const float elen2 = ex * ex + ey * ey;
            if (elen2 < 1e-8f) return;
            const float elen = sqrtf(elen2);
            const float d = ((mrx + rx) * 0.5f * ex +
                             (mry + ry) * 0.5f * ey) / elen;
            if (d < min_border || (d == min_border && k < bo)) {
                min_border = d;
                bo = k;
            }
        });
        const float lower = sa_voronize_uniform_outside_lower(
            tile, radius, px, py, bound_pad);
        if (lower >= 1e30f) break;
        const float conservative = (lower - nearest) * 0.5f - bound_pad;
        if (conservative > min_border) break;
    }

    if (out_d1)              *out_d1 = nearest;
    if (out_border)          *out_border = min_border < 1e30f ? min_border : 0.0f;
    if (out_nx)              *out_nx = tile->sx[bo];
    if (out_ny)              *out_ny = tile->sy[bo];
    if (out_sx)              *out_sx = tile->sx[bi];
    if (out_sy)              *out_sy = tile->sy[bi];
    if (out_site_index)      *out_site_index = bi;
    if (out_neighbour_index) *out_neighbour_index = bo;
}

/* Exhaustive metric-space nearest and border search.  Unlike the scalar
   Uniform helper, this intentionally evaluates every cached candidate: the
   normalized winner and its physical border metric are the A-mode contract. */
static inline void sa_voronize_edge_from_metric_uniform_tile(
    const SaVoronizeMetric* v, const SaVoronizeMetricUniformTile* tile,
    float px, float py,
    float* out_d1, float* out_border,
    float* out_nx, float* out_ny,
    float* out_sx, float* out_sy,
    int* out_site_index, int* out_neighbour_index)
{
    const float ux = px / v->size_x, uy = py / v->size_y;
    int bi = 0;
    float best = 1e30f;
    for (int k = 0; k < SA_VZ_UNIFORM_TILE_SITES; ++k) {
        const float dx = tile->ux[k] - ux, dy = tile->uy[k] - uy;
        const float d = dx * dx + dy * dy;
        if (d < best) { best = d; bi = k; }
    }

    int bo = bi;
    float min_border = 1e30f;
    for (int k = 0; k < SA_VZ_UNIFORM_TILE_SITES; ++k) {
        if (k == bi) continue;
        const float vx = tile->ux[k] - tile->ux[bi];
        const float vy = tile->uy[k] - tile->uy[bi];
        const float numerator = fabsf(vx * (ux - 0.5f * (tile->ux[bi] + tile->ux[k])) +
                                      vy * (uy - 0.5f * (tile->uy[bi] + tile->uy[k])));
        const float denominator = sqrtf((vx / v->size_x) * (vx / v->size_x) +
                                        (vy / v->size_y) * (vy / v->size_y));
        if (denominator == 0.0f) continue;
        const float border = numerator / denominator;
        if (border < min_border) { min_border = border; bo = k; }
    }

    const float dx = tile->px[bi] - px, dy = tile->py[bi] - py;
    if (out_d1)              *out_d1 = sqrtf(dx * dx + dy * dy);
    if (out_border)          *out_border = min_border < 1e30f ? min_border : 0.0f;
    if (out_nx)              *out_nx = tile->px[bo];
    if (out_ny)              *out_ny = tile->py[bo];
    if (out_sx)              *out_sx = tile->px[bi];
    if (out_sy)              *out_sy = tile->py[bi];
    if (out_site_index)      *out_site_index = bi;
    if (out_neighbour_index) *out_neighbour_index = bo;
}

template <class Fn>
static inline void sa_voronize_metric_scatter_visit_ring(
    const SaVoronizeMetricScatterTile* tile, int qx, int qy, int radius,
    const Fn& visit)
{
    int x0 = qx - radius, x1 = qx + radius;
    int y0 = qy - radius, y1 = qy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SA_VZ_SCATTER_GRID_SIDE) x1 = SA_VZ_SCATTER_GRID_SIDE - 1;
    if (y1 >= SA_VZ_SCATTER_GRID_SIDE) y1 = SA_VZ_SCATTER_GRID_SIDE - 1;

    if (radius == 0) {
        const int k = tile->bin_to_candidate[qy * SA_VZ_SCATTER_GRID_SIDE + qx];
        if (k >= 0) visit(k);
        return;
    }

    for (int x = x0; x <= x1; ++x) {
        int k = tile->bin_to_candidate[y0 * SA_VZ_SCATTER_GRID_SIDE + x];
        if (k >= 0) visit(k);
        if (y1 != y0) {
            k = tile->bin_to_candidate[y1 * SA_VZ_SCATTER_GRID_SIDE + x];
            if (k >= 0) visit(k);
        }
    }
    for (int y = y0 + 1; y < y1; ++y) {
        int k = tile->bin_to_candidate[y * SA_VZ_SCATTER_GRID_SIDE + x0];
        if (k >= 0) visit(k);
        if (x1 != x0) {
            k = tile->bin_to_candidate[y * SA_VZ_SCATTER_GRID_SIDE + x1];
            if (k >= 0) visit(k);
        }
    }
}

/* Lower bound in normalized scatter-subcell units.  The unvisited boundary
   is physical, then each axis is divided by its own pitch before the minimum
   is taken; mixing physical Euclidean distance here would choose wrong rings
   for anisotropic cells. */
static inline float sa_voronize_scatter_outside_lower_metric(
    const SaVoronizeMetricScatterTile* tile, int qx, int qy, int radius,
    float px, float py)
{
    int x0 = qx - radius, x1 = qx + radius;
    int y0 = qy - radius, y1 = qy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SA_VZ_SCATTER_GRID_SIDE) x1 = SA_VZ_SCATTER_GRID_SIDE - 1;
    if (y1 >= SA_VZ_SCATTER_GRID_SIDE) y1 = SA_VZ_SCATTER_GRID_SIDE - 1;

    float lower = 1e30f;
    if (x0 > 0) {
        const float boundary = (float)(tile->origin_gx + x0) * tile->cell_x;
        const float d = (px - boundary) / tile->cell_x;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    if (x1 + 1 < SA_VZ_SCATTER_GRID_SIDE) {
        const float boundary = (float)(tile->origin_gx + x1 + 1) * tile->cell_x;
        const float d = (boundary - px) / tile->cell_x;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    if (y0 > 0) {
        const float boundary = (float)(tile->origin_gy + y0) * tile->cell_y;
        const float d = (py - boundary) / tile->cell_y;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    if (y1 + 1 < SA_VZ_SCATTER_GRID_SIDE) {
        const float boundary = (float)(tile->origin_gy + y1 + 1) * tile->cell_y;
        const float d = (boundary - py) / tile->cell_y;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    return lower;
}

/* Exact normalized nearest-site selection and physical-pixel border distance
   for the 5x5 scatter gather.  Optional ring outputs are test instrumentation
   only; passing null leaves the renderer-facing result unchanged. */
static inline void sa_voronize_edge_from_metric_scatter_tile(
    const SaVoronizeMetricScatterTile* tile, int query_gx, int query_gy,
    float px, float py,
    float* out_d1, float* out_border,
    float* out_nx, float* out_ny,
    float* out_sx, float* out_sy,
    int* out_nearest_last_radius, int* out_border_last_radius)
{
    const float query_ux = px / tile->cell_x;
    const float query_uy = py / tile->cell_y;
    const float epsilon = 32.0f * FLT_EPSILON *
        (fabsf(query_ux) + fabsf(query_uy) + 1.0f) + 1.0e-6f;
    int qx = query_gx - tile->origin_gx;
    int qy = query_gy - tile->origin_gy;
    if (qx < 0) qx = 0;
    if (qy < 0) qy = 0;
    if (qx >= SA_VZ_SCATTER_GRID_SIDE) qx = SA_VZ_SCATTER_GRID_SIDE - 1;
    if (qy >= SA_VZ_SCATTER_GRID_SIDE) qy = SA_VZ_SCATTER_GRID_SIDE - 1;

    int rx = qx > SA_VZ_SCATTER_GRID_SIDE - 1 - qx
           ? qx : SA_VZ_SCATTER_GRID_SIDE - 1 - qx;
    int ry = qy > SA_VZ_SCATTER_GRID_SIDE - 1 - qy
           ? qy : SA_VZ_SCATTER_GRID_SIDE - 1 - qy;
    const int max_radius = rx > ry ? rx : ry;

    int bi = -1;
    float best = 1e30f;
    int nearest_last_radius = max_radius;
    for (int radius = 0; radius <= max_radius; ++radius) {
        sa_voronize_metric_scatter_visit_ring(tile, qx, qy, radius, [&](int k) {
            const float dx = tile->ux[k] - query_ux, dy = tile->uy[k] - query_uy;
            const float d = dx * dx + dy * dy;
            if (d < best || (d == best && (bi < 0 || k < bi))) {
                best = d;
                bi = k;
            }
        });
        if (bi >= 0) {
            const float outside_R = sa_voronize_scatter_outside_lower_metric(
                tile, qx, qy, radius, px, py);
            if (outside_R >= 1e30f || outside_R > sqrtf(best) + epsilon) {
                nearest_last_radius = radius;
                break;
            }
        }
    }

    if (bi < 0) {
        if (out_d1) *out_d1 = 0.0f;
        if (out_border) *out_border = 0.0f;
        if (out_nx) *out_nx = 0.0f;
        if (out_ny) *out_ny = 0.0f;
        if (out_sx) *out_sx = 0.0f;
        if (out_sy) *out_sy = 0.0f;
        if (out_nearest_last_radius) *out_nearest_last_radius = nearest_last_radius;
        if (out_border_last_radius) *out_border_last_radius = max_radius;
        return;
    }

    const float nearest_r = sqrtf(best);
    const float min_cell = tile->cell_x < tile->cell_y ? tile->cell_x : tile->cell_y;
    int bo = bi;
    float min_border = 1e30f;
    int border_last_radius = max_radius;
    for (int radius = 0; radius <= max_radius; ++radius) {
        sa_voronize_metric_scatter_visit_ring(tile, qx, qy, radius, [&](int k) {
            if (k == bi) return;
            const float vx = tile->ux[k] - tile->ux[bi];
            const float vy = tile->uy[k] - tile->uy[bi];
            const float numerator = fabsf(vx * (query_ux - 0.5f * (tile->ux[bi] + tile->ux[k])) +
                                          vy * (query_uy - 0.5f * (tile->uy[bi] + tile->uy[k])));
            const float denominator = sqrtf((vx / tile->cell_x) * (vx / tile->cell_x) +
                                            (vy / tile->cell_y) * (vy / tile->cell_y));
            if (denominator == 0.0f) return;
            const float border = numerator / denominator;
            if (border < min_border || (border == min_border && k < bo)) {
                min_border = border;
                bo = k;
            }
        });
        if (min_border < 1e30f) {
            const float outside_R = sa_voronize_scatter_outside_lower_metric(
                tile, qx, qy, radius, px, py);
            if (outside_R >= 1e30f) {
                border_last_radius = radius;
                break;
            }
            const float lower_border_px = min_cell *
                (0.5f * (outside_R - nearest_r) - epsilon);
            if (lower_border_px > min_border) {
                border_last_radius = radius;
                break;
            }
        }
    }

    const float dx = tile->px[bi] - px, dy = tile->py[bi] - py;
    if (out_d1) *out_d1 = sqrtf(dx * dx + dy * dy);
    if (out_border) *out_border = min_border < 1e30f ? min_border : 0.0f;
    if (out_nx) *out_nx = tile->px[bo];
    if (out_ny) *out_ny = tile->py[bo];
    if (out_sx) *out_sx = tile->px[bi];
    if (out_sy) *out_sy = tile->py[bi];
    if (out_nearest_last_radius) *out_nearest_last_radius = nearest_last_radius;
    if (out_border_last_radius) *out_border_last_radius = border_last_radius;
}

template <class Fn>
static inline void sa_voronize_scatter_visit_ring(const SaVoronizeScatterTile* tile,
                                                  int qx, int qy, int radius,
                                                  const Fn& visit)
{
    int x0 = qx - radius, x1 = qx + radius;
    int y0 = qy - radius, y1 = qy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SA_VZ_SCATTER_GRID_SIDE) x1 = SA_VZ_SCATTER_GRID_SIDE - 1;
    if (y1 >= SA_VZ_SCATTER_GRID_SIDE) y1 = SA_VZ_SCATTER_GRID_SIDE - 1;

    if (radius == 0) {
        const int k = tile->bin_to_candidate[qy * SA_VZ_SCATTER_GRID_SIDE + qx];
        if (k >= 0) visit(k);
        return;
    }

    for (int x = x0; x <= x1; ++x) {
        int k = tile->bin_to_candidate[y0 * SA_VZ_SCATTER_GRID_SIDE + x];
        if (k >= 0) visit(k);
        if (y1 != y0) {
            k = tile->bin_to_candidate[y1 * SA_VZ_SCATTER_GRID_SIDE + x];
            if (k >= 0) visit(k);
        }
    }
    for (int y = y0 + 1; y < y1; ++y) {
        int k = tile->bin_to_candidate[y * SA_VZ_SCATTER_GRID_SIDE + x0];
        if (k >= 0) visit(k);
        if (x1 != x0) {
            k = tile->bin_to_candidate[y * SA_VZ_SCATTER_GRID_SIDE + x1];
            if (k >= 0) visit(k);
        }
    }
}

/* Conservative lower bound on the distance from the query to any subcell
   outside the square of rings already visited. `pad` expands the unvisited
   region toward the query to cover float boundary rounding. */
static inline float sa_voronize_scatter_outside_lower(
    const SaVoronizeScatterTile* tile, int qx, int qy, int radius,
    float px, float py, float pad)
{
    int x0 = qx - radius, x1 = qx + radius;
    int y0 = qy - radius, y1 = qy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SA_VZ_SCATTER_GRID_SIDE) x1 = SA_VZ_SCATTER_GRID_SIDE - 1;
    if (y1 >= SA_VZ_SCATTER_GRID_SIDE) y1 = SA_VZ_SCATTER_GRID_SIDE - 1;

    float lower = 1e30f;
    if (x0 > 0) {
        const float boundary = (float)(tile->origin_gx + x0) * tile->cell;
        const float d = px - boundary - pad;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    if (x1 + 1 < SA_VZ_SCATTER_GRID_SIDE) {
        const float boundary = (float)(tile->origin_gx + x1 + 1) * tile->cell;
        const float d = boundary - px - pad;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    if (y0 > 0) {
        const float boundary = (float)(tile->origin_gy + y0) * tile->cell;
        const float d = py - boundary - pad;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    if (y1 + 1 < SA_VZ_SCATTER_GRID_SIDE) {
        const float boundary = (float)(tile->origin_gy + y1 + 1) * tile->cell;
        const float d = boundary - py - pad;
        if ((d > 0.0f ? d : 0.0f) < lower) lower = d > 0.0f ? d : 0.0f;
    }
    return lower;
}

/* Exact-result spatial search for an adaptive scatter tile.

   The nearest pass stops only when every unvisited subcell is farther than
   the best exact squared distance already seen. After the nearest site is
   fixed, a candidate at query distance R and nearest distance r cannot put
   their bisector closer than (R-r)/2; the border pass uses that lower bound
   to stop safely. Comparisons use the original candidate index as a tie
   break, reproducing sa_voronize_edge_from_sites' first-in-array result even
   though candidates are visited in spatial order. */
static inline void sa_voronize_edge_from_scatter_tile(
    const SaVoronizeScatterTile* tile, int query_gx, int query_gy,
    float px, float py,
    float* out_d1, float* out_border,
    float* out_nx, float* out_ny,
    float* out_sx, float* out_sy)
{
    int qx = query_gx - tile->origin_gx;
    int qy = query_gy - tile->origin_gy;
    if (qx < 0) qx = 0;
    if (qy < 0) qy = 0;
    if (qx >= SA_VZ_SCATTER_GRID_SIDE) qx = SA_VZ_SCATTER_GRID_SIDE - 1;
    if (qy >= SA_VZ_SCATTER_GRID_SIDE) qy = SA_VZ_SCATTER_GRID_SIDE - 1;

    int rx = qx > SA_VZ_SCATTER_GRID_SIDE - 1 - qx
           ? qx : SA_VZ_SCATTER_GRID_SIDE - 1 - qx;
    int ry = qy > SA_VZ_SCATTER_GRID_SIDE - 1 - qy
           ? qy : SA_VZ_SCATTER_GRID_SIDE - 1 - qy;
    const int max_radius = rx > ry ? rx : ry;
    const float bound_pad = 32.0f * 1.192092896e-7f *
                            (fabsf(px) + fabsf(py) + tile->cell + 1.0f)
                          + tile->cell * 1.0e-6f;

    int bi = -1;
    float best = 1e30f;
    for (int radius = 0; radius <= max_radius; ++radius) {
        sa_voronize_scatter_visit_ring(tile, qx, qy, radius, [&](int k) {
            const float dx = tile->sx[k] - px, dy = tile->sy[k] - py;
            const float d = dx * dx + dy * dy;
            if (d < best || (d == best && (bi < 0 || k < bi))) {
                best = d;
                bi = k;
            }
        });
        if (bi >= 0) {
            const float lower = sa_voronize_scatter_outside_lower(
                tile, qx, qy, radius, px, py, bound_pad);
            if (lower >= 1e30f || lower * lower > best) break;
        }
    }

    /* Every one of the 25 gathered blocks contributes at least a fallback,
       so bi is always valid for renderer-built tiles. */
    const float mrx = tile->sx[bi] - px, mry = tile->sy[bi] - py;
    const float nearest = sqrtf(best);
    int bo = bi;
    float min_border = 1e30f;

    for (int radius = 0; radius <= max_radius; ++radius) {
        sa_voronize_scatter_visit_ring(tile, qx, qy, radius, [&](int k) {
            if (k == bi) return;
            const float rx0 = tile->sx[k] - px, ry0 = tile->sy[k] - py;
            const float ex = rx0 - mrx, ey = ry0 - mry;
            const float elen2 = ex * ex + ey * ey;
            if (elen2 < 1e-8f) return;
            const float elen = sqrtf(elen2);
            const float d = ((mrx + rx0) * 0.5f * ex +
                             (mry + ry0) * 0.5f * ey) / elen;
            if (d < min_border || (d == min_border && k < bo)) {
                min_border = d;
                bo = k;
            }
        });
        if (min_border < 1e30f) {
            const float lower_r = sa_voronize_scatter_outside_lower(
                tile, qx, qy, radius, px, py, bound_pad);
            if (lower_r >= 1e30f) break;
            const float conservative = (lower_r - nearest) * 0.5f - bound_pad;
            if (conservative > min_border) break;
        }
    }

    if (out_d1)     *out_d1     = nearest;
    if (out_border) *out_border = min_border < 1e30f ? min_border : 0.0f;
    if (out_nx)     *out_nx     = tile->sx[bo];
    if (out_ny)     *out_ny     = tile->sy[bo];
    if (out_sx)     *out_sx     = tile->sx[bi];
    if (out_sy)     *out_sy     = tile->sy[bi];
}

/*  `out_sx`/`out_sy` (optional) receive the nearest site's own position --
    the same value sa_voronize_nearest's 3x3 search would return, since the
    3x3 neighbourhood already contains the true global nearest site (that is
    the whole correctness argument for the 3x3 bound above) and this 5x5
    search is a strict superset of it. Callers that need both the nearest
    site AND the border/neighbour data get both from this one call instead
    of running sa_voronize_nearest first and throwing its result away. */
static inline void sa_voronize_edge(const SaVoronize* v, float px, float py,
                                    float* out_d1, float* out_border,
                                    float* out_nx, float* out_ny,
                                    float* out_sx, float* out_sy)
{
    const int cx = (int)floorf(px * v->inv_size);
    const int cy = (int)floorf(py * v->inv_size);

    float sxs[25], sys[25];
    int idx = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i, ++idx)
            sa_voronize_site(v, cx + i, cy + j, &sxs[idx], &sys[idx]);

    sa_voronize_edge_from_sites(sxs, sys, 25, px, py, out_d1, out_border, out_nx, out_ny, out_sx, out_sy);
}

/*  Coverage of the band [0, width) around a boundary distance. `dist` is
    typically the true edge distance from sa_voronize_edge (0 exactly on a
    cell boundary, growing inward).

    `hard` off (default) antialiases the outer edge with a smoothstep that
    spans the ENTIRE band from 0 to width, which lets a wide outline fade
    smoothly toward the cell interior -- visible as the underlying Voronoi
    cell's rounded shape bleeding through once the width approaches the
    cell's own radius. `hard` on thresholds coverage to exactly 0 or 1
    instead, so a wide outline reads as a flat fill with no interior
    gradient, at the cost of a jagged (non-antialiased) outer edge.

    `smooth` (only used when `hard` is on) softens just that jagged outer
    edge, independent of `width`: instead of stepping at `width`, coverage
    ramps across the narrow band [width - smooth, width + smooth]. Because
    that ramp is centred ON the boundary and only `smooth` pixels wide (not
    the whole band like the non-hard path above), it antialiases the edge
    without reintroducing the interior gradient -- the flat-fill interior
    stays flat right up to a thin strip around the boundary. */
static inline float sa_voronize_band(float dist, float width, int hard, float smooth)
{
    if (width <= 0.0f) return 0.0f;
    if (hard) {
        if (smooth <= 0.0f) return (dist < width) ? 1.0f : 0.0f;
        const float lo = (width > smooth) ? (width - smooth) : 0.0f;
        const float hi = width + smooth;
        return 1.0f - sa_smoothstep(lo, hi, dist);
    }
    return 1.0f - sa_smoothstep(0.0f, width, dist);
}

/*  Image-adaptive density: how likely a site "survives" in a region whose
    analysis value is `a` (0..1, meaning-of-1 depends on the mode: high detail,
    bright, or dark -- see Sa_voronize.cpp). Mirrors the probability formula in
    tweak_voronoi_browser_prototype/voronoi_singlepass.glsl's getSite(), just
    applied once per analysis block instead of once per candidate site.

    strength = 0 ignores the analysis entirely (probability stays 1).
    min_density is the floor the probability can fall to at a=0, strength=1. */
static inline float sa_voronize_density_probability(float a, float strength, float min_density)
{
    a = sa_clamp01(a);
    strength = sa_clamp01(strength);
    min_density = sa_clamp01(min_density);
    const float shaped = powf(a, 0.65f);
    const float floor_p = sa_lerpf(min_density, 1.0f, shaped);
    return sa_lerpf(1.0f, floor_p, strength);
}

/*  Effective cell-size multiplier for a survival probability `p`: sparser
    surviving sites (lower p) means each one covers more area on average, and
    the average nearest-neighbour spacing of a Poisson process scales as
    1/sqrt(density). Clamped to `max_mult` so the result -- and therefore the
    checkout margin PreRender derives from it -- has a known worst case
    regardless of how low the caller lets p go. */
static inline float sa_voronize_density_mult(float p, float max_mult)
{
    if (max_mult < 1.0f) max_mult = 1.0f;
    if (p < 1e-4f) p = 1e-4f;
    if (p >= 1.0f) return 1.0f;
    return sa_clampf(1.0f / sqrtf(p), 1.0f, max_mult);
}

static inline float sa_voronize_feature_scale(float feature_x, float feature_y)
{
    feature_x = sa_voronize_safe_axis(feature_x);
    feature_y = sa_voronize_safe_axis(feature_y);
    return sqrtf(feature_x * feature_y);
}

static inline void sa_voronize_average_offsets(float feature_x, float feature_y,
                                               int* offset_x, int* offset_y)
{
    int ox = (int)(sa_voronize_safe_axis(feature_x) * 0.18f);
    int oy = (int)(sa_voronize_safe_axis(feature_y) * 0.18f);
    if (ox < 1) ox = 1;
    if (oy < 1) oy = 1;
    *offset_x = ox;
    *offset_y = oy;
}

static inline int sa_voronize_prerender_margin(float size_x, float size_y,
                                               int density_uniform)
{
    const float axis_x = sa_voronize_safe_axis(size_x);
    const float axis_y = sa_voronize_safe_axis(size_y);
    const float max_axis = axis_x > axis_y ? axis_x : axis_y;
    if (density_uniform)
        return (int)ceilf(max_axis * 1.5f) + 2;
    return (int)ceilf(max_axis * SA_VZ_DENSITY_BLOCK_MULT * 3.5f) + 3;
}

/* Metric adaptive rendering must not derive geometry from checkout-local
   extrema: SmartFX can provide a different source ROI for the same output
   pixel. Histogram statistics may remain ROI-local because they are cosmetic,
   but render normalization is always the intrinsic 0..1 density domain. */
static inline void sa_voronize_metric_render_density_range(float* minimum,
                                                           float* maximum)
{
    *minimum = 0.0f;
    *maximum = 1.0f;
}

/*  PreRender always checks out the widened input margin so pixels at the
    source boundary have the same neighbours as before. Cropping changes only
    the advertised output extent: zero out_margin keeps result_rect and
    max_result_rect at the input bounds inside sa_prerender_checkout. */
static inline int sa_voronize_prerender_out_margin(int render_margin,
                                                   int crop_to_source_bounds)
{
    return crop_to_source_bounds ? 0 : render_margin;
}

/*  Deterministic accept test + jittered position for one scatter candidate.

    (gx, gy) is the subcell's ABSOLUTE index (block index * SA_VZ_SCATTER_
    SUBDIV + subcell index, on each axis) -- a plain lattice index, so this
    hashes exactly like sa_voronize_site does, just at the finer subcell
    pitch `cell` instead of the base grid's `size`. `prob` is the survival
    probability already computed for this subcell (from
    sa_voronize_density_probability, using whatever analysis value the caller
    sampled at the subcell's own centre). Returns 1 (and fills *sx, *sy in
    pixels) if this candidate survives the accept test; 0 otherwise -- either
    way *sx/*sy are filled with this subcell's own jittered position, since
    callers also need it as a fallback candidate when a whole block's
    subcells all fail their accept test (see the header note above). */
static inline int sa_voronize_scatter_candidate(float cell, float jitter, int seed,
                                                int gx, int gy, float prob,
                                                float* sx, float* sy)
{
    const float u  = sa_rand3(gx, gy, seed + 2003);
    const float jx = sa_rand3(gx, gy, seed + 3001) - 0.5f;
    const float jy = sa_rand3(gx, gy, seed + 4001) - 0.5f;
    *sx = ((float)gx + 0.5f + jx * (jitter * 2.0f)) * cell;
    *sy = ((float)gy + 0.5f + jy * (jitter * 2.0f)) * cell;
    return (u < prob) ? 1 : 0;
}

/*  Photoshop-style input Levels for the density value, applied AFTER the
    automatic min/max stretch Sa_voronize.cpp's block_analysis already does
    (see SaCtx::density_min/max): clip/remap [black, white] to [0, 1], then a
    gamma curve (gamma > 1 lifts midtones toward "dense", gamma < 1 pushes
    them toward "sparse"). black=0, white=1, gamma=1 is an exact no-op, so
    leaving the Levels sliders untouched reproduces the automatic stretch
    alone -- the Map Histogram custom UI in Sa_voronize.cpp visualises
    exactly the value this function reads as `a`, so the sliders and what the
    user sees always agree. */
static inline float sa_voronize_apply_levels(float a, float black, float white, float gamma)
{
    black = sa_clamp01(black);
    white = sa_clamp01(white);
    if (white < black + 1e-4f) white = black + 1e-4f;   /* degenerate range guard */
    float t = sa_clamp01((a - black) / (white - black));
    if (gamma > 1e-3f && gamma != 1.0f) t = powf(t, 1.0f / gamma);
    return t;
}

/* Shared by the render density scan and the asynchronous Effect Controls
   preview. Values are stretched to the range actually observed in the
   sampled frame; a flat frame is represented by the middle bin. */
static inline int sa_voronize_histogram_bin(float value, float mn, float mx,
                                            int bins)
{
    if (bins <= 1) return 0;
    const float range = mx - mn;
    const float normalized = range > 1e-4f
        ? sa_clamp01((value - mn) / range)
        : 0.5f;
    int bin = (int)(normalized * (float)bins);
    if (bin < 0) bin = 0;
    if (bin >= bins) bin = bins - 1;
    return bin;
}

static inline void sa_voronize_histogram_normalize(const int* counts,
                                                    int bins,
                                                    float* out_hist)
{
    if (!out_hist || bins <= 0) return;
    int peak = 0;
    if (counts) {
        for (int i = 0; i < bins; ++i) {
            if (counts[i] > peak) peak = counts[i];
        }
    }
    for (int i = 0; i < bins; ++i) {
        out_hist[i] = (counts && peak > 0)
            ? (float)counts[i] / (float)peak
            : 0.0f;
    }
}

/* ------------------------------------------------------------------------
   Corner antialiasing -- a whole-frame second pass, run once after the
   mosaic fill, that removes the staircase jaggies along a voronoi cell
   boundary. Scoped port of loilo-inc/smooth (Apache-2.0):
   https://github.com/loilo-inc/smooth -- its "up corner" (mode_flg==3,
   Effect.cpp case 3) and "down corner" (mode_flg==5, case 5), which are
   exactly the 1px-staircase shape a voronoi seam produces. Coverage comes
   from the counted length of the flat run on each side of the corner (a
   triangle-area ratio: upMode_*CountLength / upMode_*Blending and their
   downMode_* mirrors), not a fixed-radius blur. The rarer single-pixel
   protrusion/notch cases (Link8Mode01/02/04/Square, LackMode01/02 in the
   original) are not ported -- voronoi seams are long diagonals, not
   single-pixel spikes, and porting those adds four more geometric special
   cases for a shape this renderer's seams don't produce. Where the
   original would dispatch a Lack notch-fill instead of a normal blend
   (both perpendicular run lengths >= 2, common on a shallow-angle voronoi
   seam), this falls through to the ordinary H+V blend instead of a no-op,
   so every corner still gets antialiased even without the notch fill.

   Works on straight (non-premultiplied) RGBA; the caller unpremultiplies
   before calling and re-premultiplies the result. Reads only ever come
   from in_rgba (never mutated), writes only ever go to out_rgba (starts
   as a copy of in_rgba) -- exactly the original's PF_COPY(input,output)
   + read-only-input contract. A blend can extend several pixels into a
   neighbouring row/column depending on the run length, so this cannot be
   split into independent row-parallel tasks the way the rest of the
   renderer is; call it once over the whole completed frame. */

struct SaCornerRun { float start, end; int length; int fill; };

/* Threshold matches the original's own default sensitivity: its `range`
   slider defaults to 1.0 on a 0..100 scale, giving
   range = 1.0 * (max*4) / 100 -- about 4% of the maximum possible 4-channel
   delta. A much tighter threshold (bit-exact equality) sounds more correct
   for a flat-fill mosaic but isn't: on photographic map/density-driven
   footage, two adjacent cells can sample genuinely different but visually
   near-identical source pixels, and an exact-equality threshold treats that
   as two separate flat regions instead of one -- fragmenting every run
   length down to 0 and silently turning the whole pass into a no-op. */
static inline bool sa_corner_differ(const float* buf, long a, long b)
{
    const float* pa = buf + a * 4;
    const float* pb = buf + b * 4;
    const float d = fabsf(pa[0] - pb[0]) + fabsf(pa[1] - pb[1]) +
                    fabsf(pa[2] - pb[2]) + fabsf(pa[3] - pb[3]);
    return d > 0.04f;
}
static inline bool sa_corner_same(const float* buf, long a, long b)
{
    return !sa_corner_differ(buf, a, b);
}

/* Straight-colour blend of `target` toward `ref`, weighted by ratio (1 =
   pure target). Matches the original BlendingPixelf's alpha==0 special
   cases: a fully-transparent side contributes no colour, only coverage --
   the same premultiplied-dilution fix this codebase already made once
   elsewhere, applied here to straight colour instead. */
static inline void sa_corner_blend_pixel(const float* in_rgba, float* out_rgba,
                                          long target, long ref, long out_target,
                                          float ratio)
{
    const float* t = in_rgba + target * 4;
    const float* r = in_rgba + ref * 4;
    float* o = out_rgba + out_target * 4;
    const float inv = 1.0f - ratio;
    if (t[3] <= 0.0f)      { o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; }
    else if (r[3] <= 0.0f) { o[0] = t[0]; o[1] = t[1]; o[2] = t[2]; }
    else {
        o[0] = t[0] * ratio + r[0] * inv;
        o[1] = t[1] * ratio + r[1] * inv;
        o[2] = t[2] * ratio + r[2] * inv;
    }
    o[3] = t[3] * ratio + r[3] * inv;
}

/* ---- up corner (case 3): CountLength, ported verbatim from upMode.cpp ---- */

static inline void sa_corner_up_left_count(const float* buf, int width, int height,
                                            int i, int j, long in_target,
                                            SaCornerRun core[4], int recheck)
{
    long ct; int len = 1;
    for (;;) {
        ct = in_target - (len - 1);
        if (sa_corner_differ(buf, ct, ct - 1)) {
            core[0].start = (float)(i + 1); core[0].end = (float)(i + 1) - (float)len;
            core[0].fill = 1;
            break;
        }
        ct = in_target - width - (len - 1);
        if (sa_corner_differ(buf, ct, ct - 1)) {
            core[0].start = (float)(i + 1); core[0].end = (float)(i + 1) - (float)len;
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct - 1, ct - 1 - width)) {
                SaCornerRun sc[4] = {};
                const int si = i - len, sj = j - 1;
                sa_corner_up_left_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (sc[0].length - len == 1) core[0].end -= 0.5f;
            }
            break;
        }
        ++len;
        if (i - len <= 1) {
            len = i - 1;
            core[0].start = (float)(i + 1); core[0].end = (float)(i + 1) - (float)len;
            break;
        }
    }
    core[0].length = len;
}

static inline void sa_corner_up_right_count(const float* buf, int width, int height,
                                             int i, int j, long in_target,
                                             SaCornerRun core[4], int recheck)
{
    long ct; int len = 0;
    ct = in_target + width;
    if (sa_corner_differ(buf, ct, ct + 1) && sa_corner_same(buf, in_target + 1, in_target + 1 + width)) {
        core[1].length = 0; return;
    }
    ++len;
    if ((i + 1) + len >= (width - 1)) {
        len = width - 1 - (i + 1);
        core[1].start = (float)(i + 1); core[1].end = (float)(i + 1) + (float)len; core[1].length = len;
        return;
    }
    for (;;) {
        ct = in_target + len;
        if (sa_corner_differ(buf, ct, ct + 1)) {
            core[1].start = (float)(i + 1); core[1].end = (float)(i + 1 + len);
            core[1].fill = 1;
            break;
        }
        ct = in_target + width + len;
        if (sa_corner_differ(buf, ct, ct + 1)) {
            core[1].start = (float)(i + 1); core[1].end = (float)(i + 1 + len);
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct, ct + 1)) {
                SaCornerRun sc[4] = {};
                const int si = i + len, sj = j + 1;
                sa_corner_up_right_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (len - sc[1].length == 1 && sc[1].length != 0) core[1].end -= 0.5f;
            }
            break;
        }
        ++len;
        if ((i + 1) + len >= (width - 1)) {
            len = width - 1 - (i + 1);
            core[1].start = (float)(i + 1); core[1].end = (float)(i + 1) + (float)len;
            break;
        }
    }
    core[1].length = len;
}

static inline void sa_corner_up_top_count(const float* buf, int width, int height,
                                           int i, int j, long in_target,
                                           SaCornerRun core[4], int recheck)
{
    long ct; int len = 0;
    ct = in_target - 1;
    if (sa_corner_differ(buf, ct, ct - width) && sa_corner_same(buf, in_target - width, in_target - 1 - width)) {
        core[2].length = 0; core[2].start = (float)j; core[2].end = core[2].start; return;
    }
    ++len;
    if (j - len <= 1) {
        len = j - 1;
        core[2].start = (float)j; core[2].end = (float)(j - len); core[2].length = len;
        return;
    }
    for (;;) {
        ct = in_target - (long)len * width;
        if (sa_corner_differ(buf, ct, ct - width)) {
            core[2].start = (float)j; core[2].end = (float)(j - len);
            core[2].fill = 1;
            break;
        }
        ct = in_target - (long)len * width - 1;
        if (sa_corner_differ(buf, ct, ct - width)) {
            core[2].start = (float)j; core[2].end = (float)(j - len);
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct, ct + 1)) {
                SaCornerRun sc[4] = {};
                const int si = i - 1, sj = j - len;
                sa_corner_up_top_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (len - sc[2].length == 1 && sc[2].length != 0) core[2].end += 0.5f;
            }
            break;
        }
        ++len;
        if (j - len <= 1) {
            len = j - 1;
            core[2].start = (float)j; core[2].end = (float)(j - len);
            break;
        }
    }
    core[2].length = len;
}

static inline void sa_corner_up_bottom_count(const float* buf, int width, int height,
                                              int i, int j, long in_target,
                                              SaCornerRun core[4], int recheck)
{
    long ct; int len = 1;
    for (;;) {
        ct = in_target + (long)(len - 1) * width;
        if (sa_corner_differ(buf, ct, ct + width)) {
            core[3].start = (float)j; core[3].end = (float)(j + len);
            core[3].fill = 1;
            break;
        }
        ct = in_target + (long)(len - 1) * width + 1;
        if (sa_corner_differ(buf, ct, ct + width)) {
            core[3].start = (float)j; core[3].end = (float)(j + len);
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct + width, ct + width + 1)) {
                SaCornerRun sc[4] = {};
                const int si = i + 1, sj = j + len;
                sa_corner_up_bottom_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (sc[3].length - len == 1) core[3].end += 0.5f;
            }
            break;
        }
        ++len;
        if (j + len >= height - 1) {
            len = height - 1 - j;
            core[3].start = (float)j; core[3].end = (float)(j + len);
            break;
        }
    }
    core[3].length = len;
}

/* ---- down corner (case 5): CountLength, ported verbatim from downMode.cpp ---- */

static inline void sa_corner_down_left_count(const float* buf, int width, int height,
                                              int i, int j, long in_target,
                                              SaCornerRun core[4], int recheck)
{
    long ct; int len = 1;
    for (;;) {
        ct = in_target - (len - 1);
        if (sa_corner_differ(buf, ct, ct - 1)) {
            core[0].start = (float)(i + 1); core[0].end = (float)(i + 1) - (float)len;
            core[0].fill = 1;
            break;
        }
        ct = in_target + width - (len - 1);
        if (sa_corner_differ(buf, ct, ct - 1)) {
            core[0].start = (float)(i + 1); core[0].end = (float)(i + 1) - (float)len;
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct - 1, ct - 1 + width)) {
                SaCornerRun sc[4] = {};
                const int si = i - len, sj = j + 1;
                sa_corner_down_left_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (sc[0].length - len == 1) core[0].end -= 0.5f;
            }
            break;
        }
        ++len;
        if (i - len <= 1) {
            len = i - 1;
            core[0].start = (float)(i + 1); core[0].end = (float)(i + 1) - (float)len;
            break;
        }
    }
    core[0].length = len;
}

static inline void sa_corner_down_right_count(const float* buf, int width, int height,
                                               int i, int j, long in_target,
                                               SaCornerRun core[4], int recheck)
{
    long ct; int len = 0;
    ct = in_target - width;
    if (sa_corner_differ(buf, ct, ct + 1) && sa_corner_same(buf, ct + 1, ct + 1 + width)) {
        core[1].length = 0; return;
    }
    ++len;
    if ((i + 1) + len >= (width - 1)) {
        len = width - 1 - (i + 1);
        core[1].start = (float)(i + 1); core[1].end = (float)(i + 1) + (float)len; core[1].length = len;
        return;
    }
    for (;;) {
        ct = in_target + len;
        if (sa_corner_differ(buf, ct, ct + 1)) {
            core[1].start = (float)(i + 1); core[1].end = (float)(i + 1 + len);
            core[1].fill = 1;
            break;
        }
        ct = in_target - width + len;
        if (sa_corner_differ(buf, ct, ct + 1)) {
            core[1].start = (float)(i + 1); core[1].end = (float)(i + 1 + len);
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct, ct + 1)) {
                SaCornerRun sc[4] = {};
                const int si = i + len, sj = j - 1;
                sa_corner_down_right_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (len - sc[1].length == 1 && sc[1].length != 0) core[1].end -= 0.5f;
            }
            break;
        }
        ++len;
        if ((i + 1) + len >= (width - 1)) {
            len = width - 1 - (i + 1);
            core[1].start = (float)(i + 1); core[1].end = (float)(i + 1) + (float)len;
            break;
        }
    }
    core[1].length = len;
}

static inline void sa_corner_down_top_count(const float* buf, int width, int height,
                                             int i, int j, long in_target,
                                             SaCornerRun core[4], int recheck)
{
    long ct; int len = 1;
    for (;;) {
        ct = in_target - (long)(len - 1) * width;
        if (sa_corner_differ(buf, ct, ct - width)) {
            core[2].start = (float)j; core[2].end = (float)(j - len);
            core[2].fill = 1;
            break;
        }
        ct = in_target - (long)(len - 1) * width + 1;
        if (sa_corner_differ(buf, ct, ct - width)) {
            core[2].start = (float)j; core[2].end = (float)(j - len);
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct - width, ct - width + 1)) {
                SaCornerRun sc[4] = {};
                const int si = i + 1, sj = j - len;
                sa_corner_down_top_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (len - sc[2].length == 1) core[2].end += 0.5f;
            }
            break;
        }
        ++len;
        if ((j + 1) - len <= 1) {
            len = j;
            core[2].start = (float)j; core[2].end = (float)(j - len);
            break;
        }
    }
    core[2].length = len;
}

static inline void sa_corner_down_bottom_count(const float* buf, int width, int height,
                                                int i, int j, long in_target,
                                                SaCornerRun core[4], int recheck)
{
    long ct; int len = 0;
    ct = in_target - 1;
    if (sa_corner_differ(buf, ct, ct + width) && sa_corner_same(buf, ct + width, ct + 1 + width)) {
        core[3].length = 0; return;
    }
    ++len;
    if ((j + 1) + len >= height - 1) {
        len = height - 1 - (j + 1);
        core[3].start = (float)j; core[3].end = (float)(j + len); core[3].length = len;
        return;
    }
    for (;;) {
        ct = in_target + (long)len * width;
        if (sa_corner_differ(buf, ct, ct + width)) {
            core[3].start = (float)j; core[3].end = (float)(j + len);
            core[3].fill = 1;
            break;
        }
        ct = in_target + (long)len * width - 1;
        if (sa_corner_differ(buf, ct, ct + width)) {
            core[3].start = (float)j; core[3].end = (float)(j + len);
            if (width - 2 > i && i > 2 && height - 2 > j && j > 2 && !recheck &&
                sa_corner_differ(buf, ct, ct + 1)) {
                SaCornerRun sc[4] = {};
                const int si = i - 1, sj = j + len;
                sa_corner_down_bottom_count(buf, width, height, si, sj, (long)sj * width + si, sc, 1);
                if (sc[3].length - len == 1) core[3].end += 0.5f;
            }
            break;
        }
        ++len;
        if ((j + 1) + len >= height - 1) {
            len = height - 1 - (j + 1);
            core[3].start = (float)j; core[3].end = (float)(j + len);
            break;
        }
    }
    core[3].length = len;
}

/* end-value correction (leftover-half-pixel snap) + weight application,
   shared verbatim between case 3 and case 5 (Effect.cpp has the identical
   block inlined at both call sites). */
static inline void sa_corner_adjust_ends(SaCornerRun core[4], float line_weight)
{
    if (core[0].length - core[1].length == 1) { core[0].start -= 0.5f; core[1].start -= 0.5f; }
    {
        const float weight = (core[0].fill || core[1].fill) ? 0.5f : line_weight;
        core[0].end = core[0].start - (core[0].start - core[0].end) * weight;
        core[1].end = core[1].start + (core[1].end - core[1].start) * weight;
    }
    if (core[3].length - core[2].length == 1) { core[2].start += 0.5f; core[3].start += 0.5f; }
    {
        const float weight = (core[2].fill || core[3].fill) ? 0.5f : line_weight;
        core[2].end = core[2].start - (core[2].start - core[2].end) * weight;
        core[3].end = core[3].start + (core[3].end - core[3].start) * weight;
    }
}

/* ---- Blending: ported verbatim from upMode.cpp / downMode.cpp ---- */

static inline void sa_corner_up_left_blend(const float* in_rgba, float* out_rgba,
                                            int width, int i, long target, const SaCornerRun core[4])
{
    const float start = core[0].start, end = core[0].end, len = start - end;
    if (len <= 0.0f) return;
    const int end_p = (int)end;
    const int blend_count = (int)ceilf((float)(i + 1) - end);
    float pre_ratio = 0.0f;
    long bt = target - (blend_count - 1);
    for (int t = 0; t < blend_count; ++t) {
        const float l = (float)(end_p + 1 + t) - end;
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt - width, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; ++bt;
    }
}
static inline void sa_corner_up_right_blend(const float* in_rgba, float* out_rgba,
                                             int width, int i, long target, const SaCornerRun core[4])
{
    const float start = core[1].start, end = core[1].end;
    if (core[1].length <= 0) return;
    const float len = end - start;
    const int end_p = (int)(end - 0.000001f);
    const int blend_count = (int)ceilf(end - (float)(i + 1));
    float pre_ratio = 0.0f;
    long bt = target + blend_count;
    for (int t = 0; t < blend_count; ++t) {
        const float l = end - (float)(end_p - t);
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt + width, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; --bt;
    }
}
static inline void sa_corner_up_top_blend(const float* in_rgba, float* out_rgba,
                                           int width, int j, long target, const SaCornerRun core[4])
{
    if (core[2].length <= 0) return;
    const float start = core[2].start, end = core[2].end, len = start - end;
    const int end_p = (int)end;
    const int blend_count = (int)ceilf((float)j - end);
    float pre_ratio = 0.0f;
    long bt = target - (long)blend_count * width;
    for (int t = 0; t < blend_count; ++t) {
        const float l = (float)(end_p + 1 + t) - end;
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt - 1, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; bt += width;
    }
}
static inline void sa_corner_up_bottom_blend(const float* in_rgba, float* out_rgba,
                                              int width, int j, long target, const SaCornerRun core[4])
{
    const float start = core[3].start, end = core[3].end, len = end - start;
    const int end_p = (int)(end - 0.00001f);
    const int blend_count = (int)ceilf(end - (float)j);
    float pre_ratio = 0.0f;
    long bt = target + (long)(blend_count - 1) * width;
    for (int t = 0; t < blend_count; ++t) {
        const float l = end - (float)(end_p - t);
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt + 1, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; bt -= width;
    }
}

static inline void sa_corner_down_left_blend(const float* in_rgba, float* out_rgba,
                                              int width, int i, long target, const SaCornerRun core[4])
{
    const float start = core[0].start, end = core[0].end, len = start - end;
    if (len <= 0.0f) return;
    const int end_p = (int)end;
    const int blend_count = (int)ceilf((float)(i + 1) - end);
    float pre_ratio = 0.0f;
    long bt = target - (blend_count - 1);
    for (int t = 0; t < blend_count; ++t) {
        const float l = (float)(end_p + 1 + t) - end;
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt + width, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; ++bt;
    }
}
static inline void sa_corner_down_right_blend(const float* in_rgba, float* out_rgba,
                                               int width, int i, long target, const SaCornerRun core[4])
{
    if (core[1].length <= 0) return;
    const float start = core[1].start, end = core[1].end, len = end - start;
    const int end_p = (int)(end - 0.000001f);
    const int blend_count = (int)ceilf(end - (float)(i + 1));
    float pre_ratio = 0.0f;
    long bt = target + blend_count;
    for (int t = 0; t < blend_count; ++t) {
        const float l = end - (float)(end_p - t);
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt - width, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; --bt;
    }
}
static inline void sa_corner_down_top_blend(const float* in_rgba, float* out_rgba,
                                             int width, int j, long target, const SaCornerRun core[4])
{
    const float start = core[2].start, end = core[2].end, len = start - end;
    const int end_p = (int)end;
    const int blend_count = (int)ceilf((float)j - end);
    float pre_ratio = 0.0f;
    long bt = target - (long)(blend_count - 1) * width;
    for (int t = 0; t < blend_count; ++t) {
        const float l = (float)(end_p + 1 + t) - end;
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt + 1, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; bt += width;
    }
}
static inline void sa_corner_down_bottom_blend(const float* in_rgba, float* out_rgba,
                                                int width, int j, long target, const SaCornerRun core[4])
{
    if (core[3].length <= 0) return;
    const float start = core[3].start, end = core[3].end, len = end - start;
    const int end_p = (int)(end - 0.00001f);
    const int blend_count = (int)ceilf(end - (float)j);
    float pre_ratio = 0.0f;
    long bt = target + (long)blend_count * width;
    for (int t = 0; t < blend_count; ++t) {
        const float l = end - (float)(end_p - t);
        const float ratio = (l * l * 0.25f) / len;
        sa_corner_blend_pixel(in_rgba, out_rgba, bt, bt - 1, bt, 1.0f - (ratio - pre_ratio));
        pre_ratio = ratio; bt -= width;
    }
}

/* ---- Driver: main-loop dispatch, ported (scoped) from Effect.cpp's smoothing<>() ---- */

static inline void sa_voronize_corner_smooth(const float* in_rgba, float* out_rgba,
                                              int width, int height, float strength)
{
    for (long k = 0; k < (long)width * height * 4; ++k) out_rgba[k] = in_rgba[k];
    if (strength <= 0.0f || width < 5 || height < 5) return;
    const float line_weight = strength * 0.5f + 0.5f;

    for (int j = 1; j < height - 1; ++j) {
        for (int i = 1; i < width - 1; ++i) {
            const long t = (long)j * width + i;
            unsigned mode_flg = 0;
            if (sa_corner_differ(in_rgba, t, t + 1))     mode_flg |= 1u << 0;
            if (sa_corner_differ(in_rgba, t, t - width)) mode_flg |= 1u << 1;
            if (sa_corner_differ(in_rgba, t, t + width)) mode_flg |= 1u << 2;
            if (sa_corner_differ(in_rgba, t, t - 1))     mode_flg |= 1u << 3;
            if (mode_flg != 3 && mode_flg != 5) continue;

            SaCornerRun core[4] = {};
            if (mode_flg == 3) {
                if (sa_corner_same(in_rgba, t - width, t + 1) &&
                    sa_corner_differ(in_rgba, t - width + 1, t - width) &&
                    sa_corner_differ(in_rgba, t - width + 1, t + 1)) {
                    continue;
                }
                sa_corner_up_left_count  (in_rgba, width, height, i, j, t, core, 0);
                sa_corner_up_right_count (in_rgba, width, height, i, j, t, core, 0);
                sa_corner_up_top_count   (in_rgba, width, height, i, j, t, core, 0);
                sa_corner_up_bottom_count(in_rgba, width, height, i, j, t, core, 0);
                sa_corner_adjust_ends(core, line_weight);
                if (core[1].length > 0) {
                    sa_corner_up_left_blend (in_rgba, out_rgba, width, i, t, core);
                    sa_corner_up_right_blend(in_rgba, out_rgba, width, i, t, core);
                    if (core[2].length > 1) {
                        sa_corner_up_top_blend   (in_rgba, out_rgba, width, j, t, core);
                        sa_corner_up_bottom_blend(in_rgba, out_rgba, width, j, t, core);
                    }
                } else if (core[2].length > 0) {
                    sa_corner_up_top_blend   (in_rgba, out_rgba, width, j, t, core);
                    sa_corner_up_bottom_blend(in_rgba, out_rgba, width, j, t, core);
                }
            } else {
                if (sa_corner_same(in_rgba, t + width, t + 1) &&
                    sa_corner_differ(in_rgba, t + width + 1, t + width) &&
                    sa_corner_differ(in_rgba, t + width + 1, t + 1)) {
                    continue;
                }
                sa_corner_down_left_count  (in_rgba, width, height, i, j, t, core, 0);
                sa_corner_down_right_count (in_rgba, width, height, i, j, t, core, 0);
                sa_corner_down_top_count   (in_rgba, width, height, i, j, t, core, 0);
                sa_corner_down_bottom_count(in_rgba, width, height, i, j, t, core, 0);
                sa_corner_adjust_ends(core, line_weight);
                if (core[1].length > 0) {
                    sa_corner_down_left_blend (in_rgba, out_rgba, width, i, t, core);
                    sa_corner_down_right_blend(in_rgba, out_rgba, width, i, t, core);
                    if (core[3].length > 1) {
                        sa_corner_down_top_blend   (in_rgba, out_rgba, width, j, t, core);
                        sa_corner_down_bottom_blend(in_rgba, out_rgba, width, j, t, core);
                    }
                } else if (core[3].length > 0) {
                    sa_corner_down_top_blend   (in_rgba, out_rgba, width, j, t, core);
                    sa_corner_down_bottom_blend(in_rgba, out_rgba, width, j, t, core);
                }
            }
        }
    }
}

#endif /* SA_VORONIZE_MATH_H */
