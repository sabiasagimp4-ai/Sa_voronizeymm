using System.Numerics;
using System.Runtime.InteropServices;
using Vortice;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Player.Video;

namespace SaVoronize;

/// <summary>自動レベル補正の1段目 (Shaders/VoronizeRange.hlsl)。解析ブロックの密度の最小値と最大値をタイルごとに求めます。</summary>
internal sealed class VoronizeRangeCustomEffect(IGraphicsDevicesAndContext devices)
    : D2D1CustomShaderEffectBase(Create<VoronizeRangeCustomEffect.Impl>(devices))
{
    public Vector4 ImageRect { set => SetValue((int)Impl.Properties.ImageRect, value); }
    public Vector4 Grid { set => SetValue((int)Impl.Properties.Grid, value); }
    public int BlockOriginX { set => SetValue((int)Impl.Properties.BlockOriginX, value); }
    public int BlockOriginY { set => SetValue((int)Impl.Properties.BlockOriginY, value); }
    public int BlockLastX { set => SetValue((int)Impl.Properties.BlockLastX, value); }
    public int BlockLastY { set => SetValue((int)Impl.Properties.BlockLastY, value); }
    public int TileSize { set => SetValue((int)Impl.Properties.TileSize, value); }
    public int DensityMode { set => SetValue((int)Impl.Properties.DensityMode, value); }
    public float DetailGain { set => SetValue((int)Impl.Properties.DetailGain, value); }
    public int TileCountX { set => SetValue((int)Impl.Properties.TileCountX, value); }
    public int TileCountY { set => SetValue((int)Impl.Properties.TileCountY, value); }

    [CustomEffect(1)]
    internal sealed class Impl : D2D1CustomShaderEffectImplBase<Impl>
    {
        Constants constants;
        int tileCountX = 1;
        int tileCountY = 1;

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.ImageRect)]
        public Vector4 ImageRect { get => constants.ImageRect; set { constants.ImageRect = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Vector4, (int)Properties.Grid)]
        public Vector4 Grid { get => constants.Grid; set { constants.Grid = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.BlockOriginX)]
        public int BlockOriginX { get => constants.BlockOriginX; set { constants.BlockOriginX = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.BlockOriginY)]
        public int BlockOriginY { get => constants.BlockOriginY; set { constants.BlockOriginY = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.BlockLastX)]
        public int BlockLastX { get => constants.BlockLastX; set { constants.BlockLastX = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.BlockLastY)]
        public int BlockLastY { get => constants.BlockLastY; set { constants.BlockLastY = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.TileSize)]
        public int TileSize { get => constants.TileSize; set { constants.TileSize = Math.Max(value, 1); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.DensityMode)]
        public int DensityMode { get => constants.DensityMode; set { constants.DensityMode = Math.Clamp(value, 0, 4); UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Float, (int)Properties.DetailGain)]
        public float DetailGain { get => constants.DetailGain; set { constants.DetailGain = value; UpdateConstants(); } }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.TileCountX)]
        public int TileCountX { get => tileCountX; set => tileCountX = Math.Max(value, 1); }

        [CustomEffectProperty(PropertyType.Int32, (int)Properties.TileCountY)]
        public int TileCountY { get => tileCountY; set => tileCountY = Math.Max(value, 1); }

        public Impl() : base(ShaderResourceLoader.Get("VoronizeRange")) { }

        protected override void UpdateConstants() => drawInformation?.SetPixelShaderConstantBuffer(constants);

        public override void MapInputRectsToOutputRect(RawRect[] inputRects, RawRect[] inputOpaqueSubRects, out RawRect outputRect, out RawRect outputOpaqueSubRect)
        {
            inputRect = inputRects[0];
            // 出力の1画素が1タイル。入力の位置とは無関係な小さな画像です。
            outputRect = new RawRect(0, 0, tileCountX, tileCountY);
            outputOpaqueSubRect = default;
        }

        public override void MapOutputRectToInputRects(RawRect outputRect, RawRect[] inputRects)
        {
            // 解析ブロックは画像全体に散らばるので、常に入力全体を読みます。
            inputRects[0] = inputRect;
        }

        // HLSLの定数バッファと同じ並び (16バイト x 4)。
        [StructLayout(LayoutKind.Sequential, Size = 64)]
        struct Constants
        {
            public Vector4 ImageRect;
            public Vector4 Grid;
            public int BlockOriginX;
            public int BlockOriginY;
            public int BlockLastX;
            public int BlockLastY;
            public int TileSize;
            public int DensityMode;
            public float DetailGain;
        }

        internal enum Properties
        {
            ImageRect = 0,
            Grid = 1,
            BlockOriginX = 2,
            BlockOriginY = 3,
            BlockLastX = 4,
            BlockLastY = 5,
            TileSize = 6,
            DensityMode = 7,
            DetailGain = 8,
            TileCountX = 9,
            TileCountY = 10,
        }
    }
}
