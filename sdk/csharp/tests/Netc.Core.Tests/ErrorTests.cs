namespace Netc.Tests;

public class ErrorTests
{
    [Fact]
    public void DecompressCorruptDataThrows()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict);

        byte[] corrupt = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD };
        byte[] dst = new byte[65536];

        Assert.Throws<NetcException>(() => ctx.Decompress(corrupt, dst));
    }

    [Fact]
    public void DecompressTruncatedDataThrows()
    {
        using var dict = Helpers.BuildTestDict();
        using var compressor = NetcContext.Create(dict);

        byte[] src = Helpers.SampleGameState;
        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int cLen = compressor.Compress(src, compressed);

        // Truncate to half
        byte[] truncated = compressed[..(cLen / 2)];
        using var decompressor = NetcContext.Create(dict);
        byte[] dst = new byte[65536];

        Assert.Throws<NetcException>(() => decompressor.Decompress(truncated, dst));
    }

    [Fact]
    public void CompressTooBigThrows()
    {
        using var dict = Helpers.BuildTestDict();
        using var ctx = NetcContext.Create(dict);

        // 65536 exceeds NETC_MAX_PACKET_SIZE (65535)
        byte[] src = new byte[65536];
        byte[] dst = new byte[NetcContext.MaxCompressedSize(65536)];

        Assert.Throws<NetcException>(() => ctx.Compress(src, dst));
    }

    [Fact]
    public void StatelessDecompressCorruptThrows()
    {
        using var dict = Helpers.BuildTestDict();

        byte[] corrupt = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xAB };
        byte[] dst = new byte[65536];

        Assert.Throws<NetcException>(() =>
            NetcContext.DecompressStateless(dict, corrupt, dst));
    }

    [Fact]
    public void StatelessWithDisposedDictThrows()
    {
        var dict = Helpers.BuildTestDict();
        dict.Dispose();

        byte[] src = { 1, 2, 3 };
        byte[] dst = new byte[NetcContext.MaxCompressedSize(src.Length)];

        Assert.Throws<ObjectDisposedException>(() =>
            NetcContext.CompressStateless(dict, src, dst));
    }

    [Fact]
    public void CreateContextWithDisposedDictThrows()
    {
        var dict = Helpers.BuildTestDict();
        dict.Dispose();

        Assert.Throws<ObjectDisposedException>(() =>
            NetcContext.Create(dict));
    }
}
