// Direct2Dの入力をミップ0で読むマクロ。
//
// d2d1effecthelpers.hlsli の D2DSampleInputAtPosition は Sample (勾配を使う命令) なので、
// 画素ごとに回数が変わるループの中では使えません。同じ座標変換で SampleLevel を呼びます。
// 入力テクスチャはミップが1段だけなので、結果は D2DSampleInputAtPosition と同じです。
#ifndef SA_VORONIZE_SAMPLING_HLSLI
#define SA_VORONIZE_SAMPLING_HLSLI

#define SA_SAMPLE_INPUT_LEVEL0(index, position) \
    InputTexture##index.SampleLevel(InputSampler##index, \
        __d2dstatic_uv##index.xy + __d2dstatic_uv##index.zw * ((position) - __d2dstatic_scenePos.xy), 0)

#endif
