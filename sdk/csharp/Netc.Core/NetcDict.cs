using System.Runtime.InteropServices;

namespace Netc;

/// <summary>
/// RAII wrapper for a netc trained dictionary. Thread-safe for concurrent reads.
/// </summary>
public sealed class NetcDict : IDisposable
{
    internal nint Handle;

    private NetcDict(nint handle) => Handle = handle;

    /// <summary>
    /// Load a dictionary from a binary blob.
    /// </summary>
    public static NetcDict Load(ReadOnlySpan<byte> data)
    {
        if (data.IsEmpty)
            throw new NetcException(NetcResult.ErrInvalidArg, "Data is empty");

        unsafe
        {
            fixed (byte* p = data)
            {
                int r = NetcNative.netc_dict_load((nint)p, (nuint)data.Length, out nint dict);
                NetcException.ThrowIfError(r);
                return new NetcDict(dict);
            }
        }
    }

    /// <summary>
    /// Serialize the dictionary to a byte array.
    /// </summary>
    public byte[] Save()
    {
        ThrowIfDisposed();

        int r = NetcNative.netc_dict_save(Handle, out nint blob, out nuint size);
        NetcException.ThrowIfError(r);

        try
        {
            byte[] result = new byte[(int)size];
            Marshal.Copy(blob, result, 0, (int)size);
            return result;
        }
        finally
        {
            NetcNative.netc_dict_free_blob(blob);
        }
    }

    /// <summary>
    /// Get the model ID embedded in this dictionary (1-254).
    /// </summary>
    public byte ModelId
    {
        get
        {
            ThrowIfDisposed();
            return NetcNative.netc_dict_model_id(Handle);
        }
    }

    public bool IsDisposed => Handle == 0;

    public void Dispose()
    {
        if (Handle != 0)
        {
            NetcNative.netc_dict_free(Handle);
            Handle = 0;
        }
    }

    /// <summary>
    /// Create a NetcDict from a raw handle (used internally by NetcTrainer).
    /// </summary>
    internal static NetcDict FromHandle(nint handle) => new(handle);

    internal void ThrowIfDisposed()
    {
        if (Handle == 0)
            throw new ObjectDisposedException(nameof(NetcDict));
    }
}
