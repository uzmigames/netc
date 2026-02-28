using System.Runtime.InteropServices;

namespace Netc;

/// <summary>
/// P/Invoke declarations for the netc C library.
/// </summary>
internal static class NetcNative
{
    private const string Lib = "netc";

    // ── Constants ──────────────────────────────────────────────────────
    internal const uint MaxPacketSize = 65535;
    internal const uint MaxOverhead = 8;

    // ── Config flags ───────────────────────────────────────────────────
    internal const uint CfgFlagStateful     = 0x01;
    internal const uint CfgFlagStateless    = 0x02;
    internal const uint CfgFlagDelta        = 0x04;
    internal const uint CfgFlagBigram       = 0x08;
    internal const uint CfgFlagStats        = 0x10;
    internal const uint CfgFlagCompactHdr   = 0x20;
    internal const uint CfgFlagFastCompress = 0x100;
    internal const uint CfgFlagAdaptive     = 0x200;

    // ── Structs ────────────────────────────────────────────────────────

    // C layout (64-bit): flags(4) pad(4) ring_buffer_size(8) compression_level(1) simd_level(1) pad(6) arena_size(8)
    [StructLayout(LayoutKind.Explicit, Size = 32)]
    internal struct NetcCfg
    {
        [FieldOffset(0)]  public uint flags;
        [FieldOffset(8)]  public nuint ring_buffer_size;
        [FieldOffset(16)] public byte compression_level;
        [FieldOffset(17)] public byte simd_level;
        [FieldOffset(24)] public nuint arena_size;
    }

    // NetcStats is defined as a public type (Netc.NetcStats) for external use

    // ── Context lifecycle ──────────────────────────────────────────────

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint netc_ctx_create(nint dict, ref NetcCfg cfg);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint netc_ctx_create(nint dict, nint cfg);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void netc_ctx_destroy(nint ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void netc_ctx_reset(nint ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_ctx_stats(nint ctx, out NetcStats stats);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern byte netc_ctx_simd_level(nint ctx);

    // ── Dictionary management ──────────────────────────────────────────

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int netc_dict_train(
        byte** packets, nuint* sizes, nuint count,
        byte model_id, out nint out_dict);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_dict_load(nint data, nuint size, out nint dict);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_dict_save(nint dict, out nint blob, out nuint size);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void netc_dict_free_blob(nint blob);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void netc_dict_free(nint dict);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern byte netc_dict_model_id(nint dict);

    // ── Compression ────────────────────────────────────────────────────

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_compress(
        nint ctx, nint src, nuint src_size,
        nint dst, nuint dst_cap, out nuint dst_size);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_decompress(
        nint ctx, nint src, nuint src_size,
        nint dst, nuint dst_cap, out nuint dst_size);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_compress_stateless(
        nint dict, nint src, nuint src_size,
        nint dst, nuint dst_cap, out nuint dst_size);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int netc_decompress_stateless(
        nint dict, nint src, nuint src_size,
        nint dst, nuint dst_cap, out nuint dst_size);

    // ── Utility ────────────────────────────────────────────────────────

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint netc_strerror(int result);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint netc_version();
}
