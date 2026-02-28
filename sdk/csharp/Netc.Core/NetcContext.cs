namespace Netc;

/// <summary>
/// RAII wrapper for a netc compression context. NOT thread-safe — one per connection per thread.
/// </summary>
public sealed class NetcContext : IDisposable
{
    private nint _handle;
    private readonly NetcDict _dict; // prevent GC of dict while context is alive

    private NetcContext(nint handle, NetcDict dict)
    {
        _handle = handle;
        _dict = dict;
    }

    /// <summary>
    /// Create a compression context bound to the given dictionary.
    /// </summary>
    public static NetcContext Create(NetcDict dict, NetcMode mode = NetcMode.Stateful,
                                     byte level = 5, uint extraFlags = 0)
    {
        ArgumentNullException.ThrowIfNull(dict);
        dict.ThrowIfDisposed();

        var cfg = new NetcNative.NetcCfg
        {
            flags = (mode == NetcMode.Stateful
                ? NetcNative.CfgFlagStateful
                : NetcNative.CfgFlagStateless)
                | NetcNative.CfgFlagStats
                | extraFlags,
            compression_level = level,
        };

        nint handle = NetcNative.netc_ctx_create(dict.Handle, ref cfg);
        if (handle == 0)
            throw new NetcException(NetcResult.ErrNoMem, "Failed to create context");

        return new NetcContext(handle, dict);
    }

    /// <summary>
    /// Compress a packet. Returns the number of bytes written to dst.
    /// dst must be at least MaxCompressedSize(src.Length) bytes.
    /// </summary>
    public unsafe int Compress(ReadOnlySpan<byte> src, Span<byte> dst)
    {
        ThrowIfDisposed();

        fixed (byte* pSrc = src)
        fixed (byte* pDst = dst)
        {
            int r = NetcNative.netc_compress(
                _handle, (nint)pSrc, (nuint)src.Length,
                (nint)pDst, (nuint)dst.Length, out nuint dstSize);
            NetcException.ThrowIfError(r);
            return (int)dstSize;
        }
    }

    /// <summary>
    /// Decompress a packet. Returns the number of bytes written to dst.
    /// dst must be at least NETC_MAX_PACKET_SIZE bytes for safety.
    /// </summary>
    public unsafe int Decompress(ReadOnlySpan<byte> src, Span<byte> dst)
    {
        ThrowIfDisposed();

        fixed (byte* pSrc = src)
        fixed (byte* pDst = dst)
        {
            int r = NetcNative.netc_decompress(
                _handle, (nint)pSrc, (nuint)src.Length,
                (nint)pDst, (nuint)dst.Length, out nuint dstSize);
            NetcException.ThrowIfError(r);
            return (int)dstSize;
        }
    }

    /// <summary>
    /// Compress a single packet without stateful context.
    /// </summary>
    public static unsafe int CompressStateless(NetcDict dict, ReadOnlySpan<byte> src, Span<byte> dst)
    {
        ArgumentNullException.ThrowIfNull(dict);
        dict.ThrowIfDisposed();

        fixed (byte* pSrc = src)
        fixed (byte* pDst = dst)
        {
            int r = NetcNative.netc_compress_stateless(
                dict.Handle, (nint)pSrc, (nuint)src.Length,
                (nint)pDst, (nuint)dst.Length, out nuint dstSize);
            NetcException.ThrowIfError(r);
            return (int)dstSize;
        }
    }

    /// <summary>
    /// Decompress a single packet without stateful context.
    /// </summary>
    public static unsafe int DecompressStateless(NetcDict dict, ReadOnlySpan<byte> src, Span<byte> dst)
    {
        ArgumentNullException.ThrowIfNull(dict);
        dict.ThrowIfDisposed();

        fixed (byte* pSrc = src)
        fixed (byte* pDst = dst)
        {
            int r = NetcNative.netc_decompress_stateless(
                dict.Handle, (nint)pSrc, (nuint)src.Length,
                (nint)pDst, (nuint)dst.Length, out nuint dstSize);
            NetcException.ThrowIfError(r);
            return (int)dstSize;
        }
    }

    /// <summary>
    /// Maximum compressed output size for a given input size.
    /// </summary>
    public static int MaxCompressedSize(int srcLength)
        => srcLength + (int)NetcNative.MaxOverhead;

    /// <summary>
    /// Reset per-connection state without releasing memory.
    /// </summary>
    public void Reset()
    {
        ThrowIfDisposed();
        NetcNative.netc_ctx_reset(_handle);
    }

    /// <summary>
    /// Get the active SIMD level (1=generic, 2=SSE4.2, 3=AVX2, 4=NEON).
    /// </summary>
    public byte SimdLevel
    {
        get
        {
            ThrowIfDisposed();
            return NetcNative.netc_ctx_simd_level(_handle);
        }
    }

    /// <summary>
    /// Get compression statistics (requires NETC_CFG_FLAG_STATS).
    /// </summary>
    public NetcStats GetStats()
    {
        ThrowIfDisposed();
        int r = NetcNative.netc_ctx_stats(_handle, out var stats);
        NetcException.ThrowIfError(r);
        return stats;
    }

    public bool IsDisposed => _handle == 0;

    public void Dispose()
    {
        if (_handle != 0)
        {
            NetcNative.netc_ctx_destroy(_handle);
            _handle = 0;
        }
    }

    private void ThrowIfDisposed()
    {
        if (_handle == 0)
            throw new ObjectDisposedException(nameof(NetcContext));
    }
}
