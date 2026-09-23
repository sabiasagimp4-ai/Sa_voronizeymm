// 自動レベル補正の2段目。1段目の全タイルから最小値 (r) と最大値 (g) を1画素にまとめます。
#define D2D_INPUT0_COMPLEX
#define D2D_ENTRY main
#include <d2d1effecthelpers.hlsli>
#include "VoronizeSampling.hlsli"

int tileCountX;
int tileCountY;
int padding0;
int padding1;

D2D_PS_ENTRY(main)
{
    float mn = 1.0f;
    float mx = 0.0f;
    [loop] for (int j = 0; j < tileCountY; ++j)
    {
        [loop] for (int i = 0; i < tileCountX; ++i)
        {
            float4 t = SA_SAMPLE_INPUT_LEVEL0(0, float2((float)i + 0.5f, (float)j + 0.5f));
            mn = min(mn, t.x);
            mx = max(mx, t.y);
        }
    }
    return float4(mn, mx, 0.0f, 1.0f);
}
