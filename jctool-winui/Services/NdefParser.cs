using System.Buffers.Binary;
using System.Text;

namespace JcTool.WinUI.Services;

public static class NdefParser
{
    private static readonly string[] UriPrefixes =
    {
        string.Empty, "http://www.", "https://www.", "http://", "https://",
        "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.",
        "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://",
        "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:",
        "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
        "tcpobex://", "irdaobex://", "file://", "urn:epc:id:",
        "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
        "urn:nfc:"
    };

    public static bool TryParse(byte[] tagMemory, out string kind, out string content)
    {
        kind = string.Empty;
        content = string.Empty;
        if (!TryFindNdef(tagMemory, out var message))
        {
            return false;
        }

        var position = 0;
        var header = message[position++];
        var shortRecord = (header & 0x10) != 0;
        var hasId = (header & 0x08) != 0;
        if (position >= message.Length)
        {
            return false;
        }
        var typeLength = message[position++];
        long payloadLength;
        if (shortRecord)
        {
            if (position >= message.Length)
            {
                return false;
            }
            payloadLength = message[position++];
        }
        else
        {
            if (position + 4 > message.Length)
            {
                return false;
            }
            payloadLength = BinaryPrimitives.ReadUInt32BigEndian(message.AsSpan(position, 4));
            position += 4;
        }

        var idLength = 0;
        if (hasId)
        {
            if (position >= message.Length)
            {
                return false;
            }
            idLength = message[position++];
        }
        if (payloadLength > int.MaxValue
            || position + typeLength + idLength + payloadLength > message.Length)
        {
            return false;
        }

        var type = Encoding.ASCII.GetString(message, position, typeLength);
        position += typeLength + idLength;
        var payload = message.AsSpan(position, (int)payloadLength);
        if (type == "T" && payload.Length >= 1)
        {
            var languageLength = payload[0] & 0x3f;
            if (1 + languageLength > payload.Length)
            {
                return false;
            }
            var encoding = (payload[0] & 0x80) == 0 ? Encoding.UTF8 : Encoding.BigEndianUnicode;
            kind = "Text";
            content = encoding.GetString(payload[(1 + languageLength)..]);
            return true;
        }
        if (type == "U" && payload.Length >= 1)
        {
            var prefix = payload[0] < UriPrefixes.Length ? UriPrefixes[payload[0]] : string.Empty;
            kind = "URI";
            content = prefix + Encoding.UTF8.GetString(payload[1..]);
            return true;
        }
        return false;
    }

    private static bool TryFindNdef(byte[] memory, out byte[] message)
    {
        message = Array.Empty<byte>();
        var position = memory.Length > 16 ? 16 : 0;
        while (position < memory.Length)
        {
            var type = memory[position++];
            if (type == 0x00)
            {
                continue;
            }
            if (type == 0xfe || position >= memory.Length)
            {
                return false;
            }

            int length;
            if (memory[position] == 0xff)
            {
                if (position + 3 > memory.Length)
                {
                    return false;
                }
                length = BinaryPrimitives.ReadUInt16BigEndian(memory.AsSpan(position + 1, 2));
                position += 3;
            }
            else
            {
                length = memory[position++];
            }
            if (position + length > memory.Length)
            {
                return false;
            }
            if (type == 0x03)
            {
                message = memory.AsSpan(position, length).ToArray();
                return message.Length > 0;
            }
            position += length;
        }
        return false;
    }
}
