using System.Numerics;
using System.Runtime.InteropServices;
using Vortice;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Player.Video;

namespace SaVoronize;

/// <summary>
/// ボロノイモザイクの本体 (Shaders/VoronizeRender.hlsl)。
/// 入力0は元画像、入力1は種マップ。均一モードでは入力1を読みません。
/// </summary>
internal sealed class VoronizeRenderCustomEffect(IGraphicsDevicesAndContext devices)
    : D2D1CustomShaderEffectBase(Create<VoronizeRenderCustomEffect.Impl>(devices))
{
    public Vector4 ImageRect { set => SetValue((int)Impl.Properties.ImageRect, value); }
    public Vector4 Grid { set => SetValue((int)Impl.Properties.Grid, value); }
    public Vector4 EdgeColor { set => SetValue((int)Impl.Properties.EdgeColor, value); }
    public int Seed { set => SetValue((int)Impl.Properties.Seed, value); }
    public int Adaptive { set => SetValue((int)Impl.Properties.Adaptive, value); }
    public int SampleMode { set => SetValue((int)Impl.Properties.SampleMode, value); }
    public int OutputMode { set => SetValue((int)Impl.Properties.OutputMode, value); }
    public int EdgeOn { set => SetValue((int)Impl.Properties.EdgeOn, value); }
    public int EdgeHard { set => SetValue((int)Impl.Properties.EdgeHard, value); }
    public int IndependentSize { set => SetValue((int)Impl.Properties.IndependentSize, value); }
    public float Jitter { set => SetValue((int)Impl.Properties.Jitter, value); }
    public float EdgeWidth { set => SetValue((int)Impl.Properties.EdgeWidth, value); }
    public float Amount { set => SetValue((int)Impl.Properties.Amount, value); }
    public float Smoothing { set => SetValue((int)Impl.Properties.Smoothing, value); }
    public int OutputMargin { set => SetValue((int)Impl.Properties.OutputMargin, value); }

    [CustomEffect(2)]
    internal sealed class Impl : D2D1CustomShaderEffectImplBase<Impl>
    {
        Constants constants = new() { Grid = new Vector4(0, 0, 24, 24), Amount = 1 };
        int outputMargin;
        RawRect seedRect;

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.ImageRect)]
        public Vector4 ImageRect { get => constants.ImageRect; set { constants.ImageRect = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.Grid)]
        public Vector4 Grid { get => constants.Grid; set { constants.Grid = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.EdgeColor)]
        public Vector4 EdgeColor { get => constants.EdgeColor; set { constants.EdgeColor = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.Seed)]
        public int Seed { get => constants.Seed; set { constants.Seed = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.Adaptive)]
        public int Adaptive { get => constants.Adaptive; set { constants.Adaptive = value != 0 ? 1 : 0; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.SampleMode)]
        public int SampleMode { get => constants.SampleMode; set { constants.SampleMode = Math.Clamp(value, 0, 1); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.OutputMode)]
        public int OutputMode { get => constants.OutputMode; set { constants.OutputMode = Math.Clamp(value, 0, 3); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.EdgeOn)]
        public int EdgeOn { get => constants.EdgeOn; set { constants.EdgeOn = value != 0 ? 1 : 0; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.EdgeHard)]
        public int EdgeHard { get => constants.EdgeHard; set { constants.EdgeHard = value != 0 ? 1 : 0; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.IndependentSize)]
        public int IndependentSize { get => constants.IndependentSize; set { constants.IndependentSize = value != 0 ? 1 : 0; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.Jitter)]
        public float Jitter { get => constants.Jitter; set { constants.Jitter = Math.Clamp(value, 0f, 0.5f); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.EdgeWidth)]
        public float EdgeWidth { get => constants.EdgeWidth; set { constants.EdgeWidth = Math.Max(value, 0f); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.Amount)]
        public float Amount { get => constants.Amount; set { constants.Amount = Math.Clamp(value, 0f, 1f); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.Smoothing)]
        public float Smoothing { get => constants.Smoothing; set { constants.Smoothing = Math.Max(value, 0f); UpdateConstants(); } }

        /// <summary>出力を入力より広げる量 (px)。シェーダーは読みません。</summary>
        [CustomEffectProperty(PropertyType.Int32, (int)Properties.OutputMargin)]
        public int OutputMargin { get => outputMargin; set => outputMargin = Math.Max(value, 0); }

        RawRect fullOutputRect;

        public Impl() : base(ShaderResourceLoader.Get("VoronizeRender")) { }

        protected override void UpdateConstants() => drawInformation?.SetPixelShaderConstantBuffer(constants);

        public override void MapInputRectsToOutputRect(RawRect[] inputRects, RawRect[] inputOpaqueSubRects, out RawRect outputRect, out RawRect outputOpaqueSubRect)
        {
            inputRect = inputRects[0];
            seedRect = inputRects.Length > 1 ? inputRects[1] : default;
            // 画像の端にかかるセルは外側まで広がるので、その分だけ出力を広げます。
            outputRect = VoronizeLayout.Inflate(inputRect, outputMargin);
            fullOutputRect = outputRect;
            outputOpaqueSubRect = default;
        }

        public override void MapOutputRectToInputRects(RawRect outputRect, RawRect[] inputRects)
        {
            // 種は出力の画素から離れた位置にあり、その色を読むので常に入力全体を要求します。
            inputRects[0] = inputRect;
            if (inputRects.Length > 1)
                inputRects[1] = seedRect;
        }

        public override RawRect MapInvalidRect(int inputIndex, RawRect invalidInputRect)
        {
            // どの入力画素も離れた出力画素に効きうるので、出力全体を無効にします。
            return fullOutputRect;
        }

        // HLSLの定数バッファと同じ並び (float4 x 3 の後に4バイトずつ詰めて92バイト、16バイト単位に切り上げ)。
        [StructLayout(LayoutKind.Sequential, Size = 96)]
        struct Constants
        {
            public Vector4 ImageRect;
            public Vector4 Grid;
            public Vector4 EdgeColor;
            public int Seed;
            public int Adaptive;
            public int SampleMode;
            public int OutputMode;
            public int EdgeOn;
            public int EdgeHard;
            public int IndependentSize;
            public float Jitter;
            public float EdgeWidth;
            public float Amount;
            public float Smoothing;
        }

        internal enum Properties
        {
            ImageRect = 0,
            Grid = 1,
            EdgeColor = 2,
            Seed = 3,
            Adaptive = 4,
            SampleMode = 5,
            OutputMode = 6,
            EdgeOn = 7,
            EdgeHard = 8,
            IndependentSize = 9,
            Jitter = 10,
            EdgeWidth = 11,
            Amount = 12,
            Smoothing = 13,
            OutputMargin = 14,
        }
    }
}
