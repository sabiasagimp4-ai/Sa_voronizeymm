using System.Numerics;
using System.Runtime.InteropServices;
using Vortice;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Player.Video;

namespace SaVoronize;

/// <summary>
/// 密度モードの種マップ (Shaders/VoronizeSeedMap.hlsl)。出力の1画素が1サブセルで、種の有無をアルファに書きます。
/// 入力0は元画像、入力1は自動レベル補正の結果 (1x1)。自動補正がオフのときは入力1を読みません。
/// </summary>
internal sealed class VoronizeSeedMapCustomEffect(IGraphicsDevicesAndContext devices)
    : D2D1CustomShaderEffectBase(Create<VoronizeSeedMapCustomEffect.Impl>(devices))
{
    public Vector4 ImageRect { set => SetValue((int)Impl.Properties.ImageRect, value); }
    public Vector4 Grid { set => SetValue((int)Impl.Properties.Grid, value); }
    public int Seed { set => SetValue((int)Impl.Properties.Seed, value); }
    public int DensityMode { set => SetValue((int)Impl.Properties.DensityMode, value); }
    public int DensityInvert { set => SetValue((int)Impl.Properties.DensityInvert, value); }
    public int AutoLevels { set => SetValue((int)Impl.Properties.AutoLevels, value); }
    public float DetailGain { set => SetValue((int)Impl.Properties.DetailGain, value); }
    public float LevelsBlack { set => SetValue((int)Impl.Properties.LevelsBlack, value); }
    public float LevelsWhite { set => SetValue((int)Impl.Properties.LevelsWhite, value); }
    public float LevelsGamma { set => SetValue((int)Impl.Properties.LevelsGamma, value); }
    public float DensityStrength { set => SetValue((int)Impl.Properties.DensityStrength, value); }
    public float MinDensity { set => SetValue((int)Impl.Properties.MinDensity, value); }
    public Vector4 SeedRect { set => SetValue((int)Impl.Properties.SeedRect, value); }

    [CustomEffect(2)]
    internal sealed class Impl : D2D1CustomShaderEffectImplBase<Impl>
    {
        Constants constants;
        Vector4 seedRect = new(0, 0, 1, 1);
        RawRect rangeRect;

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.ImageRect)]
        public Vector4 ImageRect { get => constants.ImageRect; set { constants.ImageRect = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.Grid)]
        public Vector4 Grid { get => constants.Grid; set { constants.Grid = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.Seed)]
        public int Seed { get => constants.Seed; set { constants.Seed = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.DensityMode)]
        public int DensityMode { get => constants.DensityMode; set { constants.DensityMode = Math.Clamp(value, 0, 4); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.DensityInvert)]
        public int DensityInvert { get => constants.DensityInvert; set { constants.DensityInvert = value != 0 ? 1 : 0; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.AutoLevels)]
        public int AutoLevels { get => constants.AutoLevels; set { constants.AutoLevels = value != 0 ? 1 : 0; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.DetailGain)]
        public float DetailGain { get => constants.DetailGain; set { constants.DetailGain = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.LevelsBlack)]
        public float LevelsBlack { get => constants.LevelsBlack; set { constants.LevelsBlack = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.LevelsWhite)]
        public float LevelsWhite { get => constants.LevelsWhite; set { constants.LevelsWhite = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.LevelsGamma)]
        public float LevelsGamma { get => constants.LevelsGamma; set { constants.LevelsGamma = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.DensityStrength)]
        public float DensityStrength { get => constants.DensityStrength; set { constants.DensityStrength = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.MinDensity)]
        public float MinDensity { get => constants.MinDensity; set { constants.MinDensity = value; UpdateConstants(); } }

        /// <summary>出力するサブセルの範囲 (left, top, right, bottom)。シェーダーは読みません。</summary>
        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.SeedRect)]
        public Vector4 SeedRect { get => seedRect; set => seedRect = value; }

        RawRect fullOutputRect;

        public Impl() : base(ShaderResourceLoader.Get("VoronizeSeedMap")) { }

        protected override void UpdateConstants() => drawInformation?.SetPixelShaderConstantBuffer(constants);

        public override void MapInputRectsToOutputRect(RawRect[] inputRects, RawRect[] inputOpaqueSubRects, out RawRect outputRect, out RawRect outputOpaqueSubRect)
        {
            inputRect = inputRects[0];
            rangeRect = inputRects.Length > 1 ? inputRects[1] : default;
            // 出力はサブセルの添字の空間にあり、入力の位置とは無関係です。
            outputRect = new RawRect((int)seedRect.X, (int)seedRect.Y, (int)seedRect.Z, (int)seedRect.W);
            fullOutputRect = outputRect;
            outputOpaqueSubRect = default;
        }

        public override void MapOutputRectToInputRects(RawRect outputRect, RawRect[] inputRects)
        {
            // 解析ブロックの中心は出力の1画素から遠く離れうるので、常に入力全体を読みます。
            inputRects[0] = inputRect;
            if (inputRects.Length > 1)
                inputRects[1] = rangeRect;
        }

        public override RawRect MapInvalidRect(int inputIndex, RawRect invalidInputRect)
        {
            // どの入力画素も離れた出力画素に効きうるので、出力全体を無効にします。
            return fullOutputRect;
        }

        // HLSLの定数バッファと同じ並び (16バイト x 5)。
        [StructLayout(LayoutKind.Sequential, Size = 80)]
        struct Constants
        {
            public Vector4 ImageRect;
            public Vector4 Grid;
            public int Seed;
            public int DensityMode;
            public int DensityInvert;
            public int AutoLevels;
            public float DetailGain;
            public float LevelsBlack;
            public float LevelsWhite;
            public float LevelsGamma;
            public float DensityStrength;
            public float MinDensity;
        }

        internal enum Properties
        {
            ImageRect = 0,
            Grid = 1,
            Seed = 2,
            DensityMode = 3,
            DensityInvert = 4,
            AutoLevels = 5,
            DetailGain = 6,
            LevelsBlack = 7,
            LevelsWhite = 8,
            LevelsGamma = 9,
            DensityStrength = 10,
            MinDensity = 11,
            SeedRect = 12,
        }
    }
}
