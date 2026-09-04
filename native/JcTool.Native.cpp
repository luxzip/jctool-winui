#include "JcTool.Native.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../jctool/hidapi.h"

bool enable_traffic_dump = false;

namespace
{
    constexpr size_t report_size = 49;
    std::mutex rumble_mutex;
    std::unordered_map<std::wstring, std::shared_ptr<std::atomic_bool>> rumble_cancellations;

    struct input_stream_session
    {
        hid_device* handle = nullptr;
        unsigned char packet_number = 0;
        std::mutex io_mutex;
        std::atomic_bool stopping{ false };
    };

    std::mutex input_streams_mutex;
    std::unordered_map<std::wstring, std::shared_ptr<input_stream_session>> input_streams;

    bool is_supported_controller(unsigned short product_id)
    {
        return product_id == 0x2006 || product_id == 0x2007 || product_id == 0x2009;
    }

    const wchar_t* fallback_product_name(unsigned short product_id)
    {
        switch (product_id)
        {
        case 0x2006: return L"Joy-Con (L)";
        case 0x2007: return L"Joy-Con (R)";
        case 0x2009: return L"Pro Controller";
        default: return L"Nintendo controller";
        }
    }

    bool send_subcommand(
        hid_device* handle,
        unsigned char& packet_number,
        unsigned char subcommand,
        const unsigned char* arguments,
        size_t argument_count,
        unsigned char expected_ack,
        unsigned char* reply)
    {
        if (!handle || argument_count > report_size - 11)
        {
            return false;
        }

        unsigned char command[report_size]{};
        command[0] = 0x01;
        command[1] = packet_number++ & 0x0f;
        command[10] = subcommand;
        if (arguments && argument_count > 0)
        {
            std::memcpy(command + 11, arguments, argument_count);
        }

        if (hid_write(handle, command, sizeof(command)) < static_cast<int>(sizeof(command)))
        {
            return false;
        }

        for (int attempt = 0; attempt < 9; ++attempt)
        {
            std::memset(reply, 0, report_size);
            const int read = hid_read_timeout(handle, reply, report_size, 64);
            if (read >= 15 && reply[0] == 0x21 && reply[13] == expected_ack && reply[14] == subcommand)
            {
                return true;
            }
            if (read <= 0)
            {
                break;
            }
        }
        return false;
    }

    bool read_spi(
        hid_device* handle,
        unsigned char& packet_number,
        std::uint32_t offset,
        unsigned char count,
        unsigned char* output)
    {
        if (!output || count == 0 || count > 29 || offset > 0x7ffff || count > 0x80000 - offset)
        {
            return false;
        }

        unsigned char arguments[5]{};
        std::memcpy(arguments, &offset, sizeof(offset));
        arguments[4] = count;
        unsigned char reply[report_size]{};
        if (!send_subcommand(handle, packet_number, 0x10, arguments, sizeof(arguments), 0x90, reply))
        {
            return false;
        }

        std::uint32_t reply_offset = 0;
        std::memcpy(&reply_offset, reply + 15, sizeof(reply_offset));
        if (reply_offset != offset || reply[19] != count)
        {
            return false;
        }
        std::memcpy(output, reply + 20, count);
        return true;
    }

    int battery_percent(std::uint16_t voltage)
    {
        if (voltage < 0x560) return 1;
        if (voltage < 0x5a0) return static_cast<int>(((voltage - 0x60) & 0xff) / 7.0f + 1);
        if (voltage < 0x5e0) return static_cast<int>(((voltage - 0xa0) & 0xff) / 2.625f + 11);
        if (voltage < 0x618) return static_cast<int>((voltage - 0x5e0) / 1.8965f + 36);
        if (voltage < 0x658) return static_cast<int>(((voltage - 0x18) & 0xff) / 1.8529f + 66);
        return 100;
    }

    void populate_details(hid_device_info* source, jc_device_snapshot& target)
    {
        hid_device* handle = hid_open_path(source->path);
        if (!handle)
        {
            return;
        }

        unsigned char packet_number = 0;
        unsigned char reply[report_size]{};
        if (send_subcommand(handle, packet_number, 0x02, nullptr, 0, 0x82, reply))
        {
            swprintf_s(target.firmware, L"%X.%02X", reply[15], reply[16]);
            swprintf_s(target.mac_address, L"%02X:%02X:%02X:%02X:%02X:%02X",
                reply[19], reply[20], reply[21], reply[22], reply[23], reply[24]);
            target.details_available = 1;
        }

        if (send_subcommand(handle, packet_number, 0x50, nullptr, 0, 0xd0, reply))
        {
            const std::uint16_t voltage = static_cast<std::uint16_t>(reply[15] | (reply[16] << 8));
            target.battery_percent = battery_percent(voltage);
            target.battery_charging = ((reply[2] >> 4) & 1) != 0;
            target.battery_voltage = voltage * 2.5f / 1000.0f;
        }
        else
        {
            target.battery_percent = -1;
        }

        bool imu_enabled_for_read = false;
        const unsigned char imu_status_arguments[] = { 0x10, 0x01 };
        if (send_subcommand(handle, packet_number, 0x43,
            imu_status_arguments, sizeof(imu_status_arguments), 0xc0, reply)
            && (reply[17] >> 4) == 0)
        {
            const unsigned char enable_imu[] = { 0x01 };
            if (send_subcommand(handle, packet_number, 0x40,
                enable_imu, sizeof(enable_imu), 0x80, reply))
            {
                imu_enabled_for_read = true;
                Sleep(64);
            }
        }

        const unsigned char temperature_arguments[] = { 0x20, 0x02 };
        if (send_subcommand(handle, packet_number, 0x43,
            temperature_arguments, sizeof(temperature_arguments), 0xc0, reply))
        {
            const auto raw = static_cast<std::int16_t>(reply[17] | (reply[18] << 8));
            target.temperature_celsius = 25.0f + raw * 0.0625f;
            target.temperature_available = 1;
        }

        if (imu_enabled_for_read)
        {
            const unsigned char disable_imu[] = { 0x00 };
            send_subcommand(handle, packet_number, 0x40,
                disable_imu, sizeof(disable_imu), 0x80, reply);
        }

        target.colors_available = read_spi(handle, packet_number, 0x6050, 12, target.colors) ? 1 : 0;
        hid_close(handle);
    }

    hid_device* open_device(const wchar_t* device_key)
    {
        if (!device_key || !device_key[0])
        {
            return nullptr;
        }

        char path[768]{};
        if (WideCharToMultiByte(CP_UTF8, 0, device_key, -1,
            path, static_cast<int>(_countof(path)), nullptr, nullptr) == 0)
        {
            return nullptr;
        }
        return hid_open_path(path);
    }

    bool write_spi(
        hid_device* handle,
        unsigned char& packet_number,
        std::uint32_t offset,
        const unsigned char* data,
        unsigned char count)
    {
        if (!handle || !data || count == 0 || count > 29
            || offset > 0x7ffff || count > 0x80000 - offset)
        {
            return false;
        }

        unsigned char arguments[34]{};
        std::memcpy(arguments, &offset, sizeof(offset));
        arguments[4] = count;
        std::memcpy(arguments + 5, data, count);
        unsigned char reply[report_size]{};
        if (!send_subcommand(handle, packet_number, 0x11,
            arguments, 5 + count, 0x80, reply) || reply[15] != 0)
        {
            return false;
        }

        Sleep(20);
        unsigned char verified[29]{};
        return read_spi(handle, packet_number, offset, count, verified)
            && std::memcmp(data, verified, count) == 0;
    }

    bool write_raw_report(hid_device* handle, unsigned char* report)
    {
        return hid_write(handle, report, report_size) >= static_cast<int>(report_size);
    }

    bool is_safe_write_range(std::uint32_t offset, int length)
    {
        if (length <= 0 || offset > 0x7ffff
            || static_cast<std::uint32_t>(length) > 0x80000 - offset)
        {
            return false;
        }

        const auto end = offset + static_cast<std::uint32_t>(length);
        return (offset >= 0x6000 && end <= 0x7000)
            || (offset >= 0x8000 && end <= 0x9000)
            || (offset >= 0xf000 && end <= 0xf010);
    }

    bool is_safe_spi_read_range(std::uint32_t offset, unsigned char length)
    {
        return length > 0 && length <= 29 && offset <= 0x7ffff
            && length <= 0x80000 - offset;
    }

    bool command_tail_is_zero(const unsigned char* command, int start)
    {
        for (int index = start; index < static_cast<int>(report_size); ++index)
        {
            if (command[index] != 0)
            {
                return false;
            }
        }
        return true;
    }

    bool is_safe_debug_command(const unsigned char* command)
    {
        if (!command || command[0] != 0x01)
        {
            return false;
        }
        switch (command[10])
        {
        case 0x02:
        case 0x30:
        case 0x38:
        case 0x50:
            return true;
        case 0x03:
            return command[11] == 0x30 || command[11] == 0x31 || command[11] == 0x3f;
        case 0x10:
        {
            const auto offset = static_cast<std::uint32_t>(command[11])
                | static_cast<std::uint32_t>(command[12]) << 8
                | static_cast<std::uint32_t>(command[13]) << 16
                | static_cast<std::uint32_t>(command[14]) << 24;
            return is_safe_spi_read_range(offset, command[15]);
        }
        case 0x40:
        case 0x48:
            return command[11] <= 1;
        case 0x43:
            return (command[11] == 0x10 && command[12] == 0x01)
                || (command[11] == 0x20 && command[12] == 0x02);
        default:
            return false;
        }
    }

    bool is_safe_internal_command(const unsigned char* command)
    {
        if (!command || command[0] != 0x01)
        {
            return false;
        }
        if (command[10] == 0x06)
        {
            return (command[11] == 0x00 || command[11] == 0x02)
                && command_tail_is_zero(command, 12);
        }
        if (command[10] == 0x07)
        {
            return command_tail_is_zero(command, 11);
        }
        if (command[10] == 0x08)
        {
            return command[11] <= 1 && command_tail_is_zero(command, 12);
        }
        return false;
    }

    void sleep_interruptibly(int milliseconds, const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        int remaining = milliseconds;
        while (remaining > 0 && !cancelled->load())
        {
            const int delay = (std::min)(remaining, 20);
            Sleep(delay);
            remaining -= delay;
        }
    }

    bool parse_input_report(
        const unsigned char* reply,
        int length,
        jc_input_snapshot* snapshot)
    {
        if (!reply || !snapshot || length < 25
            || (reply[0] != 0x30 && reply[0] != 0x31
                && reply[0] != 0x32 && reply[0] != 0x33))
        {
            return false;
        }

        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->buttons = static_cast<unsigned int>(reply[3])
            | static_cast<unsigned int>(reply[4]) << 8
            | static_cast<unsigned int>(reply[5]) << 16;
        snapshot->left_x = reply[6] | (reply[7] & 0x0f) << 8;
        snapshot->left_y = reply[7] >> 4 | reply[8] << 4;
        snapshot->right_x = reply[9] | (reply[10] & 0x0f) << 8;
        snapshot->right_y = reply[10] >> 4 | reply[11] << 4;
        snapshot->acceleration_x = static_cast<std::int16_t>(reply[13] | reply[14] << 8);
        snapshot->acceleration_y = static_cast<std::int16_t>(reply[15] | reply[16] << 8);
        snapshot->acceleration_z = static_cast<std::int16_t>(reply[17] | reply[18] << 8);
        snapshot->gyroscope_x = static_cast<std::int16_t>(reply[19] | reply[20] << 8);
        snapshot->gyroscope_y = static_cast<std::int16_t>(reply[21] | reply[22] << 8);
        snapshot->gyroscope_z = static_cast<std::int16_t>(reply[23] | reply[24] << 8);
        snapshot->connection_type = (reply[2] >> 1) & 0x03;
        snapshot->battery_level = reply[2] >> 5;
        snapshot->charging = (reply[2] >> 4) & 0x01;
        return true;
    }
}

int __cdecl jc_get_devices(jc_device_snapshot* devices, int capacity)
{
    if (!devices || capacity <= 0)
    {
        return 0;
    }

    std::memset(devices, 0, sizeof(jc_device_snapshot) * capacity);
    if (hid_init() != 0)
    {
        return -1;
    }

    hid_device_info* enumeration = hid_enumerate(0x057e, 0x0000);
    int count = 0;
    for (hid_device_info* current = enumeration;
         current && count < capacity;
         current = current->next)
    {
        if (!is_supported_controller(current->product_id))
        {
            continue;
        }

        jc_device_snapshot& target = devices[count++];
        target.product_id = current->product_id;
        wcsncpy_s(target.serial_number, current->serial_number ? current->serial_number : L"", _TRUNCATE);
        wcsncpy_s(target.product_name,
            current->product_string ? current->product_string : fallback_product_name(current->product_id),
            _TRUNCATE);
        if (current->path)
        {
            MultiByteToWideChar(CP_UTF8, 0, current->path, -1,
                target.device_key, static_cast<int>(_countof(target.device_key)));
        }
        populate_details(current, target);
    }

    hid_free_enumeration(enumeration);
    return count;
}

int __cdecl jc_read_spi(
    const wchar_t* device_key,
    unsigned int offset,
    unsigned char* output,
    int length)
{
    if (!output || length <= 0 || offset > 0x7ffff
        || static_cast<unsigned int>(length) > 0x80000 - offset)
    {
        return 1;
    }

    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        return 2;
    }

    unsigned char packet_number = 0;
    int position = 0;
    while (position < length)
    {
        const auto count = static_cast<unsigned char>((std::min)(29, length - position));
        if (!read_spi(handle, packet_number, offset + position, count, output + position))
        {
            hid_close(handle);
            return 3;
        }
        position += count;
    }

    hid_close(handle);
    return 0;
}

int __cdecl jc_write_colors(
    const wchar_t* device_key,
    const unsigned char* colors,
    int length)
{
    if (!colors || (length != 6 && length != 12))
    {
        return 1;
    }

    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        return 2;
    }

    unsigned char packet_number = 0;
    const bool written = write_spi(
        handle,
        packet_number,
        0x6050,
        colors,
        static_cast<unsigned char>(length));
    hid_close(handle);
    return written ? 0 : 3;
}

int __cdecl jc_write_spi(
    const wchar_t* device_key,
    unsigned int offset,
    const unsigned char* data,
    int length)
{
    if (!data || !is_safe_write_range(offset, length))
    {
        return 1;
    }

    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        return 2;
    }

    unsigned char packet_number = 0;
    int position = 0;
    while (position < length)
    {
        const auto count = static_cast<unsigned char>((std::min)(29, length - position));
        if (!write_spi(handle, packet_number, offset + position, data + position, count))
        {
            hid_close(handle);
            return 3;
        }
        position += count;
    }

    hid_close(handle);
    return 0;
}

int __cdecl jc_identify(const wchar_t* device_key)
{
    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        return 2;
    }

    unsigned char packet_number = 0;
    unsigned char reply[report_size]{};
    const unsigned char enable_rumble[] = { 0x01 };
    if (!send_subcommand(handle, packet_number, 0x48,
        enable_rumble, sizeof(enable_rumble), 0x80, reply))
    {
        hid_close(handle);
        return 3;
    }

    unsigned char report[report_size]{};
    report[0] = 0x10;
    report[1] = packet_number++ & 0x0f;
    const unsigned char pulse[] = { 0xc2, 0xc8, 0x03, 0x72 };
    std::memcpy(report + 2, pulse, sizeof(pulse));
    std::memcpy(report + 6, pulse, sizeof(pulse));
    bool success = write_raw_report(handle, report);
    Sleep(90);

    report[1] = packet_number++ & 0x0f;
    const unsigned char neutral[] = { 0x00, 0x01, 0x40, 0x40 };
    std::memcpy(report + 2, neutral, sizeof(neutral));
    std::memcpy(report + 6, neutral, sizeof(neutral));
    success = write_raw_report(handle, report) && success;

    const unsigned char disable_rumble[] = { 0x00 };
    success = send_subcommand(handle, packet_number, 0x48,
        disable_rumble, sizeof(disable_rumble), 0x80, reply) && success;
    hid_close(handle);
    return success ? 0 : 3;
}

int __cdecl jc_play_rumble_raw(
    const wchar_t* device_key,
    const unsigned char* samples,
    int sample_count,
    int sample_rate_ms)
{
    if (!device_key || !samples || sample_count <= 0 || sample_count > 10000000
        || sample_rate_ms <= 0 || sample_rate_ms > 1000)
    {
        return 1;
    }

    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        return 2;
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> guard(rumble_mutex);
        if (rumble_cancellations.find(device_key) != rumble_cancellations.end())
        {
            hid_close(handle);
            return 5;
        }
        rumble_cancellations.emplace(device_key, cancelled);
    }

    unsigned char packet_number = 0;
    unsigned char reply[report_size]{};
    const unsigned char enable_rumble[] = { 0x01 };
    bool success = send_subcommand(handle, packet_number, 0x48,
        enable_rumble, sizeof(enable_rumble), 0x80, reply);

    for (int index = 0; success && index < sample_count && !cancelled->load(); ++index)
    {
        sleep_interruptibly(sample_rate_ms, cancelled);
        if (cancelled->load())
        {
            break;
        }

        unsigned char report[report_size]{};
        report[0] = 0x10;
        report[1] = packet_number++ & 0x0f;
        std::memcpy(report + 2, samples + index * 4, 4);
        std::memcpy(report + 6, samples + index * 4, 4);
        success = write_raw_report(handle, report);
    }

    unsigned char neutral_report[report_size]{};
    neutral_report[0] = 0x10;
    neutral_report[1] = packet_number++ & 0x0f;
    const unsigned char neutral[] = { 0x00, 0x01, 0x40, 0x40 };
    std::memcpy(neutral_report + 2, neutral, 4);
    std::memcpy(neutral_report + 6, neutral, 4);
    write_raw_report(handle, neutral_report);

    const unsigned char disable_rumble[] = { 0x00 };
    send_subcommand(handle, packet_number, 0x48,
        disable_rumble, sizeof(disable_rumble), 0x80, reply);
    hid_close(handle);

    {
        std::lock_guard<std::mutex> guard(rumble_mutex);
        rumble_cancellations.erase(device_key);
    }
    return !success ? 3 : (cancelled->load() ? 4 : 0);
}

int __cdecl jc_stop_rumble(const wchar_t* device_key)
{
    if (!device_key)
    {
        return 1;
    }

    std::lock_guard<std::mutex> guard(rumble_mutex);
    const auto found = rumble_cancellations.find(device_key);
    if (found == rumble_cancellations.end())
    {
        return 0;
    }
    found->second->store(true);
    return 0;
}

int __cdecl jc_send_diagnostic(
    const wchar_t* device_key,
    int internal_command,
    const unsigned char* arguments,
    int argument_length,
    jc_diagnostic_reply* reply)
{
    if (!device_key || !arguments || argument_length < 6 || argument_length > 44 || !reply)
    {
        return 1;
    }
    std::memset(reply, 0, sizeof(*reply));

    unsigned char command[report_size]{};
    command[0] = arguments[0];
    command[1] = 0;
    command[2] = command[6] = arguments[1];
    command[3] = command[7] = arguments[2];
    command[4] = command[8] = arguments[3];
    command[5] = command[9] = arguments[4];
    command[10] = arguments[5];
    if (command[0] == 0x01 || command[0] == 0x10 || command[0] == 0x11)
    {
        for (int index = 6; index < argument_length; ++index)
        {
            command[5 + index] = arguments[index];
        }
    }
    else
    {
        for (int index = 6; index < argument_length && index - 5 < static_cast<int>(report_size); ++index)
        {
            command[index - 5] = arguments[index];
        }
    }

    if (!is_safe_debug_command(command)
        && !(internal_command != 0 && is_safe_internal_command(command)))
    {
        return 2;
    }

    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        return 3;
    }

    const int written = hid_write(handle, command, sizeof(command));
    if (written < static_cast<int>(sizeof(command)))
    {
        hid_close(handle);
        return 3;
    }

    unsigned char received[0x170]{};
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const int read = hid_read_timeout(handle, received, sizeof(received), 64);
        if (read < 0)
        {
            hid_close(handle);
            return 3;
        }
        if (read == 0)
        {
            continue;
        }
        if (read > static_cast<int>(sizeof(reply->data)))
        {
            reply->length = sizeof(reply->data);
        }
        else
        {
            reply->length = read;
        }
        std::memcpy(reply->data, received, reply->length);
        if (read >= 15 && received[0] == 0x21 && received[14] == command[10])
        {
            reply->matched = 1;
            reply->accepted = (received[13] & 0x80) != 0;
            break;
        }
    }
    hid_close(handle);

    if (command[10] == 0x06 && internal_command != 0 && !reply->matched)
    {
        return 0;
    }
    return reply->matched && reply->accepted ? 0 : 4;
}

int __cdecl jc_read_input(const wchar_t* device_key, jc_input_snapshot* snapshot)
{
    const int start_result = jc_start_input_stream(device_key);
    if (start_result != 0)
    {
        return start_result;
    }

    int read_result = 4;
    for (int attempt = 0; attempt < 6 && read_result == 4; ++attempt)
    {
        read_result = jc_read_input_stream(device_key, snapshot, 120);
    }
    jc_stop_input_stream(device_key);
    return read_result == 0 ? 0 : 3;
}

int __cdecl jc_start_input_stream(const wchar_t* device_key)
{
    if (!device_key || !device_key[0])
    {
        return 1;
    }

    auto session = std::make_shared<input_stream_session>();
    session->handle = open_device(device_key);
    if (!session->handle)
    {
        return 2;
    }

    const std::wstring key(device_key);
    {
        std::lock_guard<std::mutex> guard(input_streams_mutex);
        if (input_streams.find(key) != input_streams.end())
        {
            hid_close(session->handle);
            return 5;
        }
        input_streams.emplace(key, session);
    }

    unsigned char reply[report_size]{};
    const unsigned char standard_report[] = { 0x30 };
    const unsigned char enable_imu[] = { 0x01 };
    const bool success = send_subcommand(session->handle, session->packet_number, 0x03,
        standard_report, sizeof(standard_report), 0x80, reply)
        && send_subcommand(session->handle, session->packet_number, 0x40,
            enable_imu, sizeof(enable_imu), 0x80, reply);

    if (!success)
    {
        {
            std::lock_guard<std::mutex> guard(input_streams_mutex);
            const auto found = input_streams.find(key);
            if (found != input_streams.end() && found->second == session)
            {
                input_streams.erase(found);
            }
        }
        hid_close(session->handle);
        return 3;
    }
    return 0;
}

int __cdecl jc_read_input_stream(
    const wchar_t* device_key,
    jc_input_snapshot* snapshot,
    int timeout_ms)
{
    if (!device_key || !snapshot || timeout_ms <= 0 || timeout_ms > 1000)
    {
        return 1;
    }
    std::memset(snapshot, 0, sizeof(*snapshot));

    std::shared_ptr<input_stream_session> session;
    {
        std::lock_guard<std::mutex> guard(input_streams_mutex);
        const auto found = input_streams.find(device_key);
        if (found == input_streams.end())
        {
            return 5;
        }
        session = found->second;
    }

    std::lock_guard<std::mutex> io_guard(session->io_mutex);
    if (session->stopping.load())
    {
        return 4;
    }

    unsigned char reply[report_size]{};
    const int read = hid_read_timeout(session->handle, reply, sizeof(reply), timeout_ms);
    if (read < 0)
    {
        return 3;
    }
    if (read == 0 || !parse_input_report(reply, read, snapshot))
    {
        return 4;
    }
    return 0;
}

int __cdecl jc_stop_input_stream(const wchar_t* device_key)
{
    if (!device_key || !device_key[0])
    {
        return 1;
    }

    const std::wstring key(device_key);
    std::shared_ptr<input_stream_session> session;
    {
        std::lock_guard<std::mutex> guard(input_streams_mutex);
        const auto found = input_streams.find(key);
        if (found == input_streams.end())
        {
            return 0;
        }
        session = found->second;
        session->stopping.store(true);
    }

    std::lock_guard<std::mutex> io_guard(session->io_mutex);
    unsigned char reply[report_size]{};
    const unsigned char simple_report[] = { 0x3f };
    const unsigned char disable_imu[] = { 0x00 };
    bool success = send_subcommand(session->handle, session->packet_number, 0x03,
        simple_report, sizeof(simple_report), 0x80, reply);
    success = send_subcommand(session->handle, session->packet_number, 0x40,
        disable_imu, sizeof(disable_imu), 0x80, reply) && success;
    hid_close(session->handle);

    {
        std::lock_guard<std::mutex> guard(input_streams_mutex);
        const auto found = input_streams.find(key);
        if (found != input_streams.end() && found->second == session)
        {
            input_streams.erase(found);
        }
    }
    return success ? 0 : 3;
}
