// Sa_voronize のセル探索と1画素の仕上げ。
//
// VoronizeCommon.hlsli の後に取り込みます。取り込む側は次の2つを先に定義します。
//   float4 SampleSource(float2 scenePosition)  元画像 (乗算済みアルファ)
//   bool   SeedActive(int gx, int gy)           サブセルに種があるか (密度モード用)
//
// 座標は3種類あります。
//   シーン座標: Direct2Dの座標。画素中心は整数+0.5。
//   ローカル座標: シーン座標 - anchor。格子はここで0から始まります。
//   正規化座標: ローカル座標 / セルサイズ。幅と高さを別指定したときの最近傍はここで求めます。
#ifndef SA_VORONIZE_CELLS_HLSLI
#define SA_VORONIZE_CELLS_HLSLI

static const int OUTPUT_IMAGE = 0;
static const int OUTPUT_BORDERS = 1;
static const int OUTPUT_DISTANCE = 2;
static const int OUTPUT_SEEDS = 3;

static const int SAMPLE_CENTER = 0;
static const int SAMPLE_AVERAGE = 1;

struct CellResult
{
    float2 site;      // 最寄りの種 (ローカル座標, px)
    float2 neighbour; // 最寄りの境界の向こう側の種 (ローカル座標, px)
    float d1;         // 最寄りの種までの距離 (px)
    float border;     // 自分のセルの境界までの距離 (px)
};

// 格子セル (ix, iy) の種 (正規化座標)。不規則さは最大0.5で、種は自分のセルから出ません。
float2 saUniformSite(int ix, int iy, float jitter, int seed)
{
    float jx = (saRand3(ix, iy, seed) - 0.5f) * 2.0f * jitter;
    float jy = (saRand3(ix, iy, seed + 977) - 0.5f) * 2.0f * jitter;
    return float2((float)ix + 0.5f + jx, (float)iy + 0.5f + jy);
}

// 正規化座標の2点 a (最寄り) と b の垂直二等分線から、点 u までの距離 (px)。
// 分母が0 (同じ位置の種) のときは負の値を返します。
float saBisectorDistance(float2 u, float2 a, float2 b, float2 size)
{
    float vx = b.x - a.x;
    float vy = b.y - a.y;
    float numerator = abs(vx * (u.x - 0.5f * (a.x + b.x)) + vy * (u.y - 0.5f * (a.y + b.y)));
    float nx = vx / size.x;
    float ny = vy / size.y;
    float denominator = sqrt(nx * nx + ny * ny);
    if (denominator == 0.0f)
        return -1.0f;
    return numerator / denominator;
}

// 均一モード。近傍5x5の種から最寄りと境界距離を求めます。
// 種は自分のセルから出ないので、5x5で境界を作りうる種はすべて含まれます。
CellResult saUniformCells(float2 local, float2 size, float jitter, int seed)
{
    float2 u = float2(local.x / size.x, local.y / size.y);
    int qx = (int)floor(local.x * (1.0f / size.x));
    int qy = (int)floor(local.y * (1.0f / size.y));

    float2 sites[25];
    float best = SA_HUGE;
    int bi = 0;
    UNROLL for (int k = 0; k < 25; ++k)
    {
        float2 s = saUniformSite(qx - 2 + (k % 5), qy - 2 + (k / 5), jitter, seed);
        sites[k] = s;
        float dx = s.x - u.x;
        float dy = s.y - u.y;
        float d = dx * dx + dy * dy;
        if (d < best)
        {
            best = d;
            bi = k;
        }
    }

    float2 nearest = sites[bi];
    float minBorder = SA_HUGE;
    float2 other = nearest;
    UNROLL for (int m = 0; m < 25; ++m)
    {
        float b = saBisectorDistance(u, nearest, sites[m], size);
        if (m != bi && b >= 0.0f && b < minBorder)
        {
            minBorder = b;
            other = sites[m];
        }
    }

    CellResult r;
    r.site = float2(nearest.x * size.x, nearest.y * size.y);
    r.neighbour = float2(other.x * size.x, other.y * size.y);
    float ex = r.site.x - local.x;
    float ey = r.site.y - local.y;
    r.d1 = sqrt(ex * ex + ey * ey);
    r.border = minBorder < SA_HUGE ? minBorder : 0.0f;
    return r;
}

// 密度モードのサブセル (gx, gy) の種 (サブセル単位の正規化座標)。
float2 saScatterSite(int gx, int gy, float jitter, int seed)
{
    return float2((float)gx + 0.5f + (saRand3(gx, gy, seed + 3001) - 0.5f) * (jitter * 2.0f),
                  (float)gy + 0.5f + (saRand3(gx, gy, seed + 4001) - 0.5f) * (jitter * 2.0f));
}

// 半径 radius の正方形の輪の t 番目のマス (中心からのずれ)。輪のマス数は 8*radius。
int2 saRingOffset(int radius, int t)
{
    int side = 2 * radius;
    if (t < side)
        return int2(-radius + t, -radius);
    if (t < 2 * side)
        return int2(radius, -radius + (t - side));
    if (t < 3 * side)
        return int2(radius - (t - 2 * side), radius);
    return int2(-radius, radius - (t - 3 * side));
}

// 窓内で (qx, qy) を中心に半径 radius まで調べたとき、まだ調べていないサブセルまでの
// 距離の下限 (サブセル単位)。窓の端まで調べ終わった方向は数えません。
float saScatterOutsideLower(int qx, int qy, int radius, int originX, int originY, float2 local, float2 cell)
{
    int x0 = max(qx - radius, 0);
    int y0 = max(qy - radius, 0);
    int x1 = min(qx + radius, SCATTER_SIDE - 1);
    int y1 = min(qy + radius, SCATTER_SIDE - 1);
    float lower = SA_HUGE;
    if (x0 > 0)
        lower = min(lower, max((local.x - (float)(originX + x0) * cell.x) / cell.x, 0.0f));
    if (x1 + 1 < SCATTER_SIDE)
        lower = min(lower, max(((float)(originX + x1 + 1) * cell.x - local.x) / cell.x, 0.0f));
    if (y0 > 0)
        lower = min(lower, max((local.y - (float)(originY + y0) * cell.y) / cell.y, 0.0f));
    if (y1 + 1 < SCATTER_SIDE)
        lower = min(lower, max(((float)(originY + y1 + 1) * cell.y - local.y) / cell.y, 0.0f));
    return lower;
}

// 同じ距離のときは、AE版で候補を追加した順 (ブロック行 → ブロック列 → サブセル行 → サブセル列) を優先します。
int saScatterOrder(int x, int y)
{
    return ((y / SCATTER_SUBDIV) * 5 + (x / SCATTER_SUBDIV)) * (SCATTER_SUBDIV * SCATTER_SUBDIV) +
           (y % SCATTER_SUBDIV) * SCATTER_SUBDIV + (x % SCATTER_SUBDIV);
}

// 密度モード。画素が属する解析ブロックを中心とした5x5ブロック (20x20サブセル) の種から、
// 最寄りと境界距離を求めます。近いマスから輪状に調べ、残りが確実に遠いと分かった時点で止めます。
CellResult saScatterCells(float2 local, float2 size, float jitter, int seed)
{
    float2 blockPitch = float2(size.x * (float)SCATTER_SUBDIV, size.y * (float)SCATTER_SUBDIV);
    float2 cell = float2(blockPitch.x / (float)SCATTER_SUBDIV, blockPitch.y / (float)SCATTER_SUBDIV);
    int queryBx = (int)floor(local.x / blockPitch.x);
    int queryBy = (int)floor(local.y / blockPitch.y);
    int originX = (queryBx - 2) * SCATTER_SUBDIV;
    int originY = (queryBy - 2) * SCATTER_SUBDIV;
    int qx = clamp((int)floor(local.x / cell.x) - originX, 0, SCATTER_SIDE - 1);
    int qy = clamp((int)floor(local.y / cell.y) - originY, 0, SCATTER_SIDE - 1);
    float2 u = float2(local.x / cell.x, local.y / cell.y);
    float epsilon = 32.0f * SA_FLT_EPSILON * (abs(u.x) + abs(u.y) + 1.0f) + 1.0e-6f;
    int maxRadius = max(max(qx, SCATTER_SIDE - 1 - qx), max(qy, SCATTER_SIDE - 1 - qy));

    float best = SA_HUGE;
    int bestOrder = -1;
    float2 nearest = u;
    LOOP for (int radius = 0; radius <= maxRadius; ++radius)
    {
        int count = radius == 0 ? 1 : 8 * radius;
        LOOP for (int t = 0; t < count; ++t)
        {
            int2 o = saRingOffset(radius, t);
            int x = qx + o.x;
            int y = qy + o.y;
            if (x >= 0 && y >= 0 && x < SCATTER_SIDE && y < SCATTER_SIDE && SeedActive(originX + x, originY + y))
            {
                float2 s = saScatterSite(originX + x, originY + y, jitter, seed);
                float dx = s.x - u.x;
                float dy = s.y - u.y;
                float d = dx * dx + dy * dy;
                int order = saScatterOrder(x, y);
                if (d < best || (d == best && order < bestOrder))
                {
                    best = d;
                    bestOrder = order;
                    nearest = s;
                }
            }
        }
        if (bestOrder >= 0)
        {
            float outside = saScatterOutsideLower(qx, qy, radius, originX, originY, local, cell);
            if (outside >= SA_HUGE || outside > sqrt(best) + epsilon)
                break;
        }
    }

    CellResult r;
    r.site = float2(0.0f, 0.0f);
    r.neighbour = float2(0.0f, 0.0f);
    r.d1 = 0.0f;
    r.border = 0.0f;
    if (bestOrder < 0)
        return r;

    float nearestDistance = sqrt(best);
    float minCell = min(cell.x, cell.y);
    float minBorder = SA_HUGE;
    int borderOrder = -1;
    float2 other = nearest;
    LOOP for (int radius2 = 0; radius2 <= maxRadius; ++radius2)
    {
        int count2 = radius2 == 0 ? 1 : 8 * radius2;
        LOOP for (int t2 = 0; t2 < count2; ++t2)
        {
            int2 o = saRingOffset(radius2, t2);
            int x = qx + o.x;
            int y = qy + o.y;
            int order = saScatterOrder(x, y);
            if (x >= 0 && y >= 0 && x < SCATTER_SIDE && y < SCATTER_SIDE && order != bestOrder &&
                SeedActive(originX + x, originY + y))
            {
                float2 s = saScatterSite(originX + x, originY + y, jitter, seed);
                float b = saBisectorDistance(u, nearest, s, cell);
                if (b >= 0.0f && (b < minBorder || (b == minBorder && order < borderOrder)))
                {
                    minBorder = b;
                    borderOrder = order;
                    other = s;
                }
            }
        }
        if (minBorder < SA_HUGE)
        {
            float outside = saScatterOutsideLower(qx, qy, radius2, originX, originY, local, cell);
            if (outside >= SA_HUGE)
                break;
            // 距離 R の種と最寄り (距離 r) の二等分線は (R - r) / 2 より近づけません。
            if (minCell * (0.5f * (outside - nearestDistance) - epsilon) > minBorder)
                break;
        }
    }

    r.site = float2(nearest.x * cell.x, nearest.y * cell.y);
    r.neighbour = float2(other.x * cell.x, other.y * cell.y);
    float ex = r.site.x - local.x;
    float ey = r.site.y - local.y;
    r.d1 = sqrt(ex * ex + ey * ey);
    r.border = minBorder < SA_HUGE ? minBorder : 0.0f;
    return r;
}

struct RenderSettings
{
    float4 imageRect;
    float2 anchor;
    float2 size;          // セルの幅と高さ (px)
    float4 edgeColor;     // 輪郭の色 (rgb) と不透明度 (w)
    int seed;
    int adaptive;         // 0: 均一, 1: 密度マップで種を散布
    int sampleMode;
    int outputMode;
    int edgeOn;
    int edgeHard;
    int independent;      // 幅と高さを別指定
    float jitter;         // 0..0.5
    float edgeWidth;      // px
    float amount;         // 0..1
    float smoothing;      // 境界をなめらかにする幅 (px)。0で無効
};

// 輪郭の帯 [0, width) の被覆率。hard は0/1の段差で、smoothing>0 ならその幅でなめらかにつなぎます。
float saBand(float dist, float width, int hard, float smoothing)
{
    if (width <= 0.0f)
        return 0.0f;
    if (hard != 0)
    {
        if (smoothing <= 0.0f)
            return dist < width ? 1.0f : 0.0f;
        return saClamp01(0.5f - (dist - width) / smoothing);
    }
    return 1.0f - saSmoothstep(0.0f, width, dist);
}

// 平均サンプルのタップ間隔 (px)。
int saAverageOffset(float feature, int independent)
{
    int o = independent != 0 ? (int)(feature * 0.18f) : (int)(feature * 0.18f + 0.5f);
    return max(o, 1);
}

// セルの色。種が画像の外なら透明。平均 (近似) は種の周り8点との加重平均で色味だけを混ぜ、
// アルファは種の画素のものを使います (透明な縁で色が灰色に薄まらないように)。
float4 saSampleSite(float2 site, RenderSettings s)
{
    float2 scene = float2(site.x + s.anchor.x, site.y + s.anchor.y);
    float2 c = saPixelCenter(scene);
    float4 center = saSampleClamp(c, s.imageRect);
    bool inside = saPixelInside(c, s.imageRect);
    if (!inside)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (s.sampleMode != SAMPLE_AVERAGE)
        return center;

    float ox = (float)saAverageOffset(s.size.x, s.independent);
    float oy = (float)saAverageOffset(s.size.y, s.independent);
    float4 sum = float4(center.x * 4.0f, center.y * 4.0f, center.z * 4.0f, center.w * 4.0f);
    UNROLL for (int k = 0; k < 8; ++k)
    {
        float tx = k == 0 || k == 4 || k == 6 ? 1.0f : (k == 1 || k == 5 || k == 7 ? -1.0f : 0.0f);
        float ty = k == 2 || k == 4 || k == 5 ? 1.0f : (k == 3 || k == 6 || k == 7 ? -1.0f : 0.0f);
        float4 t = saSampleClamp(float2(c.x + tx * ox, c.y + ty * oy), s.imageRect);
        sum = float4(sum.x + t.x, sum.y + t.y, sum.z + t.z, sum.w + t.w);
    }
    float alphaSum = sum.w > 1e-4f ? sum.w : 1e-4f;
    return float4((sum.x / alphaSum) * center.w, (sum.y / alphaSum) * center.w, (sum.z / alphaSum) * center.w, center.w);
}

// 乗算済みの色 p に、輪郭色 rgb を被覆率 e で重ねます。
float4 saInk(float4 p, float4 rgb, float e)
{
    return float4(saLerp(p.x, rgb.x, e), saLerp(p.y, rgb.y, e), saLerp(p.z, rgb.z, e), saLerp(p.w, 1.0f, e));
}

float4 saRenderPixel(float2 scene, RenderSettings s)
{
    // 量0は元の映像そのもの。セルを探さずに返します。
    if (s.amount <= 0.0f)
        return saSampleZero(saPixelCenter(scene), s.imageRect);

    float2 local = float2(scene.x - s.anchor.x, scene.y - s.anchor.y);
    CellResult c;
    if (s.adaptive != 0)
        c = saScatterCells(local, s.size, s.jitter, s.seed);
    else
        c = saUniformCells(local, s.size, s.jitter, s.seed);

    float4 p;
    if (s.outputMode == OUTPUT_BORDERS)
    {
        // 輪郭の太さが0でも境界が見えるよう、既定の幅を使います。
        float v = saBand(c.border, s.edgeWidth > 0.0f ? s.edgeWidth : 1.5f, s.edgeHard, s.smoothing);
        p = float4(v, v, v, 1.0f);
    }
    else if (s.outputMode == OUTPUT_DISTANCE)
    {
        float feature = sqrt(s.size.x * s.size.y) * 0.45f;
        float v = saClamp01(c.border / (feature > 1.0f ? feature : 1.0f));
        p = float4(v, v, v, 1.0f);
    }
    else if (s.outputMode == OUTPUT_SEEDS)
    {
        float v = 1.0f - saSmoothstep(1.5f, 3.5f, c.d1);
        p = float4(v, v, v, 1.0f);
    }
    else
    {
        float4 own = saSampleSite(c.site, s);
        p = own;
        bool drawEdge = s.edgeOn != 0 && s.edgeWidth > 0.0f && s.edgeColor.w > 0.0f;
        if (s.smoothing > 0.0f || drawEdge)
        {
            float4 other = saSampleSite(c.neighbour, s);
            // 境界上で隣のセルと半々になるよう、境界からの距離に応じて隣の色を混ぜます。
            if (s.smoothing > 0.0f)
                p = saLerp4(other, own, saClamp01(0.5f + c.border / s.smoothing));
            if (drawEdge)
            {
                // 両側とも透明な境界には輪郭を引きません。
                float island = own.w > other.w ? own.w : other.w;
                float e = saBand(c.border, s.edgeWidth, s.edgeHard, s.smoothing) * s.edgeColor.w * island;
                if (e > 0.0f)
                    p = saInk(p, s.edgeColor, e);
            }
        }
    }

    if (s.amount < 1.0f)
        p = saLerp4(saSampleZero(saPixelCenter(scene), s.imageRect), p, s.amount);
    return p;
}

#endif
