// 自動レベル補正の1段目。解析ブロックの生の密度値を tileSize x tileSize ずつまとめ、
// 最小値を r、最大値を g に書き出します。出力の1画素が1タイルです。
#define D2D_INPUT0_COMPLEX
#define D2D_ENTRY main
#include <d2d1effecthelpers.hlsli>
#include "VoronizeSampling.hlsli"

float4 imageRect;  // 入力画像の範囲 (シーン座標)
float4 grid;       // xy: 格子の原点, zw: 解析ブロックの大きさ (px)
int blockOriginX;  // 対象ブロックの範囲 (両端を含む)
int blockOriginY;
int blockLastX;
int blockLastY;
int tileSize;      // 1画素がまとめるブロック数 (一辺)
int densityMode;
float detailGain;
float padding0;

float4 SampleSource(float2 position)
{
    return SA_SAMPLE_INPUT_LEVEL0(0, position);
}

#include "VoronizeCommon.hlsli"

D2D_PS_ENTRY(main)
{
    float2 p = D2DGetScenePosition().xy;
    int tileX = (int)floor(p.x);
    int tileY = (int)floor(p.y);
    int n = max(tileSize, 1);
    float mn = 1.0f;
    float mx = 0.0f;
    LOOP for (int j = 0; j < n; ++j)
    {
        int by = min(blockOriginY + tileY * n + j, blockLastY);
        LOOP for (int i = 0; i < n; ++i)
        {
            int bx = min(blockOriginX + tileX * n + i, blockLastX);
            float a = saBlockRawDensity(bx, by, grid.xy, grid.zw, imageRect, densityMode, detailGain);
            mn = min(mn, a);
            mx = max(mx, a);
        }
    }
    return float4(mn, mx, 0.0f, 1.0f);
}
