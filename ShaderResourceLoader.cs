using System.IO;

namespace SaVoronize;

internal static class ShaderResourceLoader
{
    public static byte[] Get(string name)
    {
        using var stream = typeof(ShaderResourceLoader).Assembly.GetManifestResourceStream($"SaVoronize.{name}.cso")
            ?? throw new InvalidOperationException($"シェーダー リソースがありません: {name}");
        using var output = new MemoryStream();
        stream.CopyTo(output);
        return output.ToArray();
    }
}
