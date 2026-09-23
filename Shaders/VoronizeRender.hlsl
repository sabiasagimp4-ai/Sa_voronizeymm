// ボロノイモザイクの本体。
// 入力0: 元画像, 入力1: 種マップ (密度モードのみ。均一モードでは未使用)
#define D2D_INPUT0_COMPLEX
#define D2D_INPUT1_COMPLEX
#define D2D_ENTRY main
#include <d2d1effecthelpers.hlsli>
#include "VoronizeSampling.hlsli"

float4 imageRect;   // 入力画像の範囲 (シーン座標)
float4 grid;        // xy: 格子の原点, zw: セルの幅と高さ (px)
float4 edgeColor;   // rgb: 輪郭の色, w: 輪郭の不透明度
int seed;
int adaptive;
int sampleMode;
int outputMode;
int edgeOn;
int edgeHard;
int independentSize;
float jitter;
float edgeWidth;
float amount;
float smoothing;

float4 SampleSource(float2 position)
{
    return SA_SAMPLE_INPUT_LEVEL0(0, position);
}

bool SeedActive(int gx, int gy)
{
    return SA_SAMPLE_INPUT_LEVEL0(1, float2((float)gx + 0.5f, (float)gy + 0.5f)).w > 0.5f;
}

#include "VoronizeCommon.hlsli"
#include "VoronizeCells.hlsli"

D2D_PS_ENTRY(main)
{
    RenderSettings s;
    s.imageRect = imageRect;
    s.anchor = grid.xy;
    s.size = grid.zw;
    s.edgeColor = edgeColor;
    s.seed = seed;
    s.adaptive = adaptive;
    s.sampleMode = sampleMode;
    s.outputMode = outputMode;
    s.edgeOn = edgeOn;
    s.edgeHard = edgeHard;
    s.independent = independentSize;
    s.jitter = jitter;
    s.edgeWidth = edgeWidth;
    s.amount = amount;
    s.smoothing = smoothing;
    return saRenderPixel(D2DGetScenePosition().xy, s);
}
