namespace Netc.Tests;

public class TrainerTests
{
    [Fact]
    public void AddPacketIncrementsCount()
    {
        using var trainer = new NetcTrainer();
        Assert.Equal(0, trainer.PacketCount);

        trainer.AddPacket(new byte[] { 1, 2, 3 });
        Assert.Equal(1, trainer.PacketCount);

        trainer.AddPacket(new byte[] { 4, 5, 6 });
        Assert.Equal(2, trainer.PacketCount);
    }

    [Fact]
    public void AddPacketsFromEnumerable()
    {
        using var trainer = new NetcTrainer();
        var packets = new List<byte[]>
        {
            new byte[] { 1, 2, 3 },
            new byte[] { 4, 5, 6 },
            new byte[] { 7, 8, 9 },
        };
        trainer.AddPackets(packets);
        Assert.Equal(3, trainer.PacketCount);
    }

    [Fact]
    public void TrainProducesValidDict()
    {
        using var dict = Helpers.BuildTestDict(1);
        Assert.False(dict.IsDisposed);
        Assert.Equal(1, dict.ModelId);

        // Verify dict can be used for compression
        using var ctx = NetcContext.Create(dict);
        byte[] src = { 1, 2, 3, 4, 5, 6, 7, 8 };
        byte[] compressed = new byte[NetcContext.MaxCompressedSize(src.Length)];
        int len = ctx.Compress(src, compressed);
        Assert.True(len > 0);
    }

    [Fact]
    public void TrainPreservesModelId()
    {
        using var dict = Helpers.BuildTestDict(99);
        Assert.Equal(99, dict.ModelId);
    }

    [Fact]
    public void ResetClearsCorpus()
    {
        using var trainer = new NetcTrainer();
        trainer.AddPacket(new byte[] { 1, 2, 3 });
        Assert.Equal(1, trainer.PacketCount);

        trainer.Reset();
        Assert.Equal(0, trainer.PacketCount);
    }

    [Fact]
    public void TrainEmptyCorpusThrows()
    {
        using var trainer = new NetcTrainer();
        var ex = Assert.Throws<NetcException>(() => trainer.Train());
        Assert.Equal(NetcResult.ErrInvalidArg, ex.Result);
    }

    [Fact]
    public void AddEmptyPacketIsIgnored()
    {
        using var trainer = new NetcTrainer();
        trainer.AddPacket(ReadOnlySpan<byte>.Empty);
        Assert.Equal(0, trainer.PacketCount);
    }
}
