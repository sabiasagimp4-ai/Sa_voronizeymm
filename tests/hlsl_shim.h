// Shaders/*.hlsli を C++ としてコンパイルするための最小限のHLSL互換層。
// シェーダー側は f 付きリテラル、.x/.y/.z/.w、構造体の戻り値だけで書かれているので、
// ここにあるベクトル型と組み込み関数だけで足ります。
#pragma once

#include <cmath>
#include <cstdint>

#define LOOP
#define UNROLL

namespace hlsl {

typedef std::uint32_t uint;

struct float2
{
    float x, y;
    float2() : x(0.0f), y(0.0f) {}
    float2(float x_, float y_) : x(x_), y(y_) {}
};

struct float4
{
    float x, y, z, w;
    float4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct int2
{
    int x, y;
    int2() : x(0), y(0) {}
    int2(int x_, int y_) : x(x_), y(y_) {}
};

inline uint asuint(int v) { return static_cast<uint>(v); }
inline float floor(float v) { return std::floor(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float abs(float v) { return std::fabs(v); }
inline float pow(float a, float b) { return ::powf(a, b); }
inline float min(float a, float b) { return ::fminf(a, b); }
inline float max(float a, float b) { return ::fmaxf(a, b); }
inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace hlsl
