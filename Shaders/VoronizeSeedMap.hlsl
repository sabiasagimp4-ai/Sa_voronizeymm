// 密度モードの種マップ。出力の1画素が1サブセルで、種があれば不透明、なければ透明です。
// 入力0: 元画像, 入力1: 自動レベル補正の最小値/最大値 (1x1。自動補正がオフなら未使用)
#define D2D_INPUT0_COMPLEX
#define D2D_INPUT1_COMPLEX
#define D2D_ENTRY main
#include <d2d1effecthelpers.hlsli>
#include "VoronizeSampling.hlsli"

float4 imageRect;   // 入力画像の範囲 (シーン座標)
float4 grid;        // xy: 格子の原点, zw: 解析ブロックの大きさ (px)
int seed;
int densityMode;
int densityInvert;
int autoLevels;
float detailGain;
float levelsBlack;
float levelsWhite;
float levelsGamma;
float densityStrength;
float minDensity;
float padding0;
float padding1;

float4 SampleSource(float2 position)
{
    return SA_SAMPLE_INPUT_LEVEL0(0, position);
}

#include "VoronizeCommon.hlsli"

D2D_PS_ENTRY(main)
{
    float2 p = D2DGetScenePosition().xy;

    DensitySettings s;
    s.imageRect = imageRect;
    s.anchor = grid.xy;
    s.blockPitch = grid.zw;
    s.mode = densityMode;
    s.invert = densityInvert;
    s.seed = seed;
    s.detailGain = detailGain;
    s.rangeMin = 0.0f;
    s.rangeMax = 1.0f;
    s.levelsBlack = levelsBlack;
    s.levelsWhite = levelsWhite;
    s.levelsGamma = levelsGamma;
    s.strength = densityStrength;
    s.minDensity = minDensity;

    if (autoLevels != 0)
    {
        float4 range = SA_SAMPLE_INPUT_LEVEL0(1, float2(0.5f, 0.5f));
        // 平坦な画像は伸長せずに0..1のまま使います。
        if (range.y - range.x > 1e-4f)
        {
            s.rangeMin = range.x;
            s.rangeMax = range.y;
        }
    }

    bool active = saScatterActive((int)floor(p.x), (int)floor(p.y), s);
    return active ? float4(1.0f, 1.0f, 1.0f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
}
