namespace Netc.Tests;

/// <summary>
/// Shared test helpers for building dictionaries and sample data.
/// </summary>
internal static class Helpers
{
    /// <summary>
    /// Sample 64-byte game state packet (positions, health, flags).
    /// </summary>
    internal static readonly byte[] SampleGameState = new byte[]
    {
        0x01, 0x00, 0x40, 0x42, 0x0F, 0x00, 0xC8, 0x42,
        0x00, 0x00, 0x48, 0x43, 0x00, 0x00, 0x80, 0x3F,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
        0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00,
        0x02, 0x00, 0x80, 0x42, 0x0F, 0x00, 0x00, 0x43,
        0x00, 0x00, 0x96, 0x43, 0x00, 0x00, 0x80, 0xBF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
        0x50, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xFF,
    };

    /// <summary>
    /// Build a trained dictionary from synthetic training data.
    /// </summary>
    internal static NetcDict BuildTestDict(byte modelId = 1)
    {
        using var trainer = new NetcTrainer();

        var rng = new Random(42);
        for (int i = 0; i < 400; i++)
        {
            var pkt = new byte[32 + rng.Next(96)];
            // Create structured data (positions, counters)
            pkt[0] = (byte)(i & 0xFF);
            pkt[1] = (byte)((i >> 8) & 0xFF);
            for (int j = 2; j < pkt.Length; j++)
            {
                // Mix structured and semi-random data
                pkt[j] = (byte)((j < 16) ? (j * 3 + i) : rng.Next(256));
            }
            trainer.AddPacket(pkt);
        }

        return trainer.Train(modelId);
    }
}
