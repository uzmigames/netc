using System.Runtime.InteropServices;

namespace Netc;

/// <summary>
/// Compression statistics collected when NETC_CFG_FLAG_STATS is set.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct NetcStats
{
    public ulong PacketsCompressed;
    public ulong PacketsDecompressed;
    public ulong BytesIn;
    public ulong BytesOut;
    public ulong PassthroughCount;

    public double AverageRatio => BytesIn > 0 ? (double)BytesOut / BytesIn : 0.0;
}
