namespace Netc.Tests;

public class StatelessTests
{
    [Fact]
    public void CompressDecompressStatelessRoundtrip()
    {
        using var dict = Helpers.BuildTestDict();

        byte[] src = Helpers.SampleGameState;
        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int cLen = NetcContext.CompressStateless(dict, src, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = NetcContext.DecompressStateless(dict, compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(src.Length, dLen);
        Assert.Equal(src, decompressed[..dLen]);
    }

    [Fact]
    public void StatelessHighEntropyRoundtrip()
    {
        using var dict = Helpers.BuildTestDict();

        var rng = new Random(55);
        byte[] src = new byte[200];
        rng.NextBytes(src);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int cLen = NetcContext.CompressStateless(dict, src, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = NetcContext.DecompressStateless(dict, compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(src.Length, dLen);
        Assert.Equal(src, decompressed[..dLen]);
    }

    [Fact]
    public void StatelessMultipleIndependentPackets()
    {
        using var dict = Helpers.BuildTestDict();

        var rng = new Random(77);
        for (int i = 0; i < 50; i++)
        {
            byte[] src = new byte[16 + rng.Next(100)];
            rng.NextBytes(src);
            src[0] = (byte)i;

            byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
            int cLen = NetcContext.CompressStateless(dict, src, compressed);

            byte[] decompressed = new byte[65536];
            int dLen = NetcContext.DecompressStateless(dict, compressed.AsSpan(0, cLen), decompressed);

            Assert.Equal(src.Length, dLen);
            Assert.Equal(src, decompressed[..dLen]);
        }
    }

    [Fact]
    public void Stateless1ByteRoundtrip()
    {
        using var dict = Helpers.BuildTestDict();

        byte[] src = { 0xAB };
        byte[] compressed = new byte[NetcContext.MaxCompressedSize(1)];
        int cLen = NetcContext.CompressStateless(dict, src, compressed);

        byte[] decompressed = new byte[65536];
        int dLen = NetcContext.DecompressStateless(dict, compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(1, dLen);
        Assert.Equal(0xAB, decompressed[0]);
    }
}
