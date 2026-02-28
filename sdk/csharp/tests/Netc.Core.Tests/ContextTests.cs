namespace Netc.Tests;

public class ContextTests
{
    [Fact]
    public void CreateStateful()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict, NetcMode.Stateful);
        Assert.False(ctx.IsDisposed);
    }

    [Fact]
    public void CreateStateless()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict, NetcMode.Stateless);
        Assert.False(ctx.IsDisposed);
    }

    [Fact]
    public void CompressDecompressRoundtripRepetitive()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);
        using var decompressor = NetcContext.Create(dict);

        byte[] src = new byte[128];
        for (int i = 0; i < src.Length; i++) src[i] = (byte)(i % 16);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int cLen = compressor.Compress(src, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = decompressor.Decompress(compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(src.Length, dLen);
        Assert.Equal(src, decompressed[..dLen]);
    }

    [Fact]
    public void CompressDecompressRoundtripStructured()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);
        using var decompressor = NetcContext.Create(dict);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(Helpers.SampleGameState.Length)];
        int cLen = compressor.Compress(Helpers.SampleGameState, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = decompressor.Decompress(compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(Helpers.SampleGameState.Length, dLen);
        Assert.Equal(Helpers.SampleGameState, decompressed[..dLen]);
    }

    [Fact]
    public void CompressDecompressRoundtripHighEntropy()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);
        using var decompressor = NetcContext.Create(dict);

        var rng = new Random(99);
        byte[] src = new byte[256];
        rng.NextBytes(src);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int cLen = compressor.Compress(src, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = decompressor.Decompress(compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(src.Length, dLen);
        Assert.Equal(src, decompressed[..dLen]);
    }

    [Fact]
    public void MultiPacketTcpRoundtrip()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);
        using var decompressor = NetcContext.Create(dict);

        var rng = new Random(123);
        for (int i = 0; i < 100; i++)
        {
            byte[] src = new byte[32 + rng.Next(96)];
            src[0] = (byte)(i & 0xFF);
            for (int j = 1; j < src.Length; j++)
                src[j] = (byte)((j < 16) ? (j * 2 + i) : rng.Next(256));

            byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
            int cLen = compressor.Compress(src, compressed);

            byte[] decompressed = new byte[65536];
            int dLen = decompressor.Decompress(compressed.AsSpan(0, cLen), decompressed);

            Assert.Equal(src.Length, dLen);
            Assert.Equal(src, decompressed[..dLen]);
        }
    }

    [Fact]
    public void Compress1BytePacket()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);
        using var decompressor = NetcContext.Create(dict);

        byte[] src = { 0x42 };
        byte[] compressed = new byte[NetcContext.MaxCompressedSize(1)];
        int cLen = compressor.Compress(src, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = decompressor.Decompress(compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(1, dLen);
        Assert.Equal(0x42, decompressed[0]);
    }

    [Fact]
    public void CompressMaxPacket()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);
        using var decompressor = NetcContext.Create(dict);

        byte[] src = new byte[65535];
        new Random(77).NextBytes(src);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int cLen = compressor.Compress(src, compressed);
        Assert.True(cLen > 0);

        byte[] decompressed = new byte[65536];
        int dLen = decompressor.Decompress(compressed.AsSpan(0, cLen), decompressed);
        Assert.Equal(src.Length, dLen);
        Assert.Equal(src, decompressed[..dLen]);
    }

    [Fact]
    public void ResetKeepsContextValid()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(Helpers.SampleGameState.Length)];
        ctx.Compress(Helpers.SampleGameState, compressed);
        ctx.Reset();
        Assert.False(ctx.IsDisposed);

        // Can still compress after reset
        int len = ctx.Compress(Helpers.SampleGameState, compressed);
        Assert.True(len > 0);
    }

    [Fact]
    public void SimdLevelIsValid()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict);
        byte level = ctx.SimdLevel;
        Assert.True(level >= 1 && level <= 4, $"SIMD level {level} out of range [1,4]");
    }

    [Fact]
    public void GetStatsAfterCompress()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict);

        byte[] compressed = new byte[NetcContext.MaxCompressedSize(Helpers.SampleGameState.Length)];
        ctx.Compress(Helpers.SampleGameState, compressed);

        var stats = ctx.GetStats();
        Assert.Equal(1UL, stats.PacketsCompressed);
        Assert.True(stats.BytesIn > 0);
        Assert.True(stats.BytesOut > 0);
    }

    [Fact]
    public void MaxCompressedSizeCalculation()
    {
        Assert.Equal(8, NetcContext.MaxCompressedSize(0));
        Assert.Equal(9, NetcContext.MaxCompressedSize(1));
        Assert.Equal(65543, NetcContext.MaxCompressedSize(65535));
    }
}
