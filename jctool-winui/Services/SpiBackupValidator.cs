using System.Globalization;
using System.Security.Cryptography;
using JcTool.WinUI.Models;

namespace JcTool.WinUI.Services;

public static class SpiBackupValidator
{
    public const int Size = 0x80000;
    private const int ProductMarkerOffset = 0x6012;
    private const int MacOffset = 0x15;

    public static SpiBackupValidation Validate(
        byte[] data,
        ControllerSlot controller,
        string? expectedSha256 = null)
    {
        ArgumentNullException.ThrowIfNull(data);
        ArgumentNullException.ThrowIfNull(controller);
        var hash = Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
        var hasSize = data.Length == Size;
        var hasChecksum = !string.IsNullOrWhiteSpace(expectedSha256)
            && string.Equals(hash, expectedSha256.Trim(), StringComparison.OrdinalIgnoreCase);
        var expectedProduct = ProductMarker(controller.ProductId);
        var backupProduct = hasSize ? data[ProductMarkerOffset] : 0;
        var sameProduct = expectedProduct != 0 && backupProduct == expectedProduct;
        var sameController = hasSize && sameProduct && MatchesMac(data, controller.MacAddress);
        var message = !hasSize
            ? "SpiBackupSizeInvalid"
            : !sameProduct
                ? "SpiBackupProductMismatch"
                : !sameController
                    ? "SpiBackupControllerMismatch"
                    : expectedSha256 is not null && !hasChecksum
                        ? "SpiBackupChecksumInvalid"
                        : "SpiBackupReady";
        return new SpiBackupValidation
        {
            IsValid = hasSize && sameProduct && sameController && (expectedSha256 is null || hasChecksum),
            IsSameController = sameController,
            IsSameProduct = sameProduct,
            HasExpectedSize = hasSize,
            HasChecksum = hasChecksum,
            Sha256 = hash,
            MessageResource = message
        };
    }

    public static string ComputeSha256(byte[] data)
    {
        ArgumentNullException.ThrowIfNull(data);
        return Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
    }

    public static int ProductMarker(uint productId) => productId switch
    {
        0x2006 => 1,
        0x2007 => 2,
        0x2009 => 3,
        _ => 0
    };

    private static bool MatchesMac(byte[] data, string macAddress)
    {
        var parts = macAddress.Split(':', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length != 6)
        {
            return false;
        }
        var mac = new byte[6];
        for (var index = 0; index < mac.Length; index++)
        {
            if (!byte.TryParse(parts[index], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value))
            {
                return false;
            }
            mac[index] = value;
        }
        return data.AsSpan(MacOffset, mac.Length).SequenceEqual(mac.Reverse().ToArray());
    }
}
