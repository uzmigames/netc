namespace Netc.Tests;

public class DictTests
{
    [Fact]
    public void LoadSaveRoundtrip()
    {
        using var dict = Helpers.BuildTestDict(42);
        Assert.False(dict.IsDisposed);
        Assert.Equal(42, dict.ModelId);

        byte[] blob = dict.Save();
        Assert.True(blob.Length > 100);

        using var loaded = NetcDict.Load(blob);
        Assert.False(loaded.IsDisposed);
        Assert.Equal(42, loaded.ModelId);

        byte[] blob2 = loaded.Save();
        Assert.Equal(blob.Length, blob2.Length);
        Assert.Equal(blob, blob2);
    }

    [Fact]
    public void LoadInvalidDataThrows()
    {
        byte[] garbage = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 };
        var ex = Assert.Throws<NetcException>(() => NetcDict.Load(garbage));
        Assert.NotEqual(NetcResult.Ok, ex.Result);
    }

    [Fact]
    public void LoadEmptySpanThrows()
    {
        Assert.Throws<NetcException>(() => NetcDict.Load(ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void ModelIdReturnCorrectValue()
    {
        using var dict = Helpers.BuildTestDict(7);
        Assert.Equal(7, dict.ModelId);
    }

    [Fact]
    public void DoubleDisposeIsSafe()
    {
        var dict = Helpers.BuildTestDict();
        dict.Dispose();
        Assert.True(dict.IsDisposed);
        dict.Dispose(); // should not throw
        Assert.True(dict.IsDisposed);
    }

    [Fact]
    public void SaveAfterDisposeThrows()
    {
        var dict = Helpers.BuildTestDict();
        dict.Dispose();
        Assert.Throws<ObjectDisposedException>(() => dict.Save());
    }

    [Fact]
    public void ModelIdAfterDisposeThrows()
    {
        var dict = Helpers.BuildTestDict();
        dict.Dispose();
        Assert.Throws<ObjectDisposedException>(() => _ = dict.ModelId);
    }

    [Fact]
    public void LoadTruncatedDataThrows()
    {
        using var dict = Helpers.BuildTestDict();
        byte[] full = dict.Save();
        byte[] truncated = full[..(full.Length / 2)];
        var ex = Assert.Throws<NetcException>(() => NetcDict.Load(truncated));
        Assert.NotEqual(NetcResult.Ok, ex.Result);
    }
}
