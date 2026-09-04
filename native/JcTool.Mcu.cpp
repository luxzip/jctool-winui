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
#include <vector>

#include "../jctool/hidapi.h"

namespace
{
    constexpr int output_report_size = 49;
    constexpr int mcu_report_size = 0x170;

    struct ir_session
    {
        hid_device* handle = nullptr;
        unsigned char packet_number = 0;
        int width = 0;
        int height = 0;
        int max_fragment = 0;
        int received_count = 0;
        int last_fragment = 0;
        int simulated_step = 0;
        unsigned int frame_number = 0;
        bool simulated = false;
        std::vector<unsigned char> frame;
        std::vector<unsigned char> received;
        std::mutex io_mutex;
        std::atomic_bool stopping{ false };
    };

    std::mutex ir_sessions_mutex;
    std::unordered_map<std::wstring, std::shared_ptr<ir_session>> ir_sessions;
    std::mutex nfc_sessions_mutex;
    std::unordered_map<std::wstring, std::shared_ptr<std::atomic_bool>> nfc_cancellations;

    bool is_simulated(const wchar_t* device_key)
    {
        return device_key && wcsncmp(device_key, L"jctool-sim://", 13) == 0;
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

    unsigned char mcu_crc8(const unsigned char* data, int length)
    {
        unsigned char crc = 0;
        for (int index = 0; index < length; ++index)
        {
            crc ^= data[index];
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = static_cast<unsigned char>(
                    crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
            }
        }
        return crc;
    }

    bool wait_for_subcommand_ack(hid_device* handle, unsigned char subcommand)
    {
        unsigned char reply[mcu_report_size]{};
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const int read = hid_read_timeout(handle, reply, sizeof(reply), 80);
            if (read >= 15 && reply[0] == 0x21
                && (reply[13] & 0x80) != 0 && reply[14] == subcommand)
            {
                return true;
            }
            if (read < 0)
            {
                return false;
            }
        }
        return false;
    }

    bool send_subcommand(
        hid_device* handle,
        unsigned char& packet_number,
        unsigned char subcommand,
        const unsigned char* arguments,
        int argument_count)
    {
        if (!handle || argument_count < 0 || argument_count > output_report_size - 11)
        {
            return false;
        }

        unsigned char command[output_report_size]{};
        command[0] = 0x01;
        command[1] = packet_number++ & 0x0f;
        command[10] = subcommand;
        if (arguments && argument_count > 0)
        {
            std::memcpy(command + 11, arguments, argument_count);
        }
        return hid_write(handle, command, sizeof(command)) >= output_report_size
            && wait_for_subcommand_ack(handle, subcommand);
    }

    bool write_mcu_payload(
        hid_device* handle,
        unsigned char& packet_number,
        const unsigned char* payload,
        int payload_length)
    {
        if (!handle || !payload || payload_length <= 0 || payload_length > 37)
        {
            return false;
        }

        unsigned char command[output_report_size]{};
        command[0] = 0x01;
        command[1] = packet_number++ & 0x0f;
        command[10] = 0x21;
        std::memcpy(command + 11, payload, payload_length);
        command[48] = mcu_crc8(command + 12, 36);
        return hid_write(handle, command, sizeof(command)) >= output_report_size;
    }

    bool wait_for_mcu_ack(hid_device* handle)
    {
        unsigned char reply[mcu_report_size]{};
        for (int attempt = 0; attempt < 14; ++attempt)
        {
            const int read = hid_read_timeout(handle, reply, sizeof(reply), 80);
            if (read >= 16 && reply[0] == 0x21 && reply[14] == 0x21)
            {
                return true;
            }
            if (read < 0)
            {
                return false;
            }
        }
        return false;
    }

    bool request_mcu_mode(
        hid_device* handle,
        unsigned char& packet_number,
        unsigned char expected_mode)
    {
        for (int request = 0; request < 8; ++request)
        {
            unsigned char command[output_report_size]{};
            command[0] = 0x11;
            command[1] = packet_number++ & 0x0f;
            command[10] = 0x01;
            if (hid_write(handle, command, sizeof(command)) < output_report_size)
            {
                return false;
            }

            unsigned char reply[mcu_report_size]{};
            for (int attempt = 0; attempt < 10; ++attempt)
            {
                const int read = hid_read_timeout(handle, reply, sizeof(reply), 80);
                if (read >= 57 && reply[0] == 0x31
                    && reply[49] == 0x01 && reply[56] == expected_mode)
                {
                    return true;
                }
                if (read < 0)
                {
                    return false;
                }
            }
        }
        return false;
    }

    bool initialize_mcu(
        hid_device* handle,
        unsigned char& packet_number,
        unsigned char mode)
    {
        const unsigned char report_mode[] = { 0x31 };
        const unsigned char enable_mcu[] = { 0x01 };
        if (!send_subcommand(handle, packet_number, 0x03,
                report_mode, sizeof(report_mode))
            || !send_subcommand(handle, packet_number, 0x22,
                enable_mcu, sizeof(enable_mcu))
            || !request_mcu_mode(handle, packet_number, 0x01))
        {
            return false;
        }

        const unsigned char mode_payload[] = { 0x21, 0x00, mode };
        return write_mcu_payload(handle, packet_number, mode_payload, sizeof(mode_payload))
            && wait_for_mcu_ack(handle)
            && request_mcu_mode(handle, packet_number, mode);
    }

    void shutdown_mcu(hid_device* handle, unsigned char& packet_number)
    {
        const unsigned char disable_mcu[] = { 0x00 };
        const unsigned char simple_report[] = { 0x3f };
        send_subcommand(handle, packet_number, 0x22,
            disable_mcu, sizeof(disable_mcu));
        send_subcommand(handle, packet_number, 0x03,
            simple_report, sizeof(simple_report));
    }

    void write_register(unsigned char* payload, int index, std::uint16_t address, unsigned char value)
    {
        const int offset = 3 + index * 3;
        payload[offset] = static_cast<unsigned char>(address & 0xff);
        payload[offset + 1] = static_cast<unsigned char>(address >> 8);
        payload[offset + 2] = value;
    }

    bool send_ir_status_request(
        hid_device* handle,
        unsigned char& packet_number)
    {
        unsigned char command[output_report_size]{};
        command[0] = 0x11;
        command[1] = packet_number++ & 0x0f;
        command[10] = 0x03;
        command[11] = 0x02;
        command[47] = mcu_crc8(command + 11, 36);
        command[48] = 0xff;
        return hid_write(handle, command, sizeof(command)) >= output_report_size;
    }

    bool configure_ir(ir_session& session, const jc_ir_config& config)
    {
        static const unsigned char resolution_registers[] = { 0x69, 0x64, 0x50, 0x00 };
        static const int widths[] = { 40, 80, 160, 320 };
        static const int heights[] = { 30, 60, 120, 240 };
        static const int max_fragments[] = { 3, 15, 63, 255 };
        if (config.resolution < 0 || config.resolution > 3
            || config.exposure_microseconds < 0 || config.exposure_microseconds > 600
            || config.digital_gain < 1 || config.digital_gain > 255)
        {
            return false;
        }

        session.width = widths[config.resolution];
        session.height = heights[config.resolution];
        session.max_fragment = max_fragments[config.resolution];
        session.frame.resize(session.width * session.height);
        session.received.assign(session.max_fragment + 1, 0);

        if (session.simulated)
        {
            return true;
        }
        if (!initialize_mcu(session.handle, session.packet_number, 0x05))
        {
            return false;
        }

        const unsigned char mode_payload[] = {
            0x23, 0x01, 0x07,
            static_cast<unsigned char>(session.max_fragment),
            0x00, 0x05, 0x00, 0x18
        };
        if (!write_mcu_payload(session.handle, session.packet_number,
                mode_payload, sizeof(mode_payload))
            || !wait_for_mcu_ack(session.handle))
        {
            return false;
        }

        const auto exposure = static_cast<std::uint16_t>(
            config.exposure_microseconds * 31200 / 1000);
        unsigned char registers1[30] = { 0x23, 0x04, 0x09 };
        write_register(registers1, 0, 0x2e00, resolution_registers[config.resolution]);
        write_register(registers1, 1, 0x3001, static_cast<unsigned char>(exposure & 0xff));
        write_register(registers1, 2, 0x3101, static_cast<unsigned char>(exposure >> 8));
        write_register(registers1, 3, 0x3201, 0x00);
        write_register(registers1, 4, 0x1000, config.leds_enabled ? 0x00 : 0xc0);
        write_register(registers1, 5, 0x2e01,
            static_cast<unsigned char>((config.digital_gain & 0x0f) << 4));
        write_register(registers1, 6, 0x2f01,
            static_cast<unsigned char>((config.digital_gain & 0xf0) >> 4));
        write_register(registers1, 7, 0x0e00, config.external_light_filter ? 0x03 : 0x00);
        write_register(registers1, 8, 0x4301, 0xc8);
        if (!write_mcu_payload(session.handle, session.packet_number, registers1, sizeof(registers1))
            || !send_ir_status_request(session.handle, session.packet_number)
            || !wait_for_mcu_ack(session.handle))
        {
            return false;
        }

        unsigned char registers2[27] = { 0x23, 0x04, 0x08 };
        write_register(registers2, 0, 0x1100, 0x0f);
        write_register(registers2, 1, 0x1200, 0x10);
        write_register(registers2, 2, 0x2d00, config.flip_horizontal ? 0x02 : 0x00);
        write_register(registers2, 3, 0x6701, config.denoise ? 0x01 : 0x00);
        write_register(registers2, 4, 0x6801, 0x23);
        write_register(registers2, 5, 0x6901, 0x44);
        write_register(registers2, 6, 0x0400, config.resolution == 0 ? 0x2d : 0x32);
        write_register(registers2, 7, 0x0700, 0x01);
        return write_mcu_payload(session.handle, session.packet_number,
                registers2, sizeof(registers2))
            && wait_for_mcu_ack(session.handle);
    }

    bool send_ir_ack(ir_session& session, int fragment, bool request_missing = false)
    {
        unsigned char command[output_report_size]{};
        command[0] = 0x11;
        command[1] = session.packet_number++ & 0x0f;
        command[10] = 0x03;
        if (request_missing)
        {
            command[12] = 0x01;
            command[13] = static_cast<unsigned char>(fragment);
        }
        else
        {
            command[14] = static_cast<unsigned char>(fragment);
        }
        command[47] = mcu_crc8(command + 11, 36);
        command[48] = 0xff;
        return hid_write(session.handle, command, sizeof(command)) >= output_report_size;
    }

    void reset_ir_frame(ir_session& session)
    {
        std::fill(session.received.begin(), session.received.end(), 0);
        session.received_count = 0;
    }

    void fill_ir_info(const ir_session& session, jc_ir_frame_info& info)
    {
        info.width = session.width;
        info.height = session.height;
        info.progress = session.received.empty()
            ? 0
            : session.received_count * 100 / static_cast<int>(session.received.size());
        info.frame_number = session.frame_number;
    }

    void make_simulated_ir_frame(ir_session& session, jc_ir_frame_info& info)
    {
        long long intensity = 0;
        int white = 0;
        for (int y = 0; y < session.height; ++y)
        {
            for (int x = 0; x < session.width; ++x)
            {
                const int center_x = session.width / 2
                    + static_cast<int>(session.frame_number % 9) - 4;
                const int center_y = session.height / 2;
                const int distance = std::abs(x - center_x) + std::abs(y - center_y);
                const auto value = static_cast<unsigned char>((std::max)(
                    18,
                    245 - distance * 7));
                session.frame[y * session.width + x] = value;
                intensity += value;
                if (value == 255)
                {
                    ++white;
                }
            }
        }
        info.average_intensity = static_cast<int>(intensity / session.frame.size());
        info.white_pixels = white;
        info.ambient_pixels = 0;
    }

    bool write_nfc_command(
        hid_device* handle,
        unsigned char& packet_number,
        unsigned char command_id,
        const unsigned char* data,
        int data_length)
    {
        if (data_length < 0 || data_length > 31)
        {
            return false;
        }
        unsigned char command[output_report_size]{};
        command[0] = 0x11;
        command[1] = packet_number++ & 0x0f;
        command[10] = 0x02;
        command[11] = command_id;
        command[14] = 0x08;
        command[15] = static_cast<unsigned char>(data_length);
        if (data && data_length > 0)
        {
            std::memcpy(command + 16, data, data_length);
        }
        command[47] = mcu_crc8(command + 11, 36);
        return hid_write(handle, command, output_report_size - 1) >= output_report_size - 1;
    }

    bool read_mcu_report(
        hid_device* handle,
        unsigned char* report,
        int timeout_ms,
        const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        if (cancelled->load())
        {
            return false;
        }
        std::memset(report, 0, mcu_report_size);
        return hid_read_timeout(handle, report, mcu_report_size, timeout_ms) > 0;
    }

    bool prepare_nfc_polling(
        hid_device* handle,
        unsigned char& packet_number,
        ULONGLONG deadline,
        const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        if (!write_nfc_command(handle, packet_number, 0x04, nullptr, 0))
        {
            return false;
        }

        unsigned char report[mcu_report_size]{};
        bool ready = false;
        while (!cancelled->load() && GetTickCount64() < deadline)
        {
            if (!read_mcu_report(handle, report, 100, cancelled))
            {
                continue;
            }
            if (report[0] == 0x31 && report[49] == 0x2a
                && report[50] == 0x00 && report[51] == 0x05
                && report[55] == 0x31 && report[56] == 0x00)
            {
                ready = true;
                break;
            }
        }
        const unsigned char polling[] = { 0x01, 0x00, 0x00, 0x2c, 0x01 };
        return ready && write_nfc_command(
            handle, packet_number, 0x01, polling, sizeof(polling));
    }

    bool wait_for_nfc_tag(
        hid_device* handle,
        jc_nfc_tag& tag,
        ULONGLONG deadline,
        const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        unsigned char report[mcu_report_size]{};
        while (!cancelled->load() && GetTickCount64() < deadline)
        {
            if (!read_mcu_report(handle, report, 100, cancelled))
            {
                continue;
            }
            if (report[0] != 0x31 || report[49] != 0x2a)
            {
                continue;
            }
            if (report[50] == 0x00 && report[51] == 0x05 && report[56] == 0x09)
            {
                tag.tag_type = report[62];
                tag.uid_length = (std::min)(10, static_cast<int>(report[64]));
                std::memcpy(tag.uid, report + 65, tag.uid_length);
                return true;
            }
        }
        return false;
    }

    bool send_ntag_read_request(
        hid_device* handle,
        unsigned char& packet_number,
        int pages)
    {
        unsigned char request[19] = { 0xd0, 0x07 };
        request[9] = 0x00;
        if (pages == 45)
        {
            request[10] = 1;
            request[11] = 0x00;
            request[12] = 0x2c;
        }
        else if (pages == 135)
        {
            request[10] = 3;
            request[11] = 0x00;
            request[12] = 0x3b;
            request[13] = 0x3c;
            request[14] = 0x77;
            request[15] = 0x78;
            request[16] = 0x86;
        }
        else if (pages == 231)
        {
            request[10] = 4;
            request[11] = 0x00;
            request[12] = 0x3b;
            request[13] = 0x3c;
            request[14] = 0x77;
            request[15] = 0x78;
            request[16] = 0xb3;
            request[17] = 0xb4;
            request[18] = 0xe6;
        }
        else
        {
            request[10] = 1;
        }
        return write_nfc_command(handle, packet_number, 0x06, request, sizeof(request));
    }

    int discover_ntag_pages(
        hid_device* handle,
        unsigned char& packet_number,
        ULONGLONG deadline,
        const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        if (!send_ntag_read_request(handle, packet_number, 0))
        {
            return 0;
        }
        int pages = 0;
        unsigned char report[mcu_report_size]{};
        while (!cancelled->load() && GetTickCount64() < deadline)
        {
            if (!read_mcu_report(handle, report, 100, cancelled))
            {
                continue;
            }
            if (report[0] == 0x31 && report[49] == 0x3a
                && report[51] == 0x07 && report[52] == 0x01)
            {
                switch (report[74])
                {
                case 0: pages = 135; break;
                case 3: pages = 45; break;
                case 4: pages = 231; break;
                default: return 0;
                }
            }
            else if (report[0] == 0x31 && report[49] == 0x2a
                && report[56] == 0x04 && pages > 0)
            {
                return pages;
            }
        }
        return 0;
    }

    int read_ntag_data(
        hid_device* handle,
        unsigned char& packet_number,
        int pages,
        unsigned char* output,
        int capacity,
        ULONGLONG deadline,
        const std::shared_ptr<std::atomic_bool>& cancelled)
    {
        if (!send_ntag_read_request(handle, packet_number, pages))
        {
            return 0;
        }

        int position = 0;
        unsigned char report[mcu_report_size]{};
        while (!cancelled->load() && GetTickCount64() < deadline)
        {
            if (!read_mcu_report(handle, report, 100, cancelled))
            {
                continue;
            }
            if (report[0] != 0x31)
            {
                continue;
            }
            if (report[49] == 0x2a && report[56] == 0x04)
            {
                return position;
            }
            if (report[49] != 0x3a || report[51] != 0x07)
            {
                continue;
            }

            const int payload = ((report[54] << 8) | report[55]) & 0x7ff;
            const int source = report[52] == 0x01 ? 116 : 56;
            const int count = report[52] == 0x01 ? payload - 60 : payload;
            if (count <= 0 || source + count > mcu_report_size
                || position + count > capacity)
            {
                continue;
            }
            std::memcpy(output + position, report + source, count);
            position += count;
            if (!send_ntag_read_request(handle, packet_number, pages))
            {
                return position;
            }
        }
        return position;
    }

    void make_simulated_nfc_tag(jc_nfc_tag& tag, unsigned char* data, int capacity)
    {
        const unsigned char uid[] = { 0x04, 0xa2, 0xb3, 0xc4, 0xd5, 0xe6, 0xf7 };
        std::memcpy(tag.uid, uid, sizeof(uid));
        tag.uid_length = sizeof(uid);
        tag.tag_type = 2;
        tag.tag_model = 213;
        tag.data_length = (std::min)(capacity, 180);
        std::memset(data, 0, tag.data_length);
        if (tag.data_length < 64)
        {
            return;
        }

        data[12] = 0xe1;
        data[13] = 0x10;
        data[14] = 0x12;
        const char text[] = "Simulated Joy-Con NFC tag";
        const int text_length = static_cast<int>(sizeof(text) - 1);
        const int payload_length = 3 + text_length;
        int position = 16;
        data[position++] = 0x03;
        data[position++] = static_cast<unsigned char>(4 + payload_length);
        data[position++] = 0xd1;
        data[position++] = 0x01;
        data[position++] = static_cast<unsigned char>(payload_length);
        data[position++] = 'T';
        data[position++] = 0x02;
        data[position++] = 'e';
        data[position++] = 'n';
        std::memcpy(data + position, text, text_length);
        data[position + text_length] = 0xfe;
    }
}

int __cdecl jc_start_ir_stream(const wchar_t* device_key, const jc_ir_config* config)
{
    if (!device_key || !config)
    {
        return 1;
    }

    const std::wstring key(device_key);
    {
        std::lock_guard<std::mutex> guard(ir_sessions_mutex);
        if (ir_sessions.find(key) != ir_sessions.end())
        {
            return 5;
        }
    }

    auto session = std::make_shared<ir_session>();
    session->handle = open_device(device_key);
    if (!session->handle)
    {
        return 2;
    }
    session->simulated = is_simulated(device_key);
    if (!configure_ir(*session, *config))
    {
        if (!session->simulated)
        {
            shutdown_mcu(session->handle, session->packet_number);
        }
        hid_close(session->handle);
        return 3;
    }
    if (!session->simulated && !send_ir_ack(*session, 0))
    {
        shutdown_mcu(session->handle, session->packet_number);
        hid_close(session->handle);
        return 3;
    }

    std::lock_guard<std::mutex> guard(ir_sessions_mutex);
    if (ir_sessions.find(key) != ir_sessions.end())
    {
        if (!session->simulated)
        {
            shutdown_mcu(session->handle, session->packet_number);
        }
        hid_close(session->handle);
        return 5;
    }
    ir_sessions.emplace(key, session);
    return 0;
}

int __cdecl jc_read_ir_frame_fragment(
    const wchar_t* device_key,
    unsigned char* frame,
    int frame_capacity,
    jc_ir_frame_info* frame_info,
    int timeout_ms)
{
    if (!device_key || !frame || !frame_info || timeout_ms <= 0 || timeout_ms > 1000)
    {
        return 1;
    }
    std::memset(frame_info, 0, sizeof(*frame_info));

    std::shared_ptr<ir_session> session;
    {
        std::lock_guard<std::mutex> guard(ir_sessions_mutex);
        const auto found = ir_sessions.find(device_key);
        if (found == ir_sessions.end())
        {
            return 5;
        }
        session = found->second;
    }
    if (frame_capacity < session->width * session->height)
    {
        return 1;
    }

    std::lock_guard<std::mutex> io_guard(session->io_mutex);
    fill_ir_info(*session, *frame_info);
    if (session->stopping.load())
    {
        return 4;
    }

    if (session->simulated)
    {
        Sleep(static_cast<DWORD>((std::min)(timeout_ms, 12)));
        session->received_count = ++session->simulated_step;
        fill_ir_info(*session, *frame_info);
        if (session->simulated_step < 4)
        {
            return 0;
        }
        session->simulated_step = 0;
        make_simulated_ir_frame(*session, *frame_info);
        std::memcpy(frame, session->frame.data(), session->frame.size());
        frame_info->frame_ready = 1;
        frame_info->progress = 100;
        frame_info->frame_number = ++session->frame_number;
        session->received_count = 0;
        return 0;
    }

    unsigned char reply[mcu_report_size]{};
    const int read = hid_read_timeout(session->handle, reply, sizeof(reply), timeout_ms);
    if (read < 0)
    {
        return 3;
    }
    if (read < 59 || reply[0] != 0x31)
    {
        send_ir_ack(*session, session->last_fragment);
        return 4;
    }
    if (reply[49] != 0x03 || read < 359)
    {
        send_ir_ack(*session, session->last_fragment);
        return 0;
    }

    const int fragment = reply[52];
    if (fragment < 0 || fragment > session->max_fragment)
    {
        return 3;
    }
    const int offset = fragment * 300;
    const int count = (std::min)(300, static_cast<int>(session->frame.size()) - offset);
    std::memcpy(session->frame.data() + offset, reply + 59, count);
    if (!session->received[fragment])
    {
        session->received[fragment] = 1;
        ++session->received_count;
    }
    session->last_fragment = fragment;

    int missing = -1;
    for (int index = 0; index < fragment; ++index)
    {
        if (!session->received[index])
        {
            missing = index;
            break;
        }
    }
    send_ir_ack(*session, missing >= 0 ? missing : fragment, missing >= 0);
    fill_ir_info(*session, *frame_info);
    frame_info->average_intensity = reply[53];
    frame_info->white_pixels = reply[55] | reply[56] << 8;
    frame_info->ambient_pixels = reply[57] | reply[58] << 8;
    if (session->received_count == session->max_fragment + 1)
    {
        std::memcpy(frame, session->frame.data(), session->frame.size());
        frame_info->frame_ready = 1;
        frame_info->progress = 100;
        frame_info->frame_number = ++session->frame_number;
        reset_ir_frame(*session);
    }
    return 0;
}

int __cdecl jc_stop_ir_stream(const wchar_t* device_key)
{
    if (!device_key)
    {
        return 1;
    }
    const std::wstring key(device_key);
    std::shared_ptr<ir_session> session;
    {
        std::lock_guard<std::mutex> guard(ir_sessions_mutex);
        const auto found = ir_sessions.find(key);
        if (found == ir_sessions.end())
        {
            return 0;
        }
        session = found->second;
        session->stopping.store(true);
    }

    std::lock_guard<std::mutex> io_guard(session->io_mutex);
    if (!session->simulated)
    {
        shutdown_mcu(session->handle, session->packet_number);
    }
    hid_close(session->handle);
    {
        std::lock_guard<std::mutex> guard(ir_sessions_mutex);
        const auto found = ir_sessions.find(key);
        if (found != ir_sessions.end() && found->second == session)
        {
            ir_sessions.erase(found);
        }
    }
    return 0;
}

int __cdecl jc_scan_nfc(
    const wchar_t* device_key,
    jc_nfc_tag* tag,
    unsigned char* tag_data,
    int tag_data_capacity,
    int timeout_ms)
{
    if (!device_key || !tag || !tag_data || tag_data_capacity <= 0
        || timeout_ms < 100 || timeout_ms > 120000)
    {
        return 1;
    }
    std::memset(tag, 0, sizeof(*tag));
    std::memset(tag_data, 0, tag_data_capacity);

    const std::wstring key(device_key);
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> guard(nfc_sessions_mutex);
        if (nfc_cancellations.find(key) != nfc_cancellations.end())
        {
            return 5;
        }
        nfc_cancellations.emplace(key, cancelled);
    }

    int result = 3;
    hid_device* handle = open_device(device_key);
    if (!handle)
    {
        result = 2;
    }
    else if (is_simulated(device_key))
    {
        for (int step = 0; step < 4 && !cancelled->load(); ++step)
        {
            Sleep(25);
        }
        if (!cancelled->load())
        {
            make_simulated_nfc_tag(*tag, tag_data, tag_data_capacity);
            result = 0;
        }
    }
    else
    {
        unsigned char packet_number = 0;
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        if (initialize_mcu(handle, packet_number, 0x04)
            && prepare_nfc_polling(handle, packet_number, deadline, cancelled)
            && wait_for_nfc_tag(handle, *tag, deadline, cancelled))
        {
            result = 0;
            if (tag->tag_type == 2)
            {
                tag->tag_model = discover_ntag_pages(
                    handle, packet_number, deadline, cancelled);
                if (tag->tag_model > 0)
                {
                    write_nfc_command(handle, packet_number, 0x02, nullptr, 0);
                    Sleep(200);
                    if (prepare_nfc_polling(handle, packet_number, deadline, cancelled)
                        && wait_for_nfc_tag(handle, *tag, deadline, cancelled))
                    {
                        tag->data_length = read_ntag_data(
                            handle,
                            packet_number,
                            tag->tag_model,
                            tag_data,
                            tag_data_capacity,
                            deadline,
                            cancelled);
                    }
                }
            }
        }
        shutdown_mcu(handle, packet_number);
    }

    if (handle)
    {
        hid_close(handle);
    }
    {
        std::lock_guard<std::mutex> guard(nfc_sessions_mutex);
        nfc_cancellations.erase(key);
    }
    return cancelled->load() ? 4 : result;
}

int __cdecl jc_cancel_nfc(const wchar_t* device_key)
{
    if (!device_key)
    {
        return 1;
    }
    std::lock_guard<std::mutex> guard(nfc_sessions_mutex);
    const auto found = nfc_cancellations.find(device_key);
    if (found == nfc_cancellations.end())
    {
        return 0;
    }
    found->second->store(true);
    return 0;
}
