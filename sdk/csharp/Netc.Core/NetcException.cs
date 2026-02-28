using System.Runtime.InteropServices;

namespace Netc;

/// <summary>
/// Exception thrown when a netc operation fails.
/// </summary>
public class NetcException : Exception
{
    public NetcResult Result { get; }

    public NetcException(NetcResult result)
        : base(GetMessage(result))
    {
        Result = result;
    }

    public NetcException(NetcResult result, string message)
        : base(message)
    {
        Result = result;
    }

    private static string GetMessage(NetcResult result)
    {
        nint ptr = NetcNative.netc_strerror((int)result);
        return ptr != 0 ? Marshal.PtrToStringAnsi(ptr) ?? result.ToString() : result.ToString();
    }

    /// <summary>
    /// Throw if the result is not Ok.
    /// </summary>
    internal static void ThrowIfError(int result)
    {
        if (result != 0)
            throw new NetcException((NetcResult)result);
    }
}
