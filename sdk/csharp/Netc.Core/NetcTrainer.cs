using System.Runtime.InteropServices;

namespace Netc;

/// <summary>
/// Accumulates packets and trains a dictionary. NOT thread-safe.
/// </summary>
public sealed class NetcTrainer : IDisposable
{
    private List<byte[]>? _corpus = new();

    /// <summary>
    /// Add a single packet to the training corpus.
    /// </summary>
    public void AddPacket(ReadOnlySpan<byte> packet)
    {
        ThrowIfDisposed();
        if (!packet.IsEmpty)
            _corpus!.Add(packet.ToArray());
    }

    /// <summary>
    /// Add multiple packets from an enumerable.
    /// </summary>
    public void AddPackets(IEnumerable<byte[]> packets)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(packets);
        foreach (var pkt in packets)
        {
            if (pkt is { Length: > 0 })
                _corpus!.Add(pkt);
        }
    }

    /// <summary>
    /// Number of packets in the training corpus.
    /// </summary>
    public int PacketCount
    {
        get
        {
            ThrowIfDisposed();
            return _corpus!.Count;
        }
    }

    /// <summary>
    /// Train a dictionary from the accumulated corpus.
    /// </summary>
    public unsafe NetcDict Train(byte modelId = 1)
    {
        ThrowIfDisposed();
        if (_corpus!.Count == 0)
            throw new NetcException(NetcResult.ErrInvalidArg, "Corpus is empty");

        int count = _corpus.Count;
        var handles = new GCHandle[count];
        var ptrs = new byte*[count];
        var sizes = new nuint[count];

        try
        {
            for (int i = 0; i < count; i++)
            {
                handles[i] = GCHandle.Alloc(_corpus[i], GCHandleType.Pinned);
                ptrs[i] = (byte*)handles[i].AddrOfPinnedObject();
                sizes[i] = (nuint)_corpus[i].Length;
            }

            fixed (byte** pPtrs = ptrs)
            fixed (nuint* pSizes = sizes)
            {
                int r = NetcNative.netc_dict_train(pPtrs, pSizes, (nuint)count, modelId, out nint dict);
                NetcException.ThrowIfError(r);

                // Use reflection-free approach: Load the dict from its serialized form
                // Actually, we have the handle directly — create dict via internal ctor trick.
                // We'll serialize and reload to use the public API cleanly.
                // Better: just expose an internal factory on NetcDict.

                return NetcDict.FromHandle(dict);
            }
        }
        finally
        {
            for (int i = 0; i < count; i++)
            {
                if (handles[i].IsAllocated)
                    handles[i].Free();
            }
        }
    }

    /// <summary>
    /// Clear the training corpus.
    /// </summary>
    public void Reset()
    {
        ThrowIfDisposed();
        _corpus!.Clear();
    }

    public bool IsDisposed => _corpus == null;

    public void Dispose()
    {
        _corpus = null;
    }

    private void ThrowIfDisposed()
    {
        if (_corpus == null)
            throw new ObjectDisposedException(nameof(NetcTrainer));
    }
}
