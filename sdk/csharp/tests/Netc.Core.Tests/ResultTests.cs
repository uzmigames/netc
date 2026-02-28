using System.Runtime.InteropServices;

namespace Netc.Tests;

public class ResultTests
{
    [Fact]
    public void OkIsZero()
    {
        Assert.Equal(0, (int)NetcResult.Ok);
    }

    [Theory]
    [InlineData(NetcResult.ErrNoMem, -1)]
    [InlineData(NetcResult.ErrTooBig, -2)]
    [InlineData(NetcResult.ErrCorrupt, -3)]
    [InlineData(NetcResult.ErrDictInvalid, -4)]
    [InlineData(NetcResult.ErrBufSmall, -5)]
    [InlineData(NetcResult.ErrCtxNull, -6)]
    [InlineData(NetcResult.ErrUnsupported, -7)]
    [InlineData(NetcResult.ErrVersion, -8)]
    [InlineData(NetcResult.ErrInvalidArg, -9)]
    public void ErrorCodesMatchC(NetcResult result, int expected)
    {
        Assert.Equal(expected, (int)result);
    }

    [Fact]
    public void VersionReturnsNonEmpty()
    {
        nint ptr = NetcNative.netc_version();
        string? version = Marshal.PtrToStringAnsi(ptr);
        Assert.NotNull(version);
        Assert.NotEmpty(version);
        Assert.Contains(".", version); // "0.2.0" format
    }
}
