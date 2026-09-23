using Vortice;

namespace SaVoronize;

/// <summary>
/// 出力・種マップ・自動レベル補正の矩形の計算。tests/port_regression.cpp の port::layout と同じ式です。
/// </summary>
internal readonly record struct VoronizeLayout(
    int Margin,
    RawRect SeedRect,
    int BlockOriginX,
    int BlockOriginY,
    int BlockLastX,
    int BlockLastY,
    int TileSize,
    int TileCountX,
    int TileCountY)
{
    /// <summary>1解析ブロックの一辺のサブセル数 (= ブロックの大きさ / セルサイズ)。</summary>
    public const int ScatterSubdiv = 4;

    /// <summary>自動レベル補正の1段目の出力を、この数 x この数以下の画素に収めます。</summary>
    const int MaxTiles = 32;

    public static VoronizeLayout Create(
        float left, float top, float right, float bottom,
        float anchorX, float anchorY,
        float sizeX, float sizeY,
        bool adaptive, bool drawEdge, float edgeWidth, bool crop)
    {
        // 画像の外の画素は、自分が属する (解析) ブロックの種より遠い種を最寄りにできません。
        // ブロックの対角線 (1.5倍で丸める) より離れた画素は画像内の種の色を取らないので、そこまで広げます。
        // 輪郭は境界から太さの分だけ広がり、その境界も種から離れうるので太さの2倍を足します。
        var reach = Math.Max(sizeX, sizeY) * (adaptive ? ScatterSubdiv : 1);
        var margin = crop ? 0 : (int)Math.Ceiling(reach * 1.5f + (drawEdge ? 2f * edgeWidth : 0f)) + 2;

        var outLeft = (int)Math.Floor(left) - margin;
        var outTop = (int)Math.Floor(top) - margin;
        var outRight = (int)Math.Ceiling(right) + margin;
        var outBottom = (int)Math.Ceiling(bottom) + margin;

        // 各画素は自分の解析ブロックの前後2ブロックのサブセルを読みます。丸め誤差に備えて1ブロック余分に持ちます。
        var blockPitchX = sizeX * ScatterSubdiv;
        var blockPitchY = sizeY * ScatterSubdiv;
        var queryX0 = FloorDiv(outLeft + 0.5f - anchorX, blockPitchX);
        var queryX1 = FloorDiv(outRight - 0.5f - anchorX, blockPitchX);
        var queryY0 = FloorDiv(outTop + 0.5f - anchorY, blockPitchY);
        var queryY1 = FloorDiv(outBottom - 0.5f - anchorY, blockPitchY);
        var seedRect = new RawRect(
            (queryX0 - 3) * ScatterSubdiv,
            (queryY0 - 3) * ScatterSubdiv,
            (queryX1 + 4) * ScatterSubdiv,
            (queryY1 + 4) * ScatterSubdiv);

        // 自動レベル補正はAE版と同じく、画像にかかる解析ブロックとその外側1ブロックを見ます。
        var blockX0 = FloorDiv(left - anchorX, blockPitchX) - 1;
        var blockX1 = FloorDiv(right - anchorX, blockPitchX) + 1;
        var blockY0 = FloorDiv(top - anchorY, blockPitchY) - 1;
        var blockY1 = FloorDiv(bottom - anchorY, blockPitchY) + 1;
        var countX = blockX1 - blockX0 + 1;
        var countY = blockY1 - blockY0 + 1;
        var tile = Math.Max(1, (Math.Max(countX, countY) + MaxTiles - 1) / MaxTiles);

        return new VoronizeLayout(
            margin,
            seedRect,
            blockX0, blockY0, blockX1, blockY1,
            tile,
            (countX + tile - 1) / tile,
            (countY + tile - 1) / tile);
    }

    static int FloorDiv(float a, float b) => (int)Math.Floor(a / b);

    /// <summary>矩形を広げます。無限大に近い矩形はそのまま返します。</summary>
    public static RawRect Inflate(RawRect rect, int margin)
    {
        const int Limit = 1 << 28;
        if (margin <= 0 || rect.Left < -Limit || rect.Top < -Limit || rect.Right > Limit || rect.Bottom > Limit)
            return rect;
        return new RawRect(rect.Left - margin, rect.Top - margin, rect.Right + margin, rect.Bottom + margin);
    }
}
