/*
    port_regression.cpp -- YMM4版シェーダーの計算を After Effects 版と比べる回帰テスト。

    Shaders/VoronizeCommon.hlsli と VoronizeCells.hlsli を hlsl_shim.h 経由で C++ として
    コンパイルし、Direct2D の各パス (自動レベル補正 → 種マップ → 本体) を CPU で再現します。
    比較対象は tests/reference/ にある AE 版の Sa_voronizeMath.h と、Sa_voronize.cpp の
    描画ループ (sa_render_rows_legacy / _metric と finish_pixel) をこのファイルに写したものです。

    GPU での実行や fxc でのコンパイルは確かめません (それは CI のビルドが担当します)。

        bash tests/run_tests.sh
*/

#include "hlsl_shim.h"

#include <cassert>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "reference/Sa_voronizeMath.h"

/* ------------------------------------------------------------------ */
/*  テスト画像                                                         */
/* ------------------------------------------------------------------ */

struct Image
{
    int w = 0, h = 0;
    std::vector<float> px;   // 乗算済み RGBA
    const float* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
    float* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
};

// 色の勾配、はっきりした縁、半透明と完全透明の領域を含む画像。
static Image make_test_image(int w, int h)
{
    Image img;
    img.w = w; img.h = h;
    img.px.assign((size_t)w * h * 4, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float r = u, g = v, b = 0.5f + 0.5f * sinf(7.0f * u + 3.0f * v);
            const float dx = u - 0.35f, dy = v - 0.45f;
            if (dx * dx + dy * dy < 0.04f) { r = 1.0f; g = 0.95f; b = 0.1f; }
            if (u > 0.6f && u < 0.8f && v > 0.2f && v < 0.7f) { r = 0.05f; g = 0.1f; b = 0.9f; }
            float a = 1.0f;
            if (u > 0.85f) a = 0.0f;                      // 右端は透明
            else if (v > 0.8f) a = 0.25f + 0.5f * u;       // 下端は半透明
            float* p = img.at(x, y);
            p[0] = r * a; p[1] = g * a; p[2] = b * a; p[3] = a;
        }
    }
    return img;
}

/* ------------------------------------------------------------------ */
/*  パラメーター (AE版の意味。ポップアップは1始まり)                   */
/* ------------------------------------------------------------------ */

struct Params
{
    const char* name = "";
    int independent = 0;
    float size = 12.0f, cell_w = 12.0f, cell_h = 12.0f;
    float jitter = 0.5f;           // 0..0.5
    int seed = 1;
    int density_mode = 1;          // 1 均一, 2 輝度, 3 彩度, 4 色相, 5 エッジ検出
    int invert = 0;
    float black = 0.0f, white = 1.0f, gamma = 1.0f;
    float strength = 1.0f, min_density = 0.1f, detail_gain = 10.0f;
    int sample_mode = 1;           // 1 中心, 2 平均
    int edge_on = 1;
    float edge = 0.0f;
    float edge_rgb[3] = { 0.0f, 0.0f, 0.0f };
    float edge_opacity = 1.0f;
    int edge_hard = 0;
    int output_mode = 1;           // 1 画像, 2 境界, 3 距離, 4 シード
    float amount = 1.0f;
};

enum { D_UNIFORM = 1, D_LUMINANCE, D_SATURATION, D_HUE, D_EDGE };

/* ------------------------------------------------------------------ */
/*  AE版の描画 (Sa_voronize.cpp の写し。AE の型だけ置き換えています)   */
/* ------------------------------------------------------------------ */

namespace ref {

struct Ctx
{
    const Image* src;
    Params p;
    SaVoronize vor;
    SaVoronizeMetric metric;
    float size_x, size_y;
    float cx, cy;
    float density_min = 0.0f, density_max = 1.0f;
};

static void sample_clamp(const Image& img, long x, long y, float* rgba)
{
    x = sa_clampi((int)x, 0, img.w - 1);
    y = sa_clampi((int)y, 0, img.h - 1);
    std::memcpy(rgba, img.at((int)x, (int)y), sizeof(float) * 4);
}

static void sample_zero(const Image& img, long x, long y, float* rgba)
{
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    std::memcpy(rgba, img.at((int)x, (int)y), sizeof(float) * 4);
}

static inline void ink(float* p, const float* rgb, float e)
{
    const float a = sa_lerpf(p[3], 1.0f, e);
    for (int k = 0; k < 3; ++k) p[k] = sa_lerpf(p[k], rgb[k], e);
    p[3] = a;
}

static float raw_density_legacy(const Ctx& c, long bix, long biy, float block_pitch)
{
    const long sx0 = (long)floorf(((float)bix + 0.5f) * block_pitch + c.cx);
    const long sy0 = (long)floorf(((float)biy + 0.5f) * block_pitch + c.cy);
    if (c.p.density_mode == D_EDGE) {
        float l[4], r[4], u[4], d[4];
        sample_clamp(*c.src, sx0 - 1, sy0, l);
        sample_clamp(*c.src, sx0 + 1, sy0, r);
        sample_clamp(*c.src, sx0, sy0 - 1, u);
        sample_clamp(*c.src, sx0, sy0 + 1, d);
        sa_unpremul(l); sa_unpremul(r); sa_unpremul(u); sa_unpremul(d);
        const float gx = sa_luminance(r[0], r[1], r[2]) - sa_luminance(l[0], l[1], l[2]);
        const float gy = sa_luminance(d[0], d[1], d[2]) - sa_luminance(u[0], u[1], u[2]);
        return sa_clamp01(sqrtf(gx * gx + gy * gy) * c.p.detail_gain);
    }
    float s[4];
    sample_clamp(*c.src, sx0, sy0, s);
    sa_unpremul(s);
    if (c.p.density_mode == D_SATURATION) {
        const float mx = fmaxf(s[0], fmaxf(s[1], s[2]));
        const float mn = fminf(s[0], fminf(s[1], s[2]));
        return (mx > 1e-6f) ? ((mx - mn) / mx) : 0.0f;
    }
    if (c.p.density_mode == D_HUE) {
        const float mx = fmaxf(s[0], fmaxf(s[1], s[2]));
        const float mn = fminf(s[0], fminf(s[1], s[2]));
        const float chroma = mx - mn;
        float h;
        if (chroma < 1e-6f)   h = 0.0f;
        else if (mx == s[0])  h = sa_mod((s[1] - s[2]) / chroma, 6.0f);
        else if (mx == s[1])  h = (s[2] - s[0]) / chroma + 2.0f;
        else                  h = (s[0] - s[1]) / chroma + 4.0f;
        return h * (1.0f / 6.0f);
    }
    return sa_luminance(s[0], s[1], s[2]);
}

static float raw_density_metric(const Ctx& c, long bix, long biy, float bpx, float bpy)
{
    const long sx0 = (long)floorf(((float)bix + 0.5f) * bpx + c.cx);
    const long sy0 = (long)floorf(((float)biy + 0.5f) * bpy + c.cy);
    if (c.p.density_mode == D_EDGE) {
        float l[4], r[4], u[4], d[4];
        sample_clamp(*c.src, sx0 - 1, sy0, l);
        sample_clamp(*c.src, sx0 + 1, sy0, r);
        sample_clamp(*c.src, sx0, sy0 - 1, u);
        sample_clamp(*c.src, sx0, sy0 + 1, d);
        sa_unpremul(l); sa_unpremul(r); sa_unpremul(u); sa_unpremul(d);
        const float gx = sa_luminance(r[0], r[1], r[2]) - sa_luminance(l[0], l[1], l[2]);
        const float gy = sa_luminance(d[0], d[1], d[2]) - sa_luminance(u[0], u[1], u[2]);
        return sa_clamp01(sqrtf(gx * gx + gy * gy) * c.p.detail_gain);
    }
    float s[4];
    sample_clamp(*c.src, sx0, sy0, s);
    sa_unpremul(s);
    if (c.p.density_mode == D_SATURATION) {
        const float mx = fmaxf(s[0], fmaxf(s[1], s[2]));
        const float mn = fminf(s[0], fminf(s[1], s[2]));
        return (mx > 1e-6f) ? ((mx - mn) / mx) : 0.0f;
    }
    if (c.p.density_mode == D_HUE) {
        const float mx = fmaxf(s[0], fmaxf(s[1], s[2]));
        const float mn = fminf(s[0], fminf(s[1], s[2]));
        const float chroma = mx - mn;
        if (chroma <= 1e-6f) return 0.0f;
        float h;
        if (mx == s[0]) { h = (s[1] - s[2]) / chroma; if (h < 0.0f) h += 6.0f; }
        else if (mx == s[1]) h = (s[2] - s[0]) / chroma + 2.0f;
        else h = (s[0] - s[1]) / chroma + 4.0f;
        return h * (1.0f / 6.0f);
    }
    return sa_luminance(s[0], s[1], s[2]);
}

// sa_voronize_scan_density_range (旧経路だけが伸長します)
static void scan_density_range(Ctx& c, float block_pitch)
{
    c.density_min = 0.0f;
    c.density_max = 1.0f;
    if (c.p.density_mode == D_UNIFORM) return;
    const float lx_min = 0.0f - c.cx, lx_max = lx_min + (float)c.src->w;
    const float ly_min = 0.0f - c.cy, ly_max = ly_min + (float)c.src->h;
    const long bix0 = (long)floorf(lx_min / block_pitch) - 1;
    const long bix1 = (long)floorf(lx_max / block_pitch) + 1;
    const long biy0 = (long)floorf(ly_min / block_pitch) - 1;
    const long biy1 = (long)floorf(ly_max / block_pitch) + 1;
    float mn = 1.0f, mx = 0.0f;
    for (long by = biy0; by <= biy1; ++by)
        for (long bx = bix0; bx <= bix1; ++bx) {
            const float a = raw_density_legacy(c, bx, by, block_pitch);
            if (a < mn) mn = a;
            if (a > mx) mx = a;
        }
    if (mx - mn > 1e-4f) { c.density_min = mn; c.density_max = mx; }
}

struct Geometry { float sx, sy, nx, ny, d1, border; };

// 1画素分。AE版の finish_pixel と同じ。
static void finish_pixel(const Ctx& c, long x, long y, const Geometry& g, float feat_scale,
                         int avg_x, int avg_y, float* out)
{
    const int draw_edge = (c.p.edge_on && c.p.edge > 0.0f && c.p.edge_opacity > 0.0f);
    auto sample_at = [&](float site_x, float site_y, float* rgba) {
        const long sx = (long)floorf(site_x + c.cx);
        const long sy = (long)floorf(site_y + c.cy);
        const bool in = sx >= 0 && sy >= 0 && sx < c.src->w && sy < c.src->h;
        if (c.p.sample_mode == 2 && in) {
            float site[4], t[4], sum[4];
            sample_clamp(*c.src, sx, sy, site);
            const float site_a = site[3];
            sum[0] = site[0] * 4.0f; sum[1] = site[1] * 4.0f;
            sum[2] = site[2] * 4.0f; sum[3] = site[3] * 4.0f;
            static const long ox8[8] = { 1, -1, 0, 0, 1, -1, 1, -1 };
            static const long oy8[8] = { 0, 0, 1, -1, 1, 1, -1, -1 };
            for (int k = 0; k < 8; ++k) {
                sample_clamp(*c.src, sx + ox8[k] * avg_x, sy + oy8[k] * avg_y, t);
                for (int ch = 0; ch < 4; ++ch) sum[ch] += t[ch];
            }
            const float a_sum = sum[3] > 1e-4f ? sum[3] : 1e-4f;
            rgba[0] = (sum[0] / a_sum) * site_a;
            rgba[1] = (sum[1] / a_sum) * site_a;
            rgba[2] = (sum[2] / a_sum) * site_a;
            rgba[3] = site_a;
        } else {
            sample_zero(*c.src, sx, sy, rgba);
        }
    };

    float p[4];
    if (c.p.output_mode == 2) {
        const float view_width = (c.p.edge > 0.0f) ? c.p.edge : 1.5f;
        const float v = sa_voronize_band(g.border, view_width, c.p.edge_hard, 0.0f);
        p[0] = p[1] = p[2] = v; p[3] = 1.0f;
    } else if (c.p.output_mode == 3) {
        const float denom = feat_scale * 0.45f > 1.0f ? feat_scale * 0.45f : 1.0f;
        const float v = sa_clamp01(g.border / denom);
        p[0] = p[1] = p[2] = v; p[3] = 1.0f;
    } else if (c.p.output_mode == 4) {
        const float s = 1.0f - sa_smoothstep(1.5f, 3.5f, g.d1);
        p[0] = p[1] = p[2] = s; p[3] = 1.0f;
    } else {
        sample_at(g.sx, g.sy, p);
        if (draw_edge) {
            float e = sa_voronize_band(g.border, c.p.edge, c.p.edge_hard, 0.0f) * c.p.edge_opacity;
            if (e > 0.0f) {
                float np[4];
                sample_at(g.nx, g.ny, np);
                e *= (p[3] > np[3]) ? p[3] : np[3];
                if (e > 0.0f) ink(p, c.p.edge_rgb, e);
            }
        }
    }
    if (c.p.amount < 1.0f) {
        float o[4];
        sample_zero(*c.src, x, y, o);
        for (int k = 0; k < 4; ++k) p[k] = sa_lerpf(o[k], p[k], c.p.amount);
    }
    std::memcpy(out, p, sizeof(p));
}

// 旧経路 (幅と高さを別指定しない) の1画素。
static Geometry geometry_legacy(const Ctx& c, float lx, float ly,
                                std::map<std::pair<long, long>, SaVoronizeScatterTile>& cache)
{
    Geometry g;
    if (c.p.density_mode == D_UNIFORM) {
        const int qx = (int)floorf(lx * c.vor.inv_size);
        const int qy = (int)floorf(ly * c.vor.inv_size);
        SaVoronizeUniformTile tile;
        sa_voronize_uniform_tile_init(&tile, &c.vor, qx, qy);
        int si, ni;
        sa_voronize_edge_from_uniform_tile(&tile, lx, ly, &g.d1, &g.border, &g.nx, &g.ny,
                                           &g.sx, &g.sy, &si, &ni);
        return g;
    }

    const float block_pitch = c.vor.size * SA_VZ_DENSITY_BLOCK_MULT;
    const float cell = block_pitch / (float)SA_VZ_SCATTER_SUBDIV;
    const long qbx = (long)floorf(lx / block_pitch);
    const long qby = (long)floorf(ly / block_pitch);
    auto key = std::make_pair(qbx, qby);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto block_analysis = [&](long bix, long biy) -> float {
            const float raw = raw_density_legacy(c, bix, biy, block_pitch);
            const float range = c.density_max - c.density_min;
            const float stretched = (range > 1e-4f) ? sa_clamp01((raw - c.density_min) / range) : 0.5f;
            const float a = sa_voronize_apply_levels(stretched, c.p.black, c.p.white, c.p.gamma);
            return c.p.invert ? (1.0f - a) : a;
        };
        float acache[7][7];
        SaVoronizeScatterTile tile;
        sa_voronize_scatter_tile_init(&tile, (int)qbx, (int)qby, cell);
        const long acx0 = qbx - 3, acy0 = qby - 3;
        for (int j = 0; j < 7; ++j)
            for (int i = 0; i < 7; ++i)
                acache[j][i] = block_analysis(acx0 + i, acy0 + j);
        auto analysis_at = [&](float wx, float wy) -> float {
            const float bcx = wx / block_pitch - 0.5f;
            const float bcy = wy / block_pitch - 0.5f;
            const long bix0 = (long)floorf(bcx);
            const long biy0 = (long)floorf(bcy);
            const float fx = bcx - (float)bix0;
            const float fy = bcy - (float)biy0;
            long i0 = bix0 - acx0, j0 = biy0 - acy0;
            i0 = (i0 < 0) ? 0 : (i0 > 5 ? 5 : i0);
            j0 = (j0 < 0) ? 0 : (j0 > 5 ? 5 : j0);
            const float a00 = acache[j0][i0], a10 = acache[j0][i0 + 1];
            const float a01 = acache[j0 + 1][i0], a11 = acache[j0 + 1][i0 + 1];
            return sa_lerpf(sa_lerpf(a00, a10, fx), sa_lerpf(a01, a11, fx), fy);
        };
        for (long bj = -2; bj <= 2; ++bj) {
            for (long bi = -2; bi <= 2; ++bi) {
                const long bx = qbx + bi, by = qby + bj;
                float bestp = -1.0f, fbx = 0.0f, fby = 0.0f;
                int fbgx = 0, fbgy = 0, any = 0;
                for (int sj = 0; sj < SA_VZ_SCATTER_SUBDIV; ++sj) {
                    for (int si = 0; si < SA_VZ_SCATTER_SUBDIV; ++si) {
                        const long gx = bx * SA_VZ_SCATTER_SUBDIV + si;
                        const long gy = by * SA_VZ_SCATTER_SUBDIV + sj;
                        const float wx = ((float)gx + 0.5f) * cell;
                        const float wy = ((float)gy + 0.5f) * cell;
                        const float prob = sa_voronize_density_probability(
                            analysis_at(wx, wy), c.p.strength, c.p.min_density);
                        float sxc, syc;
                        const int keep = sa_voronize_scatter_candidate(
                            cell, c.vor.jitter, c.vor.seed, (int)gx, (int)gy, prob, &sxc, &syc);
                        if (prob > bestp) { bestp = prob; fbx = sxc; fby = syc; fbgx = (int)gx; fbgy = (int)gy; }
                        if (keep) { sa_voronize_scatter_tile_add(&tile, (int)gx, (int)gy, sxc, syc); any = 1; }
                    }
                }
                if (!any) sa_voronize_scatter_tile_add(&tile, fbgx, fbgy, fbx, fby);
            }
        }
        it = cache.emplace(key, tile).first;
    }
    const int qgx = (int)floorf(lx / cell);
    const int qgy = (int)floorf(ly / cell);
    sa_voronize_edge_from_scatter_tile(&it->second, qgx, qgy, lx, ly,
                                       &g.d1, &g.border, &g.nx, &g.ny, &g.sx, &g.sy);
    return g;
}

// 幅と高さを別指定する経路の1画素。
static Geometry geometry_metric(const Ctx& c, float px, float py,
                                std::map<std::pair<long, long>, SaVoronizeMetricScatterTile>& cache)
{
    Geometry g;
    if (c.p.density_mode == D_UNIFORM) {
        const int qx = (int)floorf(px * c.metric.inv_size_x);
        const int qy = (int)floorf(py * c.metric.inv_size_y);
        SaVoronizeMetricUniformTile tile;
        sa_voronize_metric_uniform_tile_init(&tile, &c.metric, qx, qy);
        int si, ni;
        sa_voronize_edge_from_metric_uniform_tile(&c.metric, &tile, px, py, &g.d1, &g.border,
                                                  &g.nx, &g.ny, &g.sx, &g.sy, &si, &ni);
        return g;
    }

    const float bpx = c.metric.size_x * SA_VZ_DENSITY_BLOCK_MULT;
    const float bpy = c.metric.size_y * SA_VZ_DENSITY_BLOCK_MULT;
    const float cell_x = bpx / (float)SA_VZ_SCATTER_SUBDIV;
    const float cell_y = bpy / (float)SA_VZ_SCATTER_SUBDIV;
    const long qbx = (long)floorf(px / bpx);
    const long qby = (long)floorf(py / bpy);
    auto key = std::make_pair(qbx, qby);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto block_analysis = [&](long bix, long biy) -> float {
            const float raw = raw_density_metric(c, bix, biy, bpx, bpy);
            const float range = c.density_max - c.density_min;
            const float stretched = range > 1e-4f ? sa_clamp01((raw - c.density_min) / range) : 0.5f;
            const float a = sa_voronize_apply_levels(stretched, c.p.black, c.p.white, c.p.gamma);
            return c.p.invert ? (1.0f - a) : a;
        };
        float acache[7][7];
        SaVoronizeMetricScatterTile tile;
        sa_voronize_metric_scatter_tile_init(&tile, (int)qbx, (int)qby, cell_x, cell_y);
        const long cx0 = qbx - 3, cy0 = qby - 3;
        for (int j = 0; j < 7; ++j)
            for (int i = 0; i < 7; ++i)
                acache[j][i] = block_analysis(cx0 + i, cy0 + j);
        auto analysis_at = [&](float wx, float wy) -> float {
            const float bx_ = wx / bpx - 0.5f;
            const float by_ = wy / bpy - 0.5f;
            const long bx0 = (long)floorf(bx_);
            const long by0 = (long)floorf(by_);
            const float fx = bx_ - (float)bx0;
            const float fy = by_ - (float)by0;
            long i0 = bx0 - cx0, j0 = by0 - cy0;
            i0 = i0 < 0 ? 0 : (i0 > 5 ? 5 : i0);
            j0 = j0 < 0 ? 0 : (j0 > 5 ? 5 : j0);
            return sa_lerpf(sa_lerpf(acache[j0][i0], acache[j0][i0 + 1], fx),
                            sa_lerpf(acache[j0 + 1][i0], acache[j0 + 1][i0 + 1], fx), fy);
        };
        for (long bj = -2; bj <= 2; ++bj) {
            for (long bi = -2; bi <= 2; ++bi) {
                const long bx = qbx + bi, by = qby + bj;
                float best = -1.0f, fux = 0.0f, fuy = 0.0f;
                int fgx = 0, fgy = 0, any = 0;
                for (int sj = 0; sj < SA_VZ_SCATTER_SUBDIV; ++sj) {
                    for (int si = 0; si < SA_VZ_SCATTER_SUBDIV; ++si) {
                        const long gx = bx * SA_VZ_SCATTER_SUBDIV + si;
                        const long gy = by * SA_VZ_SCATTER_SUBDIV + sj;
                        const float wx = ((float)gx + 0.5f) * cell_x;
                        const float wy = ((float)gy + 0.5f) * cell_y;
                        const float prob = sa_voronize_density_probability(
                            analysis_at(wx, wy), c.p.strength, c.p.min_density);
                        const float ux = (float)gx + 0.5f +
                            (sa_rand3((int)gx, (int)gy, c.metric.seed + 3001) - 0.5f) * (c.metric.jitter * 2.0f);
                        const float uy = (float)gy + 0.5f +
                            (sa_rand3((int)gx, (int)gy, c.metric.seed + 4001) - 0.5f) * (c.metric.jitter * 2.0f);
                        if (prob > best) { best = prob; fux = ux; fuy = uy; fgx = (int)gx; fgy = (int)gy; }
                        if (sa_rand3((int)gx, (int)gy, c.metric.seed + 2003) < prob) {
                            sa_voronize_metric_scatter_tile_add(&tile, (int)gx, (int)gy, ux, uy);
                            any = 1;
                        }
                    }
                }
                if (!any) sa_voronize_metric_scatter_tile_add(&tile, fgx, fgy, fux, fuy);
            }
        }
        it = cache.emplace(key, tile).first;
    }
    const int qgx = (int)floorf(px / cell_x);
    const int qgy = (int)floorf(py / cell_y);
    sa_voronize_edge_from_metric_scatter_tile(&it->second, qgx, qgy, px, py,
                                              &g.d1, &g.border, &g.nx, &g.ny, &g.sx, &g.sy,
                                              nullptr, nullptr);
    return g;
}

// 画像の範囲を margin だけ広げた矩形 [x0,x1) x [y0,y1) を描きます。
static std::vector<float> render(const Image& src, const Params& p, int x0, int y0, int x1, int y1)
{
    Ctx c;
    c.src = &src;
    c.p = p;
    c.cx = (float)src.w * 0.5f;
    c.cy = (float)src.h * 0.5f;
    float sx, sy;
    sa_voronize_resolve_cell_size(p.size, p.independent, p.cell_w, p.cell_h, &sx, &sy);
    c.size_x = sa_voronize_clamp_render_axis(sx, 500.0f);
    c.size_y = sa_voronize_clamp_render_axis(sy, 500.0f);
    const int metric = sa_voronize_use_metric_path(p.independent, c.size_x, c.size_y);
    const int adaptive = p.density_mode != D_UNIFORM;
    float feat_scale;
    int avg_x = 1, avg_y = 1;
    if (metric) {
        sa_voronize_metric_init(&c.metric, c.size_x, c.size_y, p.jitter, p.seed);
        sa_voronize_metric_render_density_range(&c.density_min, &c.density_max);
        const float fx = adaptive ? c.metric.size_x * SA_VZ_DENSITY_BLOCK_MULT / SA_VZ_SCATTER_SUBDIV : c.metric.size_x;
        const float fy = adaptive ? c.metric.size_y * SA_VZ_DENSITY_BLOCK_MULT / SA_VZ_SCATTER_SUBDIV : c.metric.size_y;
        feat_scale = sa_voronize_feature_scale(fx, fy);
        sa_voronize_average_offsets(fx, fy, &avg_x, &avg_y);
    } else {
        sa_voronize_init(&c.vor, c.size_x, p.jitter, p.seed);
        scan_density_range(c, c.vor.size * SA_VZ_DENSITY_BLOCK_MULT);
        feat_scale = adaptive ? c.vor.size * SA_VZ_DENSITY_BLOCK_MULT / SA_VZ_SCATTER_SUBDIV : c.vor.size;
        const float o = feat_scale * 0.18f;
        avg_x = avg_y = (long)(o + 0.5f) > 0 ? (int)(o + 0.5f) : 1;
    }

    std::map<std::pair<long, long>, SaVoronizeScatterTile> legacy_cache;
    std::map<std::pair<long, long>, SaVoronizeMetricScatterTile> metric_cache;
    std::vector<float> out((size_t)(x1 - x0) * (y1 - y0) * 4);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float lx = (float)x + 0.5f - c.cx;
            const float ly = (float)y + 0.5f - c.cy;
            const Geometry g = metric ? geometry_metric(c, lx, ly, metric_cache)
                                      : geometry_legacy(c, lx, ly, legacy_cache);
            finish_pixel(c, x, y, g, feat_scale, avg_x, avg_y,
                         &out[((size_t)(y - y0) * (x1 - x0) + (x - x0)) * 4]);
        }
    }
    return out;
}

} // namespace ref

/* ------------------------------------------------------------------ */
/*  YMM4版 (シェーダーの共通部を C++ として実行)                        */
/* ------------------------------------------------------------------ */

namespace hlsl {

static const Image* g_src = nullptr;

struct SeedMap
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<unsigned char> flags;
};
static const SeedMap* g_seed = nullptr;
static long g_seed_reads = 0;

// Direct2D の入力の読み出し。範囲外は透明。
float4 SampleSource(float2 p)
{
    const int x = (int)std::floor(p.x), y = (int)std::floor(p.y);
    if (x < 0 || y < 0 || x >= g_src->w || y >= g_src->h) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float* q = g_src->at(x, y);
    return float4(q[0], q[1], q[2], q[3]);
}

bool SeedActive(int gx, int gy)
{
    ++g_seed_reads;
    if (gx < g_seed->x0 || gy < g_seed->y0 || gx >= g_seed->x1 || gy >= g_seed->y1) {
        std::printf("  seed map read outside its rect: (%d, %d)\n", gx, gy);
        std::abort();
    }
    return g_seed->flags[(size_t)(gy - g_seed->y0) * (g_seed->x1 - g_seed->x0) + (gx - g_seed->x0)] != 0;
}

#include "../Shaders/VoronizeCommon.hlsli"
#include "../Shaders/VoronizeCells.hlsli"

} // namespace hlsl

namespace port {

using namespace hlsl;

struct Options
{
    bool auto_levels = true;
    float smoothing = 0.0f;
};

static int floor_div(float a, float b) { return (int)std::floor(a / b); }

// VoronizeProcessor.cs と同じ矩形の計算。
struct Layout
{
    float size_x, size_y;
    int margin;
    int out_x0, out_y0, out_x1, out_y1;          // 出力矩形
    int seed_x0, seed_y0, seed_x1, seed_y1;      // 種マップの矩形 (サブセル)
    int block_x0, block_y0, block_x1, block_y1;  // 自動レベル補正のブロック範囲 (両端を含む)
    int tile, tiles_x, tiles_y;
};

static Layout layout(const Image& src, const Params& p, bool crop)
{
    Layout L;
    L.size_x = std::fmin(std::fmax(p.independent ? p.cell_w : p.size, 1.0f), 500.0f);
    L.size_y = std::fmin(std::fmax(p.independent ? p.cell_h : p.size, 1.0f), 500.0f);
    const bool adaptive = p.density_mode != D_UNIFORM;
    // 画像の外の画素は、自分の (解析) ブロックの種が最寄りより遠くなれないので、
    // ブロックの対角線 (1.5倍で丸める) を超えて離れると画像内の種の色を取りません。
    // 輪郭は境界の両側に太さの分だけ広がるので、その2倍を足します。
    const float reach = std::fmax(L.size_x, L.size_y) * (adaptive ? 4.0f : 1.0f);
    const bool draw_edge = p.edge_on && p.edge > 0.0f && p.edge_opacity > 0.0f;
    L.margin = crop ? 0 : (int)std::ceil(reach * 1.5f + (draw_edge ? 2.0f * p.edge : 0.0f)) + 2;
    L.out_x0 = -L.margin; L.out_y0 = -L.margin;
    L.out_x1 = src.w + L.margin; L.out_y1 = src.h + L.margin;

    const float ax = src.w * 0.5f, ay = src.h * 0.5f;
    const float bpx = L.size_x * 4.0f, bpy = L.size_y * 4.0f;
    const int qbx0 = floor_div(L.out_x0 + 0.5f - ax, bpx), qbx1 = floor_div(L.out_x1 - 0.5f - ax, bpx);
    const int qby0 = floor_div(L.out_y0 + 0.5f - ay, bpy), qby1 = floor_div(L.out_y1 - 0.5f - ay, bpy);
    L.seed_x0 = (qbx0 - 3) * 4; L.seed_x1 = (qbx1 + 4) * 4;
    L.seed_y0 = (qby0 - 3) * 4; L.seed_y1 = (qby1 + 4) * 4;

    L.block_x0 = floor_div(0.0f - ax, bpx) - 1;
    L.block_x1 = floor_div((float)src.w - ax, bpx) + 1;
    L.block_y0 = floor_div(0.0f - ay, bpy) - 1;
    L.block_y1 = floor_div((float)src.h - ay, bpy) + 1;
    const int nbx = L.block_x1 - L.block_x0 + 1, nby = L.block_y1 - L.block_y0 + 1;
    L.tile = std::max(1, (std::max(nbx, nby) + 31) / 32);
    L.tiles_x = (nbx + L.tile - 1) / L.tile;
    L.tiles_y = (nby + L.tile - 1) / L.tile;
    return L;
}

static std::vector<float> render(const Image& src, const Params& p, const Options& o, bool crop,
                                 Layout* out_layout, SeedMap* out_seed)
{
    g_src = &src;
    const Layout L = layout(src, p, crop);
    if (out_layout) *out_layout = L;
    const bool adaptive = p.density_mode != D_UNIFORM;
    const float4 rect(0.0f, 0.0f, (float)src.w, (float)src.h);
    const float2 anchor(src.w * 0.5f, src.h * 0.5f);

    static SeedMap seed;
    seed = SeedMap();
    if (adaptive) {
        DensitySettings d;
        d.imageRect = rect;
        d.anchor = anchor;
        d.blockPitch = float2(L.size_x * 4.0f, L.size_y * 4.0f);
        d.mode = p.density_mode - 1;
        d.invert = p.invert;
        d.seed = p.seed;
        d.detailGain = p.detail_gain;
        d.rangeMin = 0.0f;
        d.rangeMax = 1.0f;
        d.levelsBlack = p.black;
        d.levelsWhite = p.white;
        d.levelsGamma = p.gamma;
        d.strength = p.strength;
        d.minDensity = p.min_density;

        if (o.auto_levels) {
            // VoronizeRange.hlsl → VoronizeRangeReduce.hlsl
            float mn = 1.0f, mx = 0.0f;
            for (int ty = 0; ty < L.tiles_y; ++ty)
                for (int tx = 0; tx < L.tiles_x; ++tx) {
                    float tmn = 1.0f, tmx = 0.0f;
                    for (int j = 0; j < L.tile; ++j) {
                        const int by = std::min(L.block_y0 + ty * L.tile + j, L.block_y1);
                        for (int i = 0; i < L.tile; ++i) {
                            const int bx = std::min(L.block_x0 + tx * L.tile + i, L.block_x1);
                            const float a = saBlockRawDensity(bx, by, anchor, d.blockPitch, rect, d.mode, d.detailGain);
                            tmn = std::fmin(tmn, a);
                            tmx = std::fmax(tmx, a);
                        }
                    }
                    mn = std::fmin(mn, tmn);
                    mx = std::fmax(mx, tmx);
                }
            if (mx - mn > 1e-4f) { d.rangeMin = mn; d.rangeMax = mx; }
        }

        // VoronizeSeedMap.hlsl
        seed.x0 = L.seed_x0; seed.y0 = L.seed_y0; seed.x1 = L.seed_x1; seed.y1 = L.seed_y1;
        seed.flags.assign((size_t)(seed.x1 - seed.x0) * (seed.y1 - seed.y0), 0);
        for (int gy = seed.y0; gy < seed.y1; ++gy)
            for (int gx = seed.x0; gx < seed.x1; ++gx)
                seed.flags[(size_t)(gy - seed.y0) * (seed.x1 - seed.x0) + (gx - seed.x0)] =
                    saScatterActive(gx, gy, d) ? 1 : 0;
    }
    g_seed = &seed;
    if (out_seed) *out_seed = seed;

    // VoronizeRender.hlsl
    RenderSettings s;
    s.imageRect = rect;
    s.anchor = anchor;
    s.size = float2(L.size_x, L.size_y);
    s.edgeColor = float4(p.edge_rgb[0], p.edge_rgb[1], p.edge_rgb[2], p.edge_opacity);
    s.seed = p.seed;
    s.adaptive = adaptive ? 1 : 0;
    s.sampleMode = p.sample_mode - 1;
    s.outputMode = p.output_mode - 1;
    s.edgeOn = p.edge_on;
    s.edgeHard = p.edge_hard;
    s.independent = p.independent;
    s.jitter = p.jitter;
    s.edgeWidth = p.edge;
    s.amount = p.amount;
    s.smoothing = o.smoothing;

    const int w = L.out_x1 - L.out_x0, h = L.out_y1 - L.out_y0;
    std::vector<float> out((size_t)w * h * 4);
    for (int y = L.out_y0; y < L.out_y1; ++y)
        for (int x = L.out_x0; x < L.out_x1; ++x) {
            const float4 v = saRenderPixel(float2((float)x + 0.5f, (float)y + 0.5f), s);
            float* q = &out[((size_t)(y - L.out_y0) * w + (x - L.out_x0)) * 4];
            q[0] = v.x; q[1] = v.y; q[2] = v.z; q[3] = v.w;
        }
    return out;
}

} // namespace port

/* ------------------------------------------------------------------ */

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) { std::printf("  FAIL: %s\n", what.c_str()); ++g_fail; }
}

static float pixel_diff(const float* a, const float* b)
{
    float d = 0.0f;
    for (int k = 0; k < 4; ++k) d = std::fmax(d, std::fabs(a[k] - b[k]));
    return d;
}

// AE版の出力と比べます。浮動小数点の丸めで境界上の画素がまれに入れ替わるので、
// 大きくずれた画素がごく少数であることを確かめます。
static void compare_with_reference(const Image& src, const Params& p)
{
    port::Options o;
    o.auto_levels = !p.independent;   // AE版は幅と高さを別指定すると伸長しません
    port::Layout L;
    const std::vector<float> got = port::render(src, p, o, false, &L, nullptr);
    const std::vector<float> want = ref::render(src, p, L.out_x0, L.out_y0, L.out_x1, L.out_y1);

    const int w = L.out_x1 - L.out_x0, h = L.out_y1 - L.out_y0;
    long bad = 0;
    float worst = 0.0f;
    for (long i = 0; i < (long)w * h; ++i) {
        const float d = pixel_diff(&got[i * 4], &want[i * 4]);
        worst = std::fmax(worst, d);
        if (d > 1e-3f) ++bad;
    }
    const double ratio = (double)bad / ((double)w * h);
    std::printf("  %-44s %4dx%-4d mismatched %5ld (%.3f%%), worst %.2e\n",
                p.name, w, h, bad, ratio * 100.0, worst);
    check(ratio <= 0.002, std::string(p.name) + ": more than 0.2% of pixels differ from the AE version");
}

// 出力を広げた範囲の外側に、AE版で色の付いた画素が残っていないこと (画像モードのみ)。
static void check_margin_is_enough(const Image& src, const Params& p)
{
    port::Layout L = port::layout(src, p, false);
    const int big = L.margin + (int)std::ceil(std::fmax(L.size_x, L.size_y) * 4.0f * 3.5f);
    const std::vector<float> want = ref::render(src, p, -big, -big, src.w + big, src.h + big);
    const int w = src.w + 2 * big;
    long outside = 0;
    for (int y = -big; y < src.h + big; ++y)
        for (int x = -big; x < src.w + big; ++x) {
            if (x >= L.out_x0 && x < L.out_x1 && y >= L.out_y0 && y < L.out_y1) continue;
            if (want[((size_t)(y + big) * w + (x + big)) * 4 + 3] > 0.0f) ++outside;
        }
    check(outside == 0, std::string(p.name) + ": AE draws visible pixels outside the YMM4 output margin");
}

int main()
{
    const Image img = make_test_image(97, 71);

    std::vector<Params> cases;
    auto add = [&](const char* name, auto&& edit) { Params p; p.name = name; edit(p); cases.push_back(p); };

    add("uniform default", [](Params&) {});
    add("uniform avg outline", [](Params& p) {
        p.size = 7.3f; p.jitter = 0.3f; p.seed = 42; p.sample_mode = 2;
        p.edge = 2.0f; p.edge_opacity = 0.8f; p.edge_rgb[0] = 1.0f; });
    add("uniform hard outline", [](Params& p) { p.size = 9.0f; p.edge = 1.5f; p.edge_hard = 1; });
    add("uniform independent 15x6 avg", [](Params& p) {
        p.independent = 1; p.cell_w = 15.0f; p.cell_h = 6.0f; p.sample_mode = 2; p.edge = 1.0f; });
    add("uniform size 1", [](Params& p) { p.size = 1.0f; });
    add("uniform no jitter amount 60%", [](Params& p) { p.jitter = 0.0f; p.amount = 0.6f; });
    add("uniform borders", [](Params& p) { p.output_mode = 2; });
    add("uniform distance", [](Params& p) { p.output_mode = 3; p.size = 10.0f; });
    add("uniform seeds", [](Params& p) { p.output_mode = 4; });
    add("luminance", [](Params& p) { p.density_mode = D_LUMINANCE; p.size = 4.0f; });
    add("luminance sparse (min density 0)", [](Params& p) {
        p.density_mode = D_LUMINANCE; p.size = 3.0f; p.min_density = 0.0f; p.seed = 7; });
    add("saturation inverted levels", [](Params& p) {
        p.density_mode = D_SATURATION; p.size = 3.5f; p.invert = 1;
        p.black = 0.1f; p.white = 0.8f; p.gamma = 1.7f; });
    add("hue half strength avg", [](Params& p) {
        p.density_mode = D_HUE; p.size = 4.0f; p.strength = 0.5f; p.sample_mode = 2; });
    add("edge detect outline", [](Params& p) {
        p.density_mode = D_EDGE; p.size = 3.0f; p.detail_gain = 10.0f; p.edge = 1.0f; p.edge_hard = 1; });
    add("luminance independent 6x3", [](Params& p) {
        p.density_mode = D_LUMINANCE; p.independent = 1; p.cell_w = 6.0f; p.cell_h = 3.0f; });
    add("luminance independent 2.5x5 borders", [](Params& p) {
        p.density_mode = D_LUMINANCE; p.independent = 1; p.cell_w = 2.5f; p.cell_h = 5.0f; p.output_mode = 2; });
    add("luminance distance", [](Params& p) { p.density_mode = D_LUMINANCE; p.size = 3.0f; p.output_mode = 3; });
    add("luminance seeds", [](Params& p) { p.density_mode = D_LUMINANCE; p.size = 3.0f; p.output_mode = 4; });
    add("luminance size 1", [](Params& p) { p.density_mode = D_LUMINANCE; p.size = 1.0f; });

    std::printf("[1] matches the After Effects renderer\n");
    for (const Params& p : cases) compare_with_reference(img, p);

    std::printf("[2] the output margin holds every visible AE pixel\n");
    for (const Params& p : cases)
        if (p.output_mode == 1 && p.amount == 1.0f) check_margin_is_enough(img, p);

    std::printf("[3] cropping keeps exactly the source rect\n");
    {
        Params p; p.name = "crop"; p.density_mode = D_LUMINANCE; p.size = 4.0f;
        port::Layout L;
        port::render(img, p, port::Options(), true, &L, nullptr);
        check(L.out_x0 == 0 && L.out_y0 == 0 && L.out_x1 == img.w && L.out_y1 == img.h,
              "crop: output rect is the source rect");
    }

    std::printf("[4] every analysis block keeps at least one seed\n");
    {
        Params p; p.name = "sparse"; p.density_mode = D_LUMINANCE; p.size = 2.0f;
        p.min_density = 0.0f; p.strength = 1.0f; p.invert = 1;
        port::Layout L;
        hlsl::SeedMap seed;
        port::render(img, p, port::Options(), false, &L, &seed);
        long empty = 0, sparse = 0;
        for (int by = seed.y0 / 4; by < seed.y1 / 4; ++by)
            for (int bx = seed.x0 / 4; bx < seed.x1 / 4; ++bx) {
                int n = 0;
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i)
                        n += seed.flags[(size_t)(by * 4 + j - seed.y0) * (seed.x1 - seed.x0) + (bx * 4 + i - seed.x0)];
                if (n == 0) ++empty;
                if (n == 1) ++sparse;
            }
        check(empty == 0, "no analysis block is left without a seed");
        check(sparse > 0, "the sparse setting really thins some blocks down to one seed");
    }

    std::printf("[5] seam smoothing only touches pixels near a cell border\n");
    {
        for (int adaptive = 0; adaptive <= 1; ++adaptive) {
            Params p; p.name = adaptive ? "smooth adaptive" : "smooth uniform";
            p.size = adaptive ? 4.0f : 9.0f;
            p.density_mode = adaptive ? D_LUMINANCE : D_UNIFORM;
            port::Options hard, soft;
            soft.smoothing = 1.0f;
            port::Layout L;
            const std::vector<float> a = port::render(img, p, hard, false, &L, nullptr);
            const std::vector<float> b = port::render(img, p, soft, false, nullptr, nullptr);
            Params borders = p; borders.output_mode = 3;   // 距離表示で境界までの距離を読む
            const std::vector<float> dist = port::render(img, borders, hard, false, nullptr, nullptr);
            const float denom = std::fmax(std::sqrt(L.size_x * L.size_y) * 0.45f, 1.0f);
            long far_changed = 0, near_changed = 0;
            for (size_t i = 0; i < a.size() / 4; ++i) {
                const float border = dist[i * 4] * denom;
                const bool changed = pixel_diff(&a[i * 4], &b[i * 4]) > 1e-6f;
                if ((dist[i * 4] >= 1.0f || border >= 0.5f + 1e-4f) && changed) ++far_changed;
                if (border < 0.5f && changed) ++near_changed;
            }
            check(far_changed == 0, std::string(p.name) + ": pixels half a pixel inside a cell stay untouched");
            check(near_changed > 0, std::string(p.name) + ": pixels on a seam are blended");
        }
    }

    std::printf("[6] amount 0 returns the source unchanged\n");
    {
        for (int adaptive = 0; adaptive <= 1; ++adaptive) {
            Params p; p.name = adaptive ? "amount 0 adaptive" : "amount 0 uniform";
            p.amount = 0.0f; p.edge = 2.0f; p.density_mode = adaptive ? D_LUMINANCE : D_UNIFORM;
            port::Layout L;
            const std::vector<float> got = port::render(img, p, port::Options(), false, &L, nullptr);
            const int w = L.out_x1 - L.out_x0;
            long wrong = 0;
            for (int y = L.out_y0; y < L.out_y1; ++y)
                for (int x = L.out_x0; x < L.out_x1; ++x) {
                    const float* g = &got[((size_t)(y - L.out_y0) * w + (x - L.out_x0)) * 4];
                    static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    const bool inside = x >= 0 && y >= 0 && x < img.w && y < img.h;
                    if (pixel_diff(g, inside ? img.at(x, y) : zero) != 0.0f) ++wrong;
                }
            check(wrong == 0, std::string(p.name) + ": output equals the source");
        }
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
