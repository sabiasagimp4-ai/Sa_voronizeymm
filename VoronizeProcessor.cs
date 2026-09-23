using System.Numerics;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Player.Video;

namespace SaVoronize;

/// <summary>
/// エフェクトの組み立て。
///
///   均一モード:   元画像 → 本体
///   密度モード:   元画像 → [自動レベル補正 1段目 → 2段目] → 種マップ → 本体
///
/// 本体と種マップは入力を2つ持ちます。使わない側の入力には元画像をつなぎ、描画の対象から外します。
/// </summary>
internal sealed class VoronizeProcessor : IVideoEffectProcessor
{
    readonly IGraphicsDevicesAndContext devices;
    readonly VoronizeEffect item;
    readonly VoronizeRenderCustomEffect? render;
    readonly VoronizeSeedMapCustomEffect? seedMap;
    readonly VoronizeRangeCustomEffect? range;
    readonly VoronizeRangeReduceCustomEffect? rangeReduce;
    readonly ID2D1Image? output;
    readonly ID2D1Image? seedMapOutput;
    readonly ID2D1Image? rangeOutput;
    readonly ID2D1Image? rangeReduceOutput;
    ID2D1Image? input;
    bool adaptive;
    bool autoLevels;

    public VoronizeProcessor(IGraphicsDevicesAndContext devices, VoronizeEffect item)
    {
        this.devices = devices;
        this.item = item;

        //ShaderModel非対応環境ではパススルーする
        var render = new VoronizeRenderCustomEffect(devices);
        var seedMap = new VoronizeSeedMapCustomEffect(devices);
        var range = new VoronizeRangeCustomEffect(devices);
        var rangeReduce = new VoronizeRangeReduceCustomEffect(devices);
        if (!render.IsEnabled || !seedMap.IsEnabled || !range.IsEnabled || !rangeReduce.IsEnabled)
        {
            render.Dispose();
            seedMap.Dispose();
            range.Dispose();
            rangeReduce.Dispose();
            return;
        }

        this.render = render;
        this.seedMap = seedMap;
        this.range = range;
        this.rangeReduce = rangeReduce;
        rangeOutput = range.Output;
        rangeReduceOutput = rangeReduce.Output;
        seedMapOutput = seedMap.Output;
        output = render.Output;
        rangeReduce.SetInput(0, rangeOutput, true);
    }

    public ID2D1Image Output => output ?? input ?? throw new InvalidOperationException("入力が未設定です。");

    public void SetInput(ID2D1Image? input)
    {
        this.input = input;
        Connect();
    }

    public void ClearInput()
    {
        input = null;
        render?.SetInput(0, null, true);
        render?.SetInput(1, null, true);
        seedMap?.SetInput(0, null, true);
        seedMap?.SetInput(1, null, true);
        range?.SetInput(0, null, true);
    }

    void Connect()
    {
        if (render is null || seedMap is null || range is null)
            return;
        var levels = adaptive && autoLevels;
        render.SetInput(0, input, true);
        render.SetInput(1, adaptive ? seedMapOutput : input, true);
        seedMap.SetInput(0, adaptive ? input : null, true);
        seedMap.SetInput(1, levels ? rangeReduceOutput : (adaptive ? input : null), true);
        range.SetInput(0, levels ? input : null, true);
    }

    void SetMode(bool adaptive, bool autoLevels)
    {
        if (this.adaptive == adaptive && this.autoLevels == autoLevels)
            return;
        this.adaptive = adaptive;
        this.autoLevels = autoLevels;
        Connect();
    }

    public DrawDescription Update(EffectDescription effectDescription)
    {
        if (render is null || seedMap is null || range is null || rangeReduce is null || input is null)
            return effectDescription.DrawDescription;

        var frame = effectDescription.ItemPosition.Frame;
        var length = effectDescription.ItemDuration.Frame;
        var fps = effectDescription.FPS;

        var bounds = devices.DeviceContext.GetImageLocalBounds(input);
        if (!float.IsFinite(bounds.Left) || !float.IsFinite(bounds.Top) || !float.IsFinite(bounds.Right) || !float.IsFinite(bounds.Bottom)
            || bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
        {
            //無限大や空の矩形では格子が定まらないのでパススルーする
            render.Amount = 0;
            render.OutputMargin = 0;
            SetMode(false, false);
            return effectDescription.DrawDescription;
        }

        var independent = item.IndependentSize;
        var size = item.Size.GetValue(frame, length, fps);
        var sizeX = (float)Math.Clamp(independent ? item.CellWidth.GetValue(frame, length, fps) : size, 1d, 500d);
        var sizeY = (float)Math.Clamp(independent ? item.CellHeight.GetValue(frame, length, fps) : size, 1d, 500d);
        // 不規則さ100%で種がセル内のどこにでも置ける (セルの半分までずれる) 状態です。
        var jitter = (float)(Math.Clamp(item.Jitter.GetValue(frame, length, fps), 0d, 100d) / 100d * 0.5d);
        var seed = (int)Math.Round(Math.Clamp(item.Seed.GetValue(frame, length, fps), 0d, 10000d));
        var adaptive = item.DensityMode != VoronizeDensityMode.Uniform;
        var autoLevels = adaptive && item.AutoLevels;

        var edgeWidth = (float)Math.Max(item.EdgeWidth.GetValue(frame, length, fps), 0d);
        var edgeColor = item.EdgeColor;
        var edgeOpacity = (float)(Math.Clamp(item.EdgeOpacity.GetValue(frame, length, fps), 0d, 100d) / 100d) * (edgeColor.A / 255f);
        var drawEdge = item.EdgeOn && edgeWidth > 0 && edgeOpacity > 0;
        var amount = (float)(Math.Clamp(item.Amount.GetValue(frame, length, fps), 0d, 100d) / 100d);
        // 50%でおよそ1px幅のアンチエイリアスになります。
        var smoothing = (float)(Math.Clamp(item.CornerSmooth.GetValue(frame, length, fps), 0d, 100d) / 100d * 2d);

        // 格子の原点はアイテムの中心 (ローカル座標の原点) に置きます。
        const float AnchorX = 0f;
        const float AnchorY = 0f;
        var layout = VoronizeLayout.Create(
            bounds.Left, bounds.Top, bounds.Right, bounds.Bottom,
            AnchorX, AnchorY, sizeX, sizeY,
            adaptive, drawEdge, edgeWidth, item.CropToSourceBounds);
        var imageRect = new Vector4(bounds.Left, bounds.Top, bounds.Right, bounds.Bottom);

        render.ImageRect = imageRect;
        render.Grid = new Vector4(AnchorX, AnchorY, sizeX, sizeY);
        render.EdgeColor = new Vector4(edgeColor.R / 255f, edgeColor.G / 255f, edgeColor.B / 255f, edgeOpacity);
        render.Seed = seed;
        render.Adaptive = adaptive ? 1 : 0;
        render.SampleMode = (int)item.SampleMode;
        render.OutputMode = (int)item.OutputMode;
        render.EdgeOn = item.EdgeOn ? 1 : 0;
        render.EdgeHard = item.EdgeHard ? 1 : 0;
        render.IndependentSize = independent ? 1 : 0;
        render.Jitter = jitter;
        render.EdgeWidth = edgeWidth;
        render.Amount = amount;
        render.Smoothing = smoothing;
        render.OutputMargin = layout.Margin;

        if (adaptive)
        {
            var blockGrid = new Vector4(AnchorX, AnchorY, sizeX * VoronizeLayout.ScatterSubdiv, sizeY * VoronizeLayout.ScatterSubdiv);
            var densityMode = (int)item.DensityMode;
            var detailGain = (float)Math.Clamp(item.DetailGain.GetValue(frame, length, fps), 0.1d, 40d);

            seedMap.ImageRect = imageRect;
            seedMap.Grid = blockGrid;
            seedMap.Seed = seed;
            seedMap.DensityMode = densityMode;
            seedMap.DensityInvert = item.DensityInvert ? 1 : 0;
            seedMap.AutoLevels = autoLevels ? 1 : 0;
            seedMap.DetailGain = detailGain;
            seedMap.LevelsBlack = (float)(Math.Clamp(item.LevelsBlack.GetValue(frame, length, fps), 0d, 100d) / 100d);
            seedMap.LevelsWhite = (float)(Math.Clamp(item.LevelsWhite.GetValue(frame, length, fps), 0d, 100d) / 100d);
            seedMap.LevelsGamma = (float)Math.Clamp(item.LevelsGamma.GetValue(frame, length, fps), 0.1d, 9.99d);
            seedMap.DensityStrength = (float)(Math.Clamp(item.DensityStrength.GetValue(frame, length, fps), 0d, 100d) / 100d);
            seedMap.MinDensity = (float)(Math.Clamp(item.MinDensity.GetValue(frame, length, fps), 0d, 100d) / 100d);
            seedMap.SeedRect = new Vector4(layout.SeedRect.Left, layout.SeedRect.Top, layout.SeedRect.Right, layout.SeedRect.Bottom);

            if (autoLevels)
            {
                range.ImageRect = imageRect;
                range.Grid = blockGrid;
                range.BlockOriginX = layout.BlockOriginX;
                range.BlockOriginY = layout.BlockOriginY;
                range.BlockLastX = layout.BlockLastX;
                range.BlockLastY = layout.BlockLastY;
                range.TileSize = layout.TileSize;
                range.DensityMode = densityMode;
                range.DetailGain = detailGain;
                range.TileCountX = layout.TileCountX;
                range.TileCountY = layout.TileCountY;
                rangeReduce.TileCountX = layout.TileCountX;
                rangeReduce.TileCountY = layout.TileCountY;
            }
        }

        SetMode(adaptive, autoLevels);
        return effectDescription.DrawDescription;
    }

    public void Dispose()
    {
        ClearInput();
        rangeReduce?.SetInput(0, null, true);
        output?.Dispose();
        seedMapOutput?.Dispose();
        rangeReduceOutput?.Dispose();
        rangeOutput?.Dispose();
        render?.Dispose();
        seedMap?.Dispose();
        rangeReduce?.Dispose();
        range?.Dispose();
    }
}
