using System.ComponentModel.DataAnnotations;
using System.Windows.Media;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Controls;
using YukkuriMovieMaker.Exo;
using YukkuriMovieMaker.ItemEditor.CustomVisibilityAttributes;
using YukkuriMovieMaker.Player.Video;
using YukkuriMovieMaker.Plugin.Effects;

namespace SaVoronize;

public enum VoronizeSampleMode
{
    [Display(Name = "中心")] Center = 0,
    [Display(Name = "平均（近似）")] ApproxAverage = 1,
}

public enum VoronizeDensityMode
{
    [Display(Name = "均一")] Uniform = 0,
    [Display(Name = "輝度")] Luminance = 1,
    [Display(Name = "彩度")] Saturation = 2,
    [Display(Name = "色相")] Hue = 3,
    [Display(Name = "エッジ検出")] Edge = 4,
}

public enum VoronizeOutputMode
{
    [Display(Name = "画像")] Image = 0,
    [Display(Name = "境界")] Borders = 1,
    [Display(Name = "距離")] Distance = 2,
    [Display(Name = "シード")] Seeds = 3,
}

[VideoEffect("Sa_voronize", ["フィルタ"], ["Sa_voronize", "ボロノイ", "voronoi", "モザイク", "mosaic", "セル"], IsAviUtlSupported = false)]
public sealed class VoronizeEffect : VideoEffectBase
{
    const string GridGroup = "グリッド";
    const string DensityGroup = "密度";
    const string OutlineGroup = "輪郭";
    const string OutputGroup = "出力";

    public override string Label => "Sa_voronize";

    // ---- グリッド ----

    [Display(GroupName = GridGroup, Name = "セルサイズ", Description = "セルのおおよその大きさ", Order = 0)]
    [AnimationSlider("F1", "px", 1, 120)]
    [ShowPropertyEditorWhen(nameof(IndependentSize), false)]
    public Animation Size { get; } = new(24, 1, 500);

    [Display(GroupName = GridGroup, Name = "幅と高さを別指定", Description = "オンにするとセル幅とセル高で縦横を別々に指定します", Order = 1)]
    [ToggleSlider]
    public bool IndependentSize { get => independentSize; set => Set(ref independentSize, value); }
    bool independentSize;

    [Display(GroupName = GridGroup, Name = "セル幅", Description = "セルの横の大きさ", Order = 2)]
    [AnimationSlider("F1", "px", 1, 120)]
    [ShowPropertyEditorWhen(nameof(IndependentSize), true)]
    public Animation CellWidth { get; } = new(24, 1, 500);

    [Display(GroupName = GridGroup, Name = "セル高", Description = "セルの縦の大きさ", Order = 3)]
    [AnimationSlider("F1", "px", 1, 120)]
    [ShowPropertyEditorWhen(nameof(IndependentSize), true)]
    public Animation CellHeight { get; } = new(24, 1, 500);

    [Display(GroupName = GridGroup, Name = "不規則さ", Description = "種を格子からずらす量。0%で規則正しい格子になります", Order = 4)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation Jitter { get; } = new(100, 0, 100);

    [Display(GroupName = GridGroup, Name = "乱数シード", Description = "模様の乱数を切り替えます。同じ値なら同じ模様になります", Order = 5)]
    [AnimationSlider("F0", "", 0, 100)]
    public Animation Seed { get; } = new(1, 0, 10000);

    [Display(GroupName = GridGroup, Name = "サンプル方法", Description = "セルの色を種の1点から取るか、種の周りの近似平均から取るか", Order = 6)]
    [EnumComboBox]
    public VoronizeSampleMode SampleMode { get => sampleMode; set => Set(ref sampleMode, value); }
    VoronizeSampleMode sampleMode = VoronizeSampleMode.Center;

    // ---- 密度 ----

    [Display(GroupName = DensityGroup, Name = "密度の基準", Description = "均一以外では、画像の明るさ・色・輪郭に応じてセルが集まります", Order = 10)]
    [EnumComboBox]
    public VoronizeDensityMode DensityMode { get => densityMode; set => Set(ref densityMode, value); }
    VoronizeDensityMode densityMode = VoronizeDensityMode.Uniform;

    [Display(GroupName = DensityGroup, Name = "マップを反転", Description = "密度マップの明暗を反転します", Order = 11)]
    [ToggleSlider]
    public bool DensityInvert { get => densityInvert; set => Set(ref densityInvert, value); }
    bool densityInvert;

    [Display(GroupName = DensityGroup, Name = "マップを自動補正", Description = "密度マップを画面内の最小〜最大に引き伸ばします。オフにすると動画でセル配置が揺れにくくなります", Order = 12)]
    [ToggleSlider]
    public bool AutoLevels { get => autoLevels; set => Set(ref autoLevels, value); }
    bool autoLevels = true;

    [Display(GroupName = DensityGroup, Name = "マップ黒点", Description = "ここより暗い部分を密度0側に振り切ります", Order = 13)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation LevelsBlack { get; } = new(0, 0, 100);

    [Display(GroupName = DensityGroup, Name = "マップ白点", Description = "ここより明るい部分を密度100側に振り切ります", Order = 14)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation LevelsWhite { get; } = new(100, 0, 100);

    [Display(GroupName = DensityGroup, Name = "マップガンマ", Description = "黒点と白点の間の中間調の寄り方。1.00で無変化", Order = 15)]
    [AnimationSlider("F2", "", 0.1, 3)]
    public Animation LevelsGamma { get; } = new(1, 0.1, 9.99);

    [Display(GroupName = DensityGroup, Name = "密度の強さ", Description = "密度マップをどれだけ強く反映するか。0%で均一と同じ配置になります", Order = 16)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation DensityStrength { get; } = new(100, 0, 100);

    [Display(GroupName = DensityGroup, Name = "最小密度", Description = "密度が最も低い部分に残すセルの量", Order = 17)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation MinDensity { get; } = new(10, 0, 100);

    [Display(GroupName = DensityGroup, Name = "エッジ検出の強さ", Description = "密度の基準がエッジ検出のときの感度", Order = 18)]
    [AnimationSlider("F1", "", 0.1, 40)]
    public Animation DetailGain { get; } = new(10, 0.1, 40);

    // ---- 輪郭 ----

    [Display(GroupName = OutlineGroup, Name = "輪郭を表示", Description = "セルの境界線を描きます", Order = 20)]
    [ToggleSlider]
    public bool EdgeOn { get => edgeOn; set => Set(ref edgeOn, value); }
    bool edgeOn = true;

    [Display(GroupName = OutlineGroup, Name = "輪郭の太さ", Description = "境界線の太さ。0では線が見えません", Order = 21)]
    [AnimationSlider("F1", "px", 0, 8)]
    [ShowPropertyEditorWhen(nameof(EdgeOn), true)]
    public Animation EdgeWidth { get; } = new(0, 0, 100);

    [Display(GroupName = OutlineGroup, Name = "輪郭の色", Description = "境界線の色", Order = 22)]
    [ColorPicker]
    [ShowPropertyEditorWhen(nameof(EdgeOn), true)]
    public Color EdgeColor { get => edgeColor; set => Set(ref edgeColor, value); }
    Color edgeColor = Colors.Black;

    [Display(GroupName = OutlineGroup, Name = "輪郭の不透明度", Description = "境界線の不透明度", Order = 23)]
    [AnimationSlider("F1", "%", 0, 100)]
    [ShowPropertyEditorWhen(nameof(EdgeOn), true)]
    public Animation EdgeOpacity { get; } = new(100, 0, 100);

    [Display(GroupName = OutlineGroup, Name = "硬い境界（アルファ０／１）", Description = "境界線の内側のぼかしをやめ、べた塗りの線にします", Order = 24)]
    [ToggleSlider]
    [ShowPropertyEditorWhen(nameof(EdgeOn), true)]
    public bool EdgeHard { get => edgeHard; set => Set(ref edgeHard, value); }
    bool edgeHard;

    // ---- 出力 ----

    [Display(GroupName = OutputGroup, Name = "出力方法", Description = "通常は画像。境界・距離・シードは確認用の表示です", Order = 30)]
    [EnumComboBox]
    public VoronizeOutputMode OutputMode { get => outputMode; set => Set(ref outputMode, value); }
    VoronizeOutputMode outputMode = VoronizeOutputMode.Image;

    [Display(GroupName = OutputGroup, Name = "量", Description = "元の映像との合成量", Order = 31)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation Amount { get; } = new(100, 0, 100);

    [Display(GroupName = OutputGroup, Name = "セルの角を滑らかに", Description = "セルの継ぎ目に出る階段状のギザギザを均す強さ。0%でオフ、50%でおよそ1pxのアンチエイリアス", Order = 32)]
    [AnimationSlider("F1", "%", 0, 100)]
    public Animation CornerSmooth { get; } = new(50, 0, 100);

    [Display(GroupName = OutputGroup, Name = "元画像の範囲で切り抜く", Description = "元の画像の外にはみ出したセルを切り落とします", Order = 33)]
    [ToggleSlider]
    public bool CropToSourceBounds { get => cropToSourceBounds; set => Set(ref cropToSourceBounds, value); }
    bool cropToSourceBounds;

    public override IEnumerable<string> CreateExoVideoFilters(int keyFrameIndex, ExoOutputDescription exoOutputDescription) => [];

    public override IVideoEffectProcessor CreateVideoEffect(IGraphicsDevicesAndContext devices) => new VoronizeProcessor(devices, this);

    protected override IEnumerable<IAnimatable> GetAnimatables() =>
    [
        Size, CellWidth, CellHeight, Jitter, Seed,
        LevelsBlack, LevelsWhite, LevelsGamma, DensityStrength, MinDensity, DetailGain,
        EdgeWidth, EdgeOpacity,
        Amount, CornerSmooth,
    ];
}
