namespace Netc.Tests;

public class DisposalTests
{
    [Fact]
    public void DictDoubleDisposeIsSafe()
    {
        var dict = Helpers.BuildTestDict();
        dict.Dispose();
        dict.Dispose(); // should not throw
        Assert.True(dict.IsDisposed);
    }

    [Fact]
    public void ContextDoubleDisposeIsSafe()
    {
        using var dict = Helpers.BuildTestDict();
        var ctx = NetcContext.Create(dict);
        ctx.Dispose();
        ctx.Dispose(); // should not throw
        Assert.True(ctx.IsDisposed);
    }

    [Fact]
    public void TrainerDoubleDisposeIsSafe()
    {
        var trainer = new NetcTrainer();
        trainer.AddPacket(new byte[] { 1, 2, 3 });
        trainer.Dispose();
        trainer.Dispose(); // should not throw
        Assert.True(trainer.IsDisposed);
    }

    [Fact]
    public void CompressAfterDisposeThrows()
    {
        using var dict = Helpers.BuildTestDict();
        var ctx = NetcContext.Create(dict);
        ctx.Dispose();

        byte[] src = { 1, 2, 3 };
        byte[] dst = new byte[NetcContext.MaxCompressedSize(src.Length)];
        Assert.Throws<ObjectDisposedException>(() => ctx.Compress(src, dst));
    }

    [Fact]
    public void DecompressAfterDisposeThrows()
    {
        using var dict = Helpers.BuildTestDict();
        var ctx = NetcContext.Create(dict);
        ctx.Dispose();

        byte[] data = { 0x00, 0x00, 0x00, 0x00 };
        byte[] dst = new byte[65536];
        Assert.Throws<ObjectDisposedException>(() => ctx.Decompress(data, dst));
    }

    [Fact]
    public void ResetAfterDisposeThrows()
    {
        using var dict = Helpers.BuildTestDict();
        var ctx = NetcContext.Create(dict);
        ctx.Dispose();

        Assert.Throws<ObjectDisposedException>(() => ctx.Reset());
    }

    [Fact]
    public void TrainerAddAfterDisposeThrows()
    {
        var trainer = new NetcTrainer();
        trainer.Dispose();

        Assert.Throws<ObjectDisposedException>(() =>
            trainer.AddPacket(new byte[] { 1, 2, 3 }));
    }

    [Fact]
    public void TrainerTrainAfterDisposeThrows()
    {
        var trainer = new NetcTrainer();
        trainer.Dispose();

        Assert.Throws<ObjectDisposedException>(() => trainer.Train());
    }
}
