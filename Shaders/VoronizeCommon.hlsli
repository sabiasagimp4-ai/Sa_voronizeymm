// Sa_voronize共通処理。ハッシュ、密度マップ、画素サンプリングを扱います。
//
// このファイルはHLSLとしてシェーダーに取り込まれるほか、tests/port_regression.cpp で
// C++としてもコンパイルされ、After Effects版 (Sa_voronizeMath.h) と結果を比較します。
// そのため次の書き方に揃えています。
//   - 浮動小数点リテラルには必ず f を付ける
//   - out/inout 引数やスウィズルを使わず、構造体の戻り値と .x/.y/.z/.w だけで書く
//   - 取り込む側が SampleSource(float2 scenePosition) を先に定義する
//     (シェーダーではミップ0の SampleLevel、テストでは配列の読み出し)
#ifndef SA_VORONIZE_COMMON_HLSLI
#define SA_VORONIZE_COMMON_HLSLI

#ifndef LOOP
#define LOOP [loop]
#endif
#ifndef UNROLL
#define UNROLL [unroll]
#endif

static const int DENSITY_UNIFORM = 0;
static const int DENSITY_LUMINANCE = 1;
static const int DENSITY_SATURATION = 2;
static const int DENSITY_HUE = 3;
static const int DENSITY_EDGE = 4;

// 1解析ブロックの一辺にあるサブセル数。ブロックの一辺はセルサイズの4倍。
static const int SCATTER_SUBDIV = 4;
// 探索窓は5x5ブロック = 20x20サブセル。
static const int SCATTER_SIDE = 20;

static const float SA_FLT_EPSILON = 1.192092896e-7f;
static const float SA_HUGE = 1e30f;

// ---- 決定的ハッシュ (SaMath.h の sa_hash_u32 / sa_hash2 / sa_rand3 と同じ) ----

uint saHashU32(uint x)
{
    x = (x ^ 61u) ^ (x >> 16u);
    x = x + (x << 3u);
    x = x ^ (x >> 4u);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}

uint saHash2(int x, int y)
{
    return saHashU32(asuint(x) * 0x9e3779b9u ^ saHashU32(asuint(y)));
}

// [0,1) の一様乱数。
float saRand3(int x, int y, int z)
{
    uint h = saHashU32(saHash2(x, y) ^ saHashU32(asuint(z) * 0x85ebca6bu));
    return (float)(h >> 8u) * (1.0f / 16777216.0f);
}

// ---- スカラー補助 ----

float saLerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

float4 saLerp4(float4 a, float4 b, float t)
{
    return float4(saLerp(a.x, b.x, t), saLerp(a.y, b.y, t), saLerp(a.z, b.z, t), saLerp(a.w, b.w, t));
}

float saClamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// 端点を呼び出し側で並べた smoothstep。幅0のときは段差になります。
float saSmoothstep(float e0, float e1, float x)
{
    float d = e1 - e0;
    if (d == 0.0f)
        return x < e0 ? 0.0f : 1.0f;
    float t = saClamp01((x - e0) / d);
    return t * t * (3.0f - 2.0f * t);
}

float saLuminance(float4 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

float4 saUnpremultiply(float4 c)
{
    if (c.w == 1.0f)
        return c;
    if (c.w <= 1e-6f)
        return float4(0.0f, 0.0f, 0.0f, c.w);
    float k = 1.0f / c.w;
    return float4(c.x * k, c.y * k, c.z * k, c.w);
}

// ---- 画素サンプリング ----
// imageRect は入力画像の範囲 (left, top, right, bottom)。整数の画素格子上にある前提です。

// 点を含む画素の中心。
float2 saPixelCenter(float2 p)
{
    return float2(floor(p.x) + 0.5f, floor(p.y) + 0.5f);
}

bool saPixelInside(float2 center, float4 imageRect)
{
    return center.x > imageRect.x && center.x < imageRect.z &&
           center.y > imageRect.y && center.y < imageRect.w;
}

// 画像の端の画素に寄せた中心 (AE版 sa_sample_clamp と同じ扱い)。
float2 saClampCenter(float2 center, float4 imageRect)
{
    float loX = imageRect.x + 0.5f;
    float loY = imageRect.y + 0.5f;
    float hiX = max(loX, imageRect.z - 0.5f);
    float hiY = max(loY, imageRect.w - 0.5f);
    float x = min(max(center.x, loX), hiX);
    float y = min(max(center.y, loY), hiY);
    return float2(floor(x) + 0.5f, floor(y) + 0.5f);
}

float4 saSampleClamp(float2 center, float4 imageRect)
{
    return SampleSource(saClampCenter(center, imageRect));
}

// 画像の外は透明として読みます (AE版 sa_sample_zero と同じ扱い)。
float4 saSampleZero(float2 center, float4 imageRect)
{
    float4 c = SampleSource(saClampCenter(center, imageRect));
    return saPixelInside(center, imageRect) ? c : float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// ---- 密度マップ ----

// 1点の生の密度値。エッジ検出は上下左右4点の輝度勾配、ほかは1点のHSV/輝度。
float saRawDensity(float2 samplePosition, float4 imageRect, int mode, float detailGain)
{
    float2 c = saPixelCenter(samplePosition);
    if (mode == DENSITY_EDGE)
    {
        float4 l = saUnpremultiply(saSampleClamp(float2(c.x - 1.0f, c.y), imageRect));
        float4 r = saUnpremultiply(saSampleClamp(float2(c.x + 1.0f, c.y), imageRect));
        float4 u = saUnpremultiply(saSampleClamp(float2(c.x, c.y - 1.0f), imageRect));
        float4 d = saUnpremultiply(saSampleClamp(float2(c.x, c.y + 1.0f), imageRect));
        float gx = saLuminance(r) - saLuminance(l);
        float gy = saLuminance(d) - saLuminance(u);
        return saClamp01(sqrt(gx * gx + gy * gy) * detailGain);
    }

    float4 s = saUnpremultiply(saSampleClamp(c, imageRect));
    float mx = max(s.x, max(s.y, s.z));
    float mn = min(s.x, min(s.y, s.z));
    if (mode == DENSITY_SATURATION)
        return mx > 1e-6f ? (mx - mn) / mx : 0.0f;
    if (mode == DENSITY_HUE)
    {
        float chroma = mx - mn;
        if (chroma <= 1e-6f)
            return 0.0f;
        float h;
        if (mx == s.x)
        {
            h = (s.y - s.z) / chroma;
            if (h < 0.0f)
                h += 6.0f;
        }
        else if (mx == s.y)
        {
            h = (s.z - s.x) / chroma + 2.0f;
        }
        else
        {
            h = (s.x - s.y) / chroma + 4.0f;
        }
        return h * (1.0f / 6.0f);
    }
    return saLuminance(s);
}

// 解析ブロック (bx, by) の中心で読んだ生の密度値。
float saBlockRawDensity(int bx, int by, float2 anchor, float2 blockPitch, float4 imageRect, int mode, float detailGain)
{
    float2 p = float2(((float)bx + 0.5f) * blockPitch.x + anchor.x,
                      ((float)by + 0.5f) * blockPitch.y + anchor.y);
    return saRawDensity(p, imageRect, mode, detailGain);
}

// レベル補正。黒点0、白点1、ガンマ1で無変化。
float saApplyLevels(float a, float black, float white, float gamma)
{
    black = saClamp01(black);
    white = saClamp01(white);
    if (white < black + 1e-4f)
        white = black + 1e-4f;
    float t = saClamp01((a - black) / (white - black));
    if (gamma > 1e-3f && gamma != 1.0f && t > 0.0f)
        t = pow(t, 1.0f / gamma);
    return t;
}

// 密度値 a に対するセル種の生存確率。strength=0 なら常に1。
float saDensityProbability(float a, float strength, float minDensity)
{
    a = saClamp01(a);
    strength = saClamp01(strength);
    minDensity = saClamp01(minDensity);
    float shaped = a > 0.0f ? pow(a, 0.65f) : 0.0f;
    float floorProbability = saLerp(minDensity, 1.0f, shaped);
    return saLerp(1.0f, floorProbability, strength);
}

struct DensitySettings
{
    float4 imageRect;
    float2 anchor;
    float2 blockPitch;
    int mode;
    int invert;
    int seed;
    float detailGain;
    float rangeMin;
    float rangeMax;
    float levelsBlack;
    float levelsWhite;
    float levelsGamma;
    float strength;
    float minDensity;
};

// 伸長 → レベル補正 → 反転 を通した0..1の密度値。
float saBlockAnalysis(int bx, int by, DensitySettings s)
{
    float raw = saBlockRawDensity(bx, by, s.anchor, s.blockPitch, s.imageRect, s.mode, s.detailGain);
    float range = s.rangeMax - s.rangeMin;
    float stretched = range > 1e-4f ? saClamp01((raw - s.rangeMin) / range) : 0.5f;
    float a = saApplyLevels(stretched, s.levelsBlack, s.levelsWhite, s.levelsGamma);
    return s.invert != 0 ? 1.0f - a : a;
}

int saFloorDiv4(int v)
{
    return (int)floor((float)v * 0.25f);
}

// サブセル (gx, gy) がセル種を持つか。
// 各サブセルは自分の中心で双線形補間した密度から確率を求めて生き残りを決めます。
// ブロック内の16サブセルがすべて落ちた場合は、確率が最大のサブセルを1つだけ残します。
// これで各ブロックに必ず1つ以上の種があり、最近傍探索の範囲が有限に保たれます。
bool saScatterActive(int gx, int gy, DensitySettings s)
{
    int bx = saFloorDiv4(gx);
    int by = saFloorDiv4(gy);
    float cellX = s.blockPitch.x / (float)SCATTER_SUBDIV;
    float cellY = s.blockPitch.y / (float)SCATTER_SUBDIV;

    // このブロックのサブセル中心の補間には、前後1ブロックずつの解析値で足ります。
    float a[9];
    UNROLL for (int j = 0; j < 3; ++j)
    {
        UNROLL for (int i = 0; i < 3; ++i)
            a[j * 3 + i] = saBlockAnalysis(bx - 1 + i, by - 1 + j, s);
    }

    float bestProbability = -1.0f;
    int fallbackX = 0;
    int fallbackY = 0;
    bool anyKept = false;
    bool self = false;
    UNROLL for (int sj = 0; sj < SCATTER_SUBDIV; ++sj)
    {
        UNROLL for (int si = 0; si < SCATTER_SUBDIV; ++si)
        {
            int cx = bx * SCATTER_SUBDIV + si;
            int cy = by * SCATTER_SUBDIV + sj;
            float wx = ((float)cx + 0.5f) * cellX;
            float wy = ((float)cy + 0.5f) * cellY;
            float blockX = wx / s.blockPitch.x - 0.5f;
            float blockY = wy / s.blockPitch.y - 0.5f;
            float bx0 = floor(blockX);
            float by0 = floor(blockY);
            float fx = blockX - bx0;
            float fy = blockY - by0;
            int i0 = clamp((int)bx0 - (bx - 1), 0, 1);
            int j0 = clamp((int)by0 - (by - 1), 0, 1);
            float a00 = a[j0 * 3 + i0];
            float a10 = a[j0 * 3 + i0 + 1];
            float a01 = a[(j0 + 1) * 3 + i0];
            float a11 = a[(j0 + 1) * 3 + i0 + 1];
            float analysis = saLerp(saLerp(a00, a10, fx), saLerp(a01, a11, fx), fy);
            float probability = saDensityProbability(analysis, s.strength, s.minDensity);
            bool keep = saRand3(cx, cy, s.seed + 2003) < probability;
            if (probability > bestProbability)
            {
                bestProbability = probability;
                fallbackX = cx;
                fallbackY = cy;
            }
            anyKept = anyKept || keep;
            if (cx == gx && cy == gy)
                self = keep;
        }
    }
    return self || (!anyKept && fallbackX == gx && fallbackY == gy);
}

#endif
