using System.Runtime.InteropServices;
using Vortice;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Player.Video;

namespace SaVoronize;

/// <summary>自動レベル補正の2段目 (Shaders/VoronizeRangeReduce.hlsl)。全タイルの最小値と最大値を1画素にまとめます。</summary>
internal sealed class VoronizeRangeReduceCustomEffect(IGraphicsDevicesAndContext devices)
    : D2D1CustomShaderEffectBase(Create<VoronizeRangeReduceCustomEffect.Impl>(devices))
{
    public int TileCountX { set => SetValue((int)Impl.Properties.TileCountX, value); }
    public int TileCountY { set => SetValue((int)Impl.Properties.TileCountY, value); }

    [CustomEffect(1)]
    internal sealed class Impl : D2D1CustomShaderEffectImplBase<Impl>
    {
        Constants constants = new() { TileCountX = 1, TileCountY = 1 };

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.TileCountX)]
        public int TileCountX { get => constants.TileCountX; set { constants.TileCountX = Math.Max(value, 1); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.TileCountY)]
        public int TileCountY { get => constants.TileCountY; set { constants.TileCountY = Math.Max(value, 1); UpdateConstants(); } }

        public Impl() : base(ShaderResourceLoader.Get("VoronizeRangeReduce")) { }

        protected override void UpdateConstants() => drawInformation?.SetPixelShaderConstantBuffer(constants);

        public override void MapInputRectsToOutputRect(RawRect[] inputRects, RawRect[] inputOpaqueSubRects, out RawRect outputRect, out RawRect outputOpaqueSubRect)
        {
            inputRect = inputRects[0];
            outputRect = new RawRect(0, 0, 1, 1);
            outputOpaqueSubRect = default;
        }

        public override void MapOutputRectToInputRects(RawRect outputRect, RawRect[] inputRects)
        {
            inputRects[0] = inputRect;
        }

        [StructLayout(LayoutKind.Sequential, Size = 16)]
        struct Constants
        {
            public int TileCountX;
            public int TileCountY;
        }

        internal enum Properties
        {
            TileCountX = 0,
            TileCountY = 1,
        }
    }
}
