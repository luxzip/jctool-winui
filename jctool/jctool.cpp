// Copyright (c) 2018 CTCaer. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
// #define NOMINMAX
#include <Windows.h>

#include "jctool.h"
#include "ir_sensor.h"
#include "tune.h"
#include "FormJoy.h"
#include "hidapi_log.h"

using namespace CppWinFormJoy;

#pragma comment(lib, "SetupAPI")

bool enable_traffic_dump = false;
bool check_connection_ok = true;

device_context *device_slots[4] = { nullptr, nullptr, nullptr, nullptr };
static device_context *active_device_context = nullptr;
thread_local device_context *thread_device_context = nullptr;

class device_io_guard {
public:
    explicit device_io_guard(device_context *device) : device_(device) {
        if (device_)
            EnterCriticalSection(&device_->io_lock);
    }
    ~device_io_guard() {
        if (device_)
            LeaveCriticalSection(&device_->io_lock);
    }
private:
    device_context *device_;
};

device_context *current_device_context() {
    return thread_device_context ? thread_device_context : active_device_context;
}

device_context *get_device_slot(int slot) {
    if (slot < 0 || slot >= 4)
        return nullptr;
    return device_slots[slot];
}

void set_active_device_context(device_context *device) {
    active_device_context = device;
    thread_device_context = device;
}

void set_thread_device_context(device_context *device) {
    thread_device_context = device;
}

static int get_device_type(unsigned short product_id) {
    switch (product_id) {
        case 0x2006: return 1; // Joy-Con (L)
        case 0x2007: return 2; // Joy-Con (R)
        case 0x2009: return 3; // Pro Controller
        default: return 0;
    }
}

static device_context *find_device_session(const hid_device_info *info) {
    for (int i = 0; i < 4; ++i) {
        device_context *device = device_slots[i];
        if (!device)
            continue;

        if (info->path && device->path == info->path)
            return device;

        if (info->serial_number && info->serial_number[0] &&
            wcscmp(info->serial_number, L"000000000001") != 0 &&
            device->product_id == info->product_id && device->serial == info->serial_number)
            return device;
    }
    return nullptr;
}

int refresh_device_sessions() {
    hid_init();
    for (int i = 0; i < 4; ++i) {
        if (device_slots[i])
            device_slots[i]->present = false;
    }

    hid_device_info *devices = hid_enumerate(0x057E, 0x0000);
    for (hid_device_info *info = devices; info; info = info->next) {
        int type = get_device_type(info->product_id);
        if (!type || !info->path)
            continue;

        device_context *device = find_device_session(info);
        if (!device) {
            int empty_slot = -1;
            for (int i = 0; i < 4; ++i) {
                if (!device_slots[i]) {
                    empty_slot = i;
                    break;
                }
            }
            if (empty_slot < 0)
                continue;

            device = new device_context();
            device->slot = empty_slot;
            device_slots[empty_slot] = device;
        }

        device->device_type = type;
        device->product_id = info->product_id;
        device->path = info->path;
        device->serial = info->serial_number ? info->serial_number : L"";
        device->present = true;

        if (!device->connected && device->hid_handle && device->task_kind == DEVICE_TASK_NONE) {
            hid_close(device->hid_handle);
            device->hid_handle = nullptr;
        }
        if (!device->hid_handle) {
            device->hid_handle = hid_open_path(device->path.c_str());
            device->connected = device->hid_handle != nullptr;
            device->initialized = false;
            device->packet_counter = 0;
        }
    }
    hid_free_enumeration(devices);

    int connected_count = 0;
    for (int i = 0; i < 4; ++i) {
        device_context *device = device_slots[i];
        if (!device)
            continue;

        if (!device->present) {
            device->connected = false;
            device->cancel_requested = true;
            device->enable_button_test_state = false;
            device->enable_ir_video_photo_state = false;
            device->enable_nfc_scanning_state = false;
            device->cancel_spi_dump_state = true;
            if (device->task_kind == DEVICE_TASK_NONE && device->hid_handle) {
                hid_close(device->hid_handle);
                device->hid_handle = nullptr;
                device->initialized = false;
            }
        }
        else if (device->connected) {
            connected_count++;
        }
    }

    if (!active_device_context || !active_device_context->connected) {
        active_device_context = nullptr;
        thread_device_context = nullptr;
        for (int i = 0; i < 4; ++i) {
            if (device_slots[i] && device_slots[i]->connected) {
                set_active_device_context(device_slots[i]);
                break;
            }
        }
    }
    return connected_count;
}

void close_device_sessions() {
    for (int i = 0; i < 4; ++i) {
        device_context *device = device_slots[i];
        if (!device)
            continue;
        device->cancel_requested = true;
        device->enable_button_test_state = false;
        device->enable_ir_video_photo_state = false;
        device->enable_nfc_scanning_state = false;
        device->cancel_spi_dump_state = true;
        if (device->hid_handle) {
            hid_close(device->hid_handle);
            device->hid_handle = nullptr;
        }
        delete device;
        device_slots[i] = nullptr;
    }
    active_device_context = nullptr;
    thread_device_context = nullptr;
    hid_exit();
}

s16 uint16_to_int16(u16 a) {
    s16 b;
    char* aPointer = (char*)&a, *bPointer = (char*)&b;
    memcpy(bPointer, aPointer, sizeof(a));
    return b;
}


u16 int16_to_uint16(s16 a) {
    u16 b;
    char* aPointer = (char*)&a, *bPointer = (char*)&b;
    memcpy(bPointer, aPointer, sizeof(a));
    return b;
}


u8 mcu_crc8_calc(u8 *buf, u8 size) {
    u8 crc8 = 0x0;

    for (int i = 0; i < size; ++i) {
        crc8 = mcu_crc8_table[(u8)(crc8 ^ buf[i])];
    }
    return crc8;
}


void decode_stick_params(u16 *decoded_stick_params, u8 *encoded_stick_params) {
    decoded_stick_params[0] = (encoded_stick_params[1] << 8) & 0xF00 | encoded_stick_params[0];
    decoded_stick_params[1] = (encoded_stick_params[2] << 4) | (encoded_stick_params[1] >> 4);
}


void encode_stick_params(u8 *encoded_stick_params, u16 *decoded_stick_params) {
    encoded_stick_params[0] =  decoded_stick_params[0] & 0xFF;
    encoded_stick_params[1] = (decoded_stick_params[0] & 0xF00) >> 8 | (decoded_stick_params[1] & 0xF) << 4;
    encoded_stick_params[2] = (decoded_stick_params[1] & 0xFF0) >> 4;
}


// Credit to Hypersect (Ryan Juckett)
// http://blog.hypersect.com/interpreting-analog-sticks/
void AnalogStickCalc(
    float *pOutX,       // out: resulting stick X value
    float *pOutY,       // out: resulting stick Y value
    u16 x,              // in: initial stick X value
    u16 y,              // in: initial stick Y value
    u16 x_calc[3],      // calc -X, CenterX, +X
    u16 y_calc[3]       // calc -Y, CenterY, +Y
)
{
    float x_f, y_f;
    // Apply Joy-Con center deadzone. 0xAE translates approx to 15%. Pro controller has a 10% deadzone.
    float deadZoneCenter = 0.15f;
    // Add a small ammount of outer deadzone to avoid edge cases or machine variety.
    float deadZoneOuter = 0.10f;

    // convert to float based on calibration and valid ranges per +/-axis
    x = CLAMP(x, x_calc[0], x_calc[2]);
    y = CLAMP(y, y_calc[0], y_calc[2]);
    if (x >= x_calc[1])
        x_f = (float)(x - x_calc[1]) / (float)(x_calc[2] - x_calc[1]);
    else
        x_f = -((float)(x - x_calc[1]) / (float)(x_calc[0] - x_calc[1]));
    if (y >= y_calc[1])
        y_f = (float)(y - y_calc[1]) / (float)(y_calc[2] - y_calc[1]);
    else
        y_f = -((float)(y - y_calc[1]) / (float)(y_calc[0] - y_calc[1]));

    // Interpolate zone between deadzones
    float mag = sqrtf(x_f*x_f + y_f*y_f);
    if (mag > deadZoneCenter) {
        // scale such that output magnitude is in the range [0.0f, 1.0f]
        float legalRange = 1.0f - deadZoneOuter - deadZoneCenter;
        float normalizedMag = min(1.0f, (mag - deadZoneCenter) / legalRange);
        float scale = normalizedMag / mag;
        pOutX[0] = x_f * scale;
        pOutY[0] = y_f * scale;
    }
    else
    {
        // stick is in the inner dead zone
        pOutX[0] = 0.0f;
        pOutY[0] = 0.0f;
    }
}


int set_led_busy() {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x30;
    pkt->subcmd_arg.arg1 = 0x81;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    //Set breathing HOME Led
    if (handle_ok != 1) {
        memset(buf, 0, sizeof(buf));
        hdr = (brcm_hdr *)buf;
        pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x38;
        pkt->subcmd_arg.arg1 = 0x28;
        pkt->subcmd_arg.arg2 = 0x20;
        buf[13] = 0xF2;
        buf[14] = buf[15] = 0xF0;
        res = hid_write(handle, buf, sizeof(buf));
        res = hid_read_timeout(handle, buf, 0, 64);
    }

    return 0;
}


std::string get_sn(u32 offset, const u16 read_len) {
    device_io_guard io_guard(current_device_context());
    u8 data[0x1D];
    std::string test = "";
    memset(data, 0, sizeof(data));
    if (read_len == 0 || read_len > sizeof(data) || get_spi_data(offset, read_len, data) != 0)
        return "Error!";

    for (int i = 0; i < read_len; i++) {
        if (data[i] != 0)
            test += data[i];
    }
    return test;
}


static bool is_safe_spi_read_range(u32 offset, u16 read_len) {
    return read_len > 0 && read_len <= 0x1D && offset <= 0x7FFFF &&
        read_len <= 0x80000 - offset;
}

static bool is_valid_spi_read_reply(const u8 *buf, int res, u32 offset, u16 read_len) {
    return buf && is_safe_spi_read_range(offset, read_len) &&
        res >= 0x14 + read_len && buf[0] == 0x21 &&
        buf[0xD] == 0x90 && buf[0xE] == 0x10 &&
        *(const u32 *)&buf[0xF] == offset && buf[0x13] == read_len;
}

int get_spi_data(u32 offset, const u16 read_len, u8 *test_buf) {
    device_io_guard io_guard(current_device_context());
    if (!test_buf || !is_safe_spi_read_range(offset, read_len))
        return 1;

    u8 buf[49];
    for (int command_attempt = 0; command_attempt < 3; ++command_attempt) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x10;
        pkt->spi_data.offset = offset;
        pkt->spi_data.size = read_len;
        if (hid_write(handle, buf, sizeof(buf)) < (int)sizeof(buf))
            continue;

        for (int read_attempt = 0; read_attempt < 9; ++read_attempt) {
            memset(buf, 0, sizeof(buf));
            int res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (is_valid_spi_read_reply(buf, res, offset, read_len)) {
                memcpy(test_buf, buf + 0x14, read_len);
                return 0;
            }
            if (res <= 0)
                break;
        }
    }
    return 1;
}


static bool is_safe_spi_write_range(u32 offset, u16 write_len) {
    if (write_len == 0 || write_len > 0x1D || offset > 0x7FFFF ||
        write_len > 0x80000 - offset)
        return false;
    u32 end = offset + write_len;
    return (offset >= 0x6000 && end <= 0x7000) ||
        (offset >= 0x8000 && end <= 0x9000) ||
        (offset >= 0xF000 && end <= 0xF010);
}

int write_spi_data(u32 offset, const u16 write_len, u8* test_buf) {
    device_io_guard io_guard(current_device_context());
    if (!test_buf || !is_safe_spi_write_range(offset, write_len))
        return 1;
    int res;
    u8 buf[49];
    int error_writing = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x11;
        pkt->spi_data.offset = offset;
        pkt->spi_data.size = write_len;
        for (int i = 0; i < write_len; i++)
            buf[0x10 + i] = test_buf[i];

        res = hid_write(handle, buf, sizeof(buf));
        if (res < (int)sizeof(buf))
            return 1;
        int retries = 0;
        while (1) {
            memset(buf, 0, sizeof(buf));
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (res > 15 && buf[0] == 0x21 && buf[0xD] == 0x80 &&
                buf[0xE] == 0x11 && buf[0xF] == 0)
                goto check_result;
            if (res > 15 && buf[0] == 0x21 && buf[0xD] == 0x80 && buf[0xE] == 0x11)
                return 1;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_writing++;
        if (error_writing == 3)
            return 1;
    }
    check_result:
    Sleep(20);
    u8 verify_buf[0x21];
    memset(verify_buf, 0, sizeof(verify_buf));
    if (get_spi_data(offset, write_len, verify_buf) != 0 ||
        memcmp(test_buf, verify_buf, write_len) != 0)
        return 1;
    return 0;
}


int get_device_info(u8* test_buf) {
    device_io_guard io_guard(current_device_context());
    if (!test_buf)
        return 1;

    u8 buf[49];
    for (int command_attempt = 0; command_attempt < 3; ++command_attempt) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x02;
        if (hid_write(handle, buf, sizeof(buf)) < (int)sizeof(buf))
            continue;

        for (int read_attempt = 0; read_attempt < 9; ++read_attempt) {
            memset(buf, 0, sizeof(buf));
            int res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (res >= 0x19 && buf[0] == 0x21 &&
                buf[0xD] == 0x82 && buf[0xE] == 0x02) {
                memcpy(test_buf, buf + 0xF, 0xA);
                return 0;
            }
            if (res <= 0)
                break;
        }
    }
    return 1;
}


int get_battery(u8* test_buf) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    int error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x50;
        res = hid_write(handle, buf, sizeof(buf));
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x50D0)
                goto check_result;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 20)
            break;
    }
    check_result:
    test_buf[0] = buf[0x2];
    test_buf[1] = buf[0xF];
    test_buf[2] = buf[0x10];

    return 0;
}


int get_temperature(u8* test_buf) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    int error_reading = 0;
    bool imu_changed = false;

    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x43;
        pkt->subcmd_arg.arg1 = 0x10;
        pkt->subcmd_arg.arg2 = 0x01;
        res = hid_write(handle, buf, sizeof(buf));
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x43C0)
                goto check_result;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 20)
            break;
    }
    check_result:
    if ((buf[0x11] >> 4) == 0x0) {

        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x40;
        pkt->subcmd_arg.arg1 = 0x01;
        res = hid_write(handle, buf, sizeof(buf));
        res = hid_read_timeout(handle, buf, 0, 64);

        imu_changed = true;

        // Let temperature sensor stabilize for a little bit.
        Sleep(64);
    }
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x43;
        pkt->subcmd_arg.arg1 = 0x20;
        pkt->subcmd_arg.arg2 = 0x02;
        res = hid_write(handle, buf, sizeof(buf));
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x43C0)
                goto check_result2;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 20)
            break;
    }
    check_result2:
    test_buf[0] = buf[0x11];
    test_buf[1] = buf[0x12];

    if (imu_changed) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x40;
        pkt->subcmd_arg.arg1 = 0x00;
        res = hid_write(handle, buf, sizeof(buf));
        res = hid_read_timeout(handle, buf, 0, 64);
    }

    return 0;
}


int dump_spi(const char *dev_name) {
    device_io_guard io_guard(current_device_context());
    std::string file_dev_name = dev_name;

    if (file_dev_name.size() < 2 || file_dev_name[1] != ':')
        file_dev_name = "./" + file_dev_name;

    FILE *f;
    errno_t err;

    if ((err = fopen_s(&f, file_dev_name.c_str(), "wb")) != 0) {
        return 1;
    }

    int result = 0;
    u8 block[0x1D];
    u8 verify_block[0x1D];
    u16 read_len = 0x1d;
    u32 offset = 0x0;
    while (offset < 0x80000 && !cancel_spi_dump) {
        current_device_context()->task_progress = (int)((offset * 100ULL) / 0x80000);
        bool block_ok = false;
        for (int attempt = 0; attempt < 3 && !cancel_spi_dump; ++attempt) {
            memset(block, 0, sizeof(block));
            if (get_spi_data(offset, read_len, block) != 0)
                continue;

            memset(verify_block, 0, sizeof(verify_block));
            if (get_spi_data(offset, read_len, verify_block) != 0 ||
                memcmp(block, verify_block, read_len) != 0)
                continue;
            block_ok = true;
            break;
        }

        if (cancel_spi_dump)
            break;
        if (!block_ok || fwrite(block, read_len, 1, f) != 1) {
            result = 1;
            break;
        }
        offset += read_len;
        if (offset == 0x7FFE6)
            read_len = 0x1A;
    }
    if (fclose(f) != 0)
        result = 1;
    return result;
}


int send_rumble() {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    u8 buf2[49];
    
    //Enable Vibration
    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x48;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf2, 0, 64);

    //New vibration like switch
    Sleep(16);
    //Send confirmation 
    memset(buf, 0, sizeof(buf));
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0xc2;
    hdr->rumble_l[1] = 0xc8;
    hdr->rumble_l[2] = 0x03;
    hdr->rumble_l[3] = 0x72;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf2, 0, 64);

    Sleep(81);

    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0x00;
    hdr->rumble_l[1] = 0x01;
    hdr->rumble_l[2] = 0x40;
    hdr->rumble_l[3] = 0x40;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf2, 0, 64);

    Sleep(5);

    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0xc3;
    hdr->rumble_l[1] = 0xc8;
    hdr->rumble_l[2] = 0x60;
    hdr->rumble_l[3] = 0x64;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf2, 0, 64);

    Sleep(5);

    //Disable vibration
    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0x00;
    hdr->rumble_l[1] = 0x01;
    hdr->rumble_l[2] = 0x40;
    hdr->rumble_l[3] = 0x40;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    pkt->subcmd = 0x48;
    pkt->subcmd_arg.arg1 = 0x00;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x30;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    // Set HOME Led
    if (handle_ok != 1) {
        memset(buf, 0, sizeof(buf));
        hdr = (brcm_hdr *)buf;
        pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x38;
        // Heartbeat style configuration
        buf[11] = 0xF1;
        buf[12] = 0x00;
        buf[13] = buf[14] = buf[15] = buf[16] = buf[17] = buf[18] = 0xF0;
        buf[19] = buf[22] = buf[25] = buf[28] = buf[31] = 0x00;
        buf[20] = buf[21] = buf[23] = buf[24] = buf[26] = buf[27] = buf[29] = buf[30] = buf[32] = buf[33] = 0xFF;
        res = hid_write(handle, buf, sizeof(buf));
        res = hid_read_timeout(handle, buf, 0, 64);
    }

    return 0;
}


static bool is_safe_debug_command(const u8 *buf_cmd) {
    if (buf_cmd[0] != 0x01)
        return false;

    switch (buf_cmd[10]) {
        case 0x02: // Device information
        case 0x30: // Player LEDs
        case 0x38: // HOME LED
        case 0x50: // Battery voltage
            return true;
        case 0x03: // Input report mode; 0x23 is used by MCU firmware update.
            return buf_cmd[11] == 0x30 || buf_cmd[11] == 0x31 || buf_cmd[11] == 0x3F;
        case 0x10: { // SPI read only
            u32 offset = *(const u32 *)&buf_cmd[11];
            return is_safe_spi_read_range(offset, buf_cmd[15]);
        }
        case 0x40: // IMU enable/disable
        case 0x48: // Rumble enable/disable
            return buf_cmd[11] <= 1;
        case 0x43: // Known temperature/IMU register reads only
            return (buf_cmd[11] == 0x10 && buf_cmd[12] == 0x01) ||
                (buf_cmd[11] == 0x20 && buf_cmd[12] == 0x02);
        default:
            return false;
    }
}

static bool command_tail_is_zero(const u8 *buf_cmd, int start) {
    for (int i = start; i < 49; ++i) {
        if (buf_cmd[i] != 0)
            return false;
    }
    return true;
}

static bool is_safe_internal_command(const u8 *buf_cmd) {
    if (buf_cmd[0] != 0x01)
        return false;
    if (buf_cmd[10] == 0x06)
        return (buf_cmd[11] == 0x00 || buf_cmd[11] == 0x02) && command_tail_is_zero(buf_cmd, 12);
    if (buf_cmd[10] == 0x07)
        return command_tail_is_zero(buf_cmd, 11);
    if (buf_cmd[10] == 0x08)
        return buf_cmd[11] <= 1 && command_tail_is_zero(buf_cmd, 12);
    return false;
}

static int send_custom_command_impl(const u8* arg, size_t arg_len, bool internal_command) {
    device_io_guard io_guard(current_device_context());
    if (!arg || arg_len < 6)
        return 1;
    int res_write;
    int res;
    int byte_seperator = 1;
    String^ input_report_cmd;
    String^ input_report_sys;
    String^ output_report_sys;
    u8 buf_cmd[49];
    u8 buf_reply[0x170];
    memset(buf_cmd, 0, sizeof(buf_cmd));
    memset(buf_reply, 0, sizeof(buf_reply));

    auto get_arg = [&](size_t index) -> u8 {
        return index < arg_len ? arg[index] : 0;
    };

    buf_cmd[0] = get_arg(0); // cmd
    buf_cmd[1] = timming_byte & 0xF;
    timming_byte++;
    // Vibration pattern
    buf_cmd[2] = buf_cmd[6] = get_arg(1);
    buf_cmd[3] = buf_cmd[7] = get_arg(2);
    buf_cmd[4] = buf_cmd[8] = get_arg(3);
    buf_cmd[5] = buf_cmd[9] = get_arg(4);

    buf_cmd[10] = get_arg(5); // subcmd

    output_report_sys = String::Format(L"Cmd:  {0:X2}   Subcmd: {1:X2}\r\n", buf_cmd[0], buf_cmd[10]);
    if (buf_cmd[0] == 0x01 || buf_cmd[0] == 0x10 || buf_cmd[0] == 0x11) {
        for (int i = 6; i < 44; i++) {
            buf_cmd[5 + i] = get_arg(i);
            output_report_sys += String::Format(L"{0:X2} ", buf_cmd[5 + i]);
            if (byte_seperator == 4)
                output_report_sys += L" ";
            if (byte_seperator == 8) {
                byte_seperator = 0;
                output_report_sys += L"\r\n";
            }
            byte_seperator++;
        }
    }
    //Use subcmd after command
    else {
        for (int i = 6; i < 44; i++) {
            buf_cmd[i - 5] = get_arg(i);
            output_report_sys += String::Format(L"{0:X2} ", buf_cmd[i - 5]);
            if (byte_seperator == 4)
                output_report_sys += L" ";
            if (byte_seperator == 8) {
                byte_seperator = 0;
                output_report_sys += L"\r\n";
            }
            byte_seperator++;
        }
    }
    // Subcommand 0x21 carries a CRC over bytes 12..47.
    if (buf_cmd[10] == 0x21)
        buf_cmd[48] = mcu_crc8_calc(buf_cmd + 12, 36);

    // Raw debug traffic is allowlisted. In particular, output report 0x03,
    // report 0x11 and subcommands 0x11/0x12/0x21/0x22 can reach flash or MCU
    // firmware-update state and must never be sent by this generic entry point.
    if (!is_safe_debug_command(buf_cmd) &&
        !(internal_command && is_safe_internal_command(buf_cmd))) {
        FormJoy::myform1->textBoxDbg_reply->Text =
            L"Blocked: this raw command is outside the hardware-safe diagnostic allowlist.";
        return 2;
    }
    FormJoy::myform1->textBoxDbg_sent->Text = output_report_sys;

    //Packet size header + subcommand and uint8 argument
    res_write = hid_write(handle, buf_cmd, sizeof(buf_cmd));

    if (res_write < (int)sizeof(buf_cmd)) {
        FormJoy::myform1->textBoxDbg_reply->Text = L"hid_write failed; no reply was accepted.";
        return 1;
    }
    int retries = 0;
    bool matched_reply = false;
    bool reply_accepted = false;
    while (1) {
        memset(buf_reply, 0, sizeof(buf_reply));
        res = hid_read_timeout(handle, buf_reply, sizeof(buf_reply), 64);

        if (res > 0) {
            if (get_arg(0) == 0x01 && buf_reply[0] == 0x21 && res > 14 &&
                buf_reply[14] == get_arg(5)) {
                matched_reply = true;
                reply_accepted = (buf_reply[13] & 0x80) != 0;
                break;
            }
        }

        retries++;
        if (retries == 20)
            break;
    }
    byte_seperator = 1;
    if (res > 12) {
        if (buf_reply[0] == 0x21 || buf_reply[0] == 0x30 || buf_reply[0] == 0x33 || buf_reply[0] == 0x31 || buf_reply[0] == 0x3F) {
            input_report_cmd += String::Format(L"\r\nInput report: 0x{0:X2}\r\n", buf_reply[0]);
            input_report_sys += String::Format(L"Subcmd Reply:\r\n", buf_reply[0]);
            int len = 49;
            if (buf_reply[0] == 0x33 || buf_reply[0] == 0x31)
                len = 362;
            for (int i = 1; i < 13; i++) {
                input_report_cmd += String::Format(L"{0:X2} ", buf_reply[i]);
                if (byte_seperator == 4)
                    input_report_cmd += L" ";
                if (byte_seperator == 8) {
                    byte_seperator = 0;
                    input_report_cmd += L"\r\n";
                }
                byte_seperator++;
            }
            byte_seperator = 1;
            for (int i = 13; i < len; i++) {
                input_report_sys += String::Format(L"{0:X2} ", buf_reply[i]);
                if (byte_seperator == 4)
                    input_report_sys += L" ";
                if (byte_seperator == 8) {
                    byte_seperator = 0;
                    input_report_sys += L"\r\n";
                }
                byte_seperator++;
            }
            int crc_check_ok = 0;
            if (get_arg(5) == 0x21) {
                crc_check_ok = (buf_reply[48] == mcu_crc8_calc(buf_reply + 0xF, 33));
                if (crc_check_ok)
                    input_report_sys += L"(CRC OK)";
                else
                    input_report_sys += L"(Wrong CRC)";
            }
        }
        else {
            input_report_sys += String::Format(L"ID: {0:X2} Subcmd reply:\r\n", buf_reply[0]);
            for (int i = 13; i < res; i++) {
                input_report_sys += String::Format(L"{0:X2} ", buf_reply[i]);
                if (byte_seperator == 4)
                    input_report_sys += L" ";
                if (byte_seperator == 8) {
                    byte_seperator = 0;
                    input_report_sys += L"\r\n";
                }
                byte_seperator++;
            }
        }
    }
    else if (res > 0 && res <= 12) {
        for (int i = 0; i < res; i++)
            input_report_sys += String::Format(L"{0:X2} ", buf_reply[i]);
    }
    else {
        input_report_sys += L"No reply";
    }
    FormJoy::myform1->textBoxDbg_reply->Text = input_report_sys;
    FormJoy::myform1->textBoxDbg_reply_cmd->Text = input_report_cmd;

    // Reboot/disconnect may close the link before returning its ACK, but an
    // explicit matching NACK must still be treated as failure.
    if (get_arg(5) == 0x06 && !matched_reply)
        return 0;
    return matched_reply && reply_accepted ? 0 : 1;
}

int send_custom_command(const u8* arg, size_t arg_len) {
    return send_custom_command_impl(arg, arg_len, false);
}

int send_internal_command(const u8* arg, size_t arg_len) {
    return send_custom_command_impl(arg, arg_len, true);
}


int button_test() {
    device_io_guard io_guard(current_device_context());
    int res;
    int limit_output = 0;
    int session_slot = current_device_context()->slot;
    auto set_session_text = [&](String^ control, String^ value, bool append) {
        FormJoy::myform1->setSessionControlText(session_slot, control, value, append);
    };
    String^ input_report_cmd;
    String^ input_report_sys;
    u8 buf_cmd[49];
    u8 buf_reply[0x170];
    float acc_cal_coeff[3];
    float gyro_cal_coeff[3];
    float cal_x[1] = { 0.0f };
    float cal_y[1] = { 0.0f };

    bool has_user_cal_stick_l = false;
    bool has_user_cal_stick_r = false;
    bool has_user_cal_sensor = false;

    u8 factory_stick_cal[0x12];
    u8 user_stick_cal[0x16];
    u8 sensor_model[0x6];
    u8 stick_model[0x24];
    u8 factory_sensor_cal[0x18];
    u8 user_sensor_cal[0x1A];
    s16 sensor_cal[0x2][0x3];
    u16 stick_cal_x_l[0x3];
    u16 stick_cal_y_l[0x3];
    u16 stick_cal_x_r[0x3];
    u16 stick_cal_y_r[0x3];
    memset(factory_stick_cal, 0, 0x12);
    memset(user_stick_cal, 0, 0x16);
    memset(sensor_model, 0, 0x6);
    memset(stick_model, 0, 0x12);
    memset(factory_sensor_cal, 0, 0x18);
    memset(user_sensor_cal, 0, 0x1A);
    memset(sensor_cal, 0, sizeof(sensor_cal));
    memset(stick_cal_x_l, 0, sizeof(stick_cal_x_l));
    memset(stick_cal_y_l, 0, sizeof(stick_cal_y_l));
    memset(stick_cal_x_r, 0, sizeof(stick_cal_x_r));
    memset(stick_cal_y_r, 0, sizeof(stick_cal_y_r));
    get_spi_data(0x6020, 0x18, factory_sensor_cal);
    get_spi_data(0x603D, 0x12, factory_stick_cal);
    get_spi_data(0x6080, 0x6, sensor_model);
    get_spi_data(0x6086, 0x12, stick_model);
    get_spi_data(0x6098, 0x12, &stick_model[0x12]);
    get_spi_data(0x8010, 0x16, user_stick_cal);
    get_spi_data(0x8026, 0x1A, user_sensor_cal);

    // Analog Stick device parameters
    set_session_text(L"txtBox_devParameters", String::Format(L"Flat surface ACC Offset:\r\n{0:X4} {1:X4} {2:X4}\r\n\r\n\r\nStick Parameters:\r\n{3:X3} {4:X3}\r\n{5:X2} (Deadzone)\r\n{6:X3} (Range ratio)",
        sensor_model[0] | sensor_model[1] << 8,
        sensor_model[2] | sensor_model[3] << 8,
        sensor_model[4] | sensor_model[5] << 8,
        (stick_model[1] << 8) & 0xF00 | stick_model[0], (stick_model[2] << 4) | (stick_model[1] >> 4),
        (stick_model[4] << 8) & 0xF00 | stick_model[3],
        ((stick_model[5] << 4) | (stick_model[4] >> 4))), false);

    for (int i = 0; i < 10; i = i + 3) {
        set_session_text(L"txtBox_devParameters", String::Format(L"\r\n{0:X3} {1:X3}",
            (stick_model[7 + i] << 8) & 0xF00 | stick_model[6 + i],
            (stick_model[8 + i] << 4) | (stick_model[7 + i] >> 4)), true);
    }

    set_session_text(L"txtBox_devParameters2", String::Format(L"Stick Parameters 2:\r\n{0:X3} {1:X3}\r\n{2:X2} (Deadzone)\r\n{3:X3} (Range ratio)",
        (stick_model[19] << 8) & 0xF00 | stick_model[18], (stick_model[20] << 4) | (stick_model[19] >> 4),
        (stick_model[22] << 8) & 0xF00 | stick_model[21],
        ((stick_model[23] << 4) | (stick_model[22] >> 4))), false);

    for (int i = 0; i < 10; i = i + 3) {
        set_session_text(L"txtBox_devParameters2", String::Format(L"\r\n{0:X3} {1:X3}",
            (stick_model[25 + i] << 8) & 0xF00 | stick_model[24 + i],
            (stick_model[26 + i] << 4) | (stick_model[25 + i] >> 4)), true);
    }

    // Stick calibration
    if (handle_ok != 2) {
        stick_cal_x_l[1] = (factory_stick_cal[4] << 8) & 0xF00 | factory_stick_cal[3];
        stick_cal_y_l[1] = (factory_stick_cal[5] << 4) | (factory_stick_cal[4] >> 4);
        stick_cal_x_l[0] = stick_cal_x_l[1] - ((factory_stick_cal[7] << 8) & 0xF00 | factory_stick_cal[6]);
        stick_cal_y_l[0] = stick_cal_y_l[1] - ((factory_stick_cal[8] << 4) | (factory_stick_cal[7] >> 4));
        stick_cal_x_l[2] = stick_cal_x_l[1] + ((factory_stick_cal[1] << 8) & 0xF00 | factory_stick_cal[0]);
        stick_cal_y_l[2] = stick_cal_y_l[1] + ((factory_stick_cal[2] << 4) | (factory_stick_cal[2] >> 4));
        set_session_text(L"textBox_lstick_fcal", String::Format(L"L Stick Factory:\r\nCenter X,Y: ({0:X3}, {1:X3})\r\nX: [{2:X3} - {4:X3}] Y: [{3:X3} - {5:X3}]",
            stick_cal_x_l[1], stick_cal_y_l[1], stick_cal_x_l[0], stick_cal_y_l[0], stick_cal_x_l[2], stick_cal_y_l[2]), false);
    }
    else {
        set_session_text(L"textBox_lstick_fcal", L"L Stick Factory:\r\nNo calibration", false);
    }
    if (handle_ok != 1) {
        stick_cal_x_r[1] = (factory_stick_cal[10] << 8) & 0xF00 | factory_stick_cal[9];
        stick_cal_y_r[1] = (factory_stick_cal[11] << 4) | (factory_stick_cal[10] >> 4);
        stick_cal_x_r[0] = stick_cal_x_r[1] - ((factory_stick_cal[13] << 8) & 0xF00 | factory_stick_cal[12]);
        stick_cal_y_r[0] = stick_cal_y_r[1] - ((factory_stick_cal[14] << 4) | (factory_stick_cal[13] >> 4));
        stick_cal_x_r[2] = stick_cal_x_r[1] + ((factory_stick_cal[16] << 8) & 0xF00 | factory_stick_cal[15]);
        stick_cal_y_r[2] = stick_cal_y_r[1] + ((factory_stick_cal[17] << 4) | (factory_stick_cal[16] >> 4));
        set_session_text(L"textBox_rstick_fcal", String::Format(L"R Stick Factory:\r\nCenter X,Y: ({0:X3}, {1:X3})\r\nX: [{2:X3} - {4:X3}] Y: [{3:X3} - {5:X3}]",
            stick_cal_x_r[1], stick_cal_y_r[1], stick_cal_x_r[0], stick_cal_y_r[0], stick_cal_x_r[2], stick_cal_y_r[2]), false);
    }
    else {
        set_session_text(L"textBox_rstick_fcal", L"R Stick Factory:\r\nNo calibration", false);
    }

    if ((user_stick_cal[0] | user_stick_cal[1] << 8) == 0xA1B2) {
        stick_cal_x_l[1] = (user_stick_cal[6] << 8) & 0xF00 | user_stick_cal[5];
        stick_cal_y_l[1] = (user_stick_cal[7] << 4) | (user_stick_cal[6] >> 4);
        stick_cal_x_l[0] = stick_cal_x_l[1] - ((user_stick_cal[9] << 8) & 0xF00 | user_stick_cal[8]);
        stick_cal_y_l[0] = stick_cal_y_l[1] - ((user_stick_cal[10] << 4) | (user_stick_cal[9] >> 4));
        stick_cal_x_l[2] = stick_cal_x_l[1] + ((user_stick_cal[3] << 8) & 0xF00 | user_stick_cal[2]);
        stick_cal_y_l[2] = stick_cal_y_l[1] + ((user_stick_cal[4] << 4) | (user_stick_cal[3] >> 4));
        set_session_text(L"textBox_lstick_ucal", String::Format(L"L Stick User:\r\nCenter X,Y: ({0:X3}, {1:X3})\r\nX: [{2:X3} - {4:X3}] Y: [{3:X3} - {5:X3}]",
            stick_cal_x_l[1], stick_cal_y_l[1], stick_cal_x_l[0], stick_cal_y_l[0], stick_cal_x_l[2], stick_cal_y_l[2]), false);
    }
    else {
        set_session_text(L"textBox_lstick_ucal", L"L Stick User:\r\nNo calibration", false);
    }
    if ((user_stick_cal[0xB] | user_stick_cal[0xC] << 8) == 0xA1B2) {
        stick_cal_x_r[1] = (user_stick_cal[14] << 8) & 0xF00 | user_stick_cal[13];
        stick_cal_y_r[1] = (user_stick_cal[15] << 4) | (user_stick_cal[14] >> 4);
        stick_cal_x_r[0] = stick_cal_x_r[1] - ((user_stick_cal[17] << 8) & 0xF00 | user_stick_cal[16]);
        stick_cal_y_r[0] = stick_cal_y_r[1] - ((user_stick_cal[18] << 4) | (user_stick_cal[17] >> 4));
        stick_cal_x_r[2] = stick_cal_x_r[1] + ((user_stick_cal[20] << 8) & 0xF00 | user_stick_cal[19]);
        stick_cal_y_r[2] = stick_cal_y_r[1] + ((user_stick_cal[21] << 4) | (user_stick_cal[20] >> 4));
        set_session_text(L"textBox_rstick_ucal", String::Format(L"R Stick User:\r\nCenter X,Y: ({0:X3}, {1:X3})\r\nX: [{2:X3} - {4:X3}] Y: [{3:X3} - {5:X3}]",
            stick_cal_x_r[1], stick_cal_y_r[1], stick_cal_x_r[0], stick_cal_y_r[0], stick_cal_x_r[2], stick_cal_y_r[2]), false);
    }
    else {
        set_session_text(L"textBox_rstick_ucal", L"R Stick User:\r\nNo calibration", false);
    }

    // Sensor calibration
    set_session_text(L"textBox_6axis_cal", L"6-Axis Factory (XYZ):\r\nAcc:  ", false);
    for (int i = 0; i < 0xC; i = i + 6) {
        set_session_text(L"textBox_6axis_cal", String::Format(L"{0:X4} {1:X4} {2:X4}\r\n      ",
            factory_sensor_cal[i + 0] | factory_sensor_cal[i + 1] << 8,
            factory_sensor_cal[i + 2] | factory_sensor_cal[i + 3] << 8,
            factory_sensor_cal[i + 4] | factory_sensor_cal[i + 5] << 8), true);
    }
    // Acc cal origin position
    sensor_cal[0][0] = uint16_to_int16(factory_sensor_cal[0] | factory_sensor_cal[1] << 8);
    sensor_cal[0][1] = uint16_to_int16(factory_sensor_cal[2] | factory_sensor_cal[3] << 8);
    sensor_cal[0][2] = uint16_to_int16(factory_sensor_cal[4] | factory_sensor_cal[5] << 8);

    set_session_text(L"textBox_6axis_cal", L"\r\nGyro: ", true);
    for (int i = 0xC; i < 0x18; i = i + 6) {
        set_session_text(L"textBox_6axis_cal", String::Format(L"{0:X4} {1:X4} {2:X4}\r\n      ",
            factory_sensor_cal[i + 0] | factory_sensor_cal[i + 1] << 8,
            factory_sensor_cal[i + 2] | factory_sensor_cal[i + 3] << 8,
            factory_sensor_cal[i + 4] | factory_sensor_cal[i + 5] << 8), true);
    }
    // Gyro cal origin position
    sensor_cal[1][0] = uint16_to_int16(factory_sensor_cal[0xC] | factory_sensor_cal[0xD] << 8);
    sensor_cal[1][1] = uint16_to_int16(factory_sensor_cal[0xE] | factory_sensor_cal[0xF] << 8);
    sensor_cal[1][2] = uint16_to_int16(factory_sensor_cal[0x10] | factory_sensor_cal[0x11] << 8);

    if ((user_sensor_cal[0x0] | user_sensor_cal[0x1] << 8) == 0xA1B2) {
        set_session_text(L"textBox_6axis_ucal", L"6-Axis User (XYZ):\r\nAcc:  ", false);
        for (int i = 0; i < 0xC; i = i + 6) {
            set_session_text(L"textBox_6axis_ucal", String::Format(L"{0:X4} {1:X4} {2:X4}\r\n      ",
                user_sensor_cal[i + 2] | user_sensor_cal[i + 3] << 8,
                user_sensor_cal[i + 4] | user_sensor_cal[i + 5] << 8,
                user_sensor_cal[i + 6] | user_sensor_cal[i + 7] << 8), true);
        }
        // Acc cal origin position
        sensor_cal[0][0] = uint16_to_int16(user_sensor_cal[2] | user_sensor_cal[3] << 8);
        sensor_cal[0][1] = uint16_to_int16(user_sensor_cal[4] | user_sensor_cal[5] << 8);
        sensor_cal[0][2] = uint16_to_int16(user_sensor_cal[6] | user_sensor_cal[7] << 8);
        set_session_text(L"textBox_6axis_ucal", L"\r\nGyro: ", true);
        for (int i = 0xC; i < 0x18; i = i + 6) {
            set_session_text(L"textBox_6axis_ucal", String::Format(L"{0:X4} {1:X4} {2:X4}\r\n      ",
                user_sensor_cal[i + 2] | user_sensor_cal[i + 3] << 8,
                user_sensor_cal[i + 4] | user_sensor_cal[i + 5] << 8,
                user_sensor_cal[i + 6] | user_sensor_cal[i + 7] << 8), true);
        }
        // Gyro cal origin position
        sensor_cal[1][0] = uint16_to_int16(user_sensor_cal[0xE] | user_sensor_cal[0xF] << 8);
        sensor_cal[1][1] = uint16_to_int16(user_sensor_cal[0x10] | user_sensor_cal[0x11] << 8);
        sensor_cal[1][2] = uint16_to_int16(user_sensor_cal[0x12] | user_sensor_cal[0x13] << 8);
    }
    else {
        set_session_text(L"textBox_6axis_ucal", L"\r\n\r\nUser:\r\nNo calibration", false);
    }


    // Enable nxpad standard input report
    memset(buf_cmd, 0, sizeof(buf_cmd));
    auto hdr = (brcm_hdr *)buf_cmd;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x03;
    pkt->subcmd_arg.arg1 = 0x30;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read_timeout(handle, buf_cmd, 0, 120);

    // Enable IMU
    memset(buf_cmd, 0, sizeof(buf_cmd));
    hdr = (brcm_hdr *)buf_cmd;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x40;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read_timeout(handle, buf_cmd, 0, 120);

    // Use SPI calibration and convert them to SI acc unit (m/s^2)
    acc_cal_coeff[0] = (float)(1.0 / (float)(16384 - uint16_to_int16(sensor_cal[0][0]))) * 4.0f  * 9.8f;
    acc_cal_coeff[1] = (float)(1.0 / (float)(16384 - uint16_to_int16(sensor_cal[0][1]))) * 4.0f  * 9.8f;
    acc_cal_coeff[2] = (float)(1.0 / (float)(16384 - uint16_to_int16(sensor_cal[0][2]))) * 4.0f  * 9.8f;

    // Use SPI calibration and convert them to SI gyro unit (rad/s)
    gyro_cal_coeff[0] = (float)(936.0 / (float)(13371 - uint16_to_int16(sensor_cal[1][0])) * 0.01745329251994);
    gyro_cal_coeff[1] = (float)(936.0 / (float)(13371 - uint16_to_int16(sensor_cal[1][1])) * 0.01745329251994);
    gyro_cal_coeff[2] = (float)(936.0 / (float)(13371 - uint16_to_int16(sensor_cal[1][2])) * 0.01745329251994);

    // Input report loop
    while (enable_button_test) {
        res = hid_read_timeout(handle, buf_reply, sizeof(buf_reply), 200);

        if (res > 12) {
            if (buf_reply[0] == 0x21 || buf_reply[0] == 0x30 || buf_reply[0] == 0x31 || buf_reply[0] == 0x32 || buf_reply[0] == 0x33) {
                if (((buf_reply[2] >> 1) & 0x3) == 3)
                    input_report_cmd = String::Format(L"Conn: BT");
                else if (((buf_reply[2] >> 1) & 0x3) == 0)
                    input_report_cmd = String::Format(L"Conn: USB");
                else
                    input_report_cmd = String::Format(L"Conn: {0:X}?", (buf_reply[2] >> 1) & 0x3);
                input_report_cmd += String::Format(L"\r\nBatt: {0:X}/4   ", buf_reply[2] >> 5);
                if ((buf_reply[2] >> 4) & 0x1)
                    input_report_cmd += L"Charging: Yes\r\n";
                else
                    input_report_cmd += L"Charging: No\r\n";

                input_report_cmd += String::Format(L"Vibration decision: ");
                input_report_cmd += String::Format(L"{0:X}, {1:X}\r\n", (buf_reply[12] >> 7) & 1, (buf_reply[12] >> 4) & 7);

                input_report_cmd += String::Format(L"\r\nButtons: ");
                for (int i = 3; i < 6; i++)
                    input_report_cmd += String::Format(L"{0:X2} ", buf_reply[i]);
            
                if (handle_ok != 2) {
                    input_report_cmd += String::Format(L"\r\n\r\nL Stick (Raw/Cal):\r\nX:   {0:X3}   Y:   {1:X3}\r\n",
                        buf_reply[6] | (u16)((buf_reply[7] & 0xF) << 8),
                        (buf_reply[7] >> 4) | (buf_reply[8] << 4));

                    AnalogStickCalc(
                        cal_x, cal_y,
                        buf_reply[6] | (u16)((buf_reply[7] & 0xF) << 8),
                        (buf_reply[7] >> 4) | (buf_reply[8] << 4),
                        stick_cal_x_l,
                        stick_cal_y_l);

                    input_report_cmd += String::Format(L"X: {0,5:f2}   Y: {1,5:f2}\r\n",
                        cal_x[0], cal_y[0]);
                }
                if (handle_ok != 1) {
                    input_report_cmd += String::Format(L"\r\n\r\nR Stick (Raw/Cal):\r\nX:   {0:X3}   Y:   {1:X3}\r\n",
                        buf_reply[9] | (u16)((buf_reply[10] & 0xF) << 8),
                        (buf_reply[10] >> 4) | (buf_reply[11] << 4));

                    AnalogStickCalc(
                        cal_x, cal_y,
                        buf_reply[9] | (u16)((buf_reply[10] & 0xF) << 8),
                        (buf_reply[10] >> 4) | (buf_reply[11] << 4),
                        stick_cal_x_r,
                        stick_cal_y_r);

                    input_report_cmd += String::Format(L"X: {0,5:f2}   Y: {1,5:f2}\r\n",
                        cal_x[0], cal_y[0]);
                }

                input_report_sys = String::Format(L"Acc/meter (Raw/Cal):\r\n");
                //The controller sends the sensor data 3 times with a little bit different values. Skip them
                input_report_sys += String::Format(L"X: {0:X4}  {1,7:F2} m/s\u00B2\r\n", buf_reply[13] | (buf_reply[14] << 8) & 0xFF00,
                    (float)(uint16_to_int16(buf_reply[13] | (buf_reply[14] << 8) & 0xFF00)) * acc_cal_coeff[0]);
                input_report_sys += String::Format(L"Y: {0:X4}  {1,7:F2} m/s\u00B2\r\n", buf_reply[15] | (buf_reply[16] << 8) & 0xFF00,
                    (float)(uint16_to_int16(buf_reply[15] | (buf_reply[16] << 8) & 0xFF00)) * acc_cal_coeff[1]);
                input_report_sys += String::Format(L"Z: {0:X4}  {1,7:F2} m/s\u00B2\r\n", buf_reply[17] | (buf_reply[18] << 8) & 0xFF00,
                    (float)(uint16_to_int16(buf_reply[17] | (buf_reply[18] << 8) & 0xFF00))  * acc_cal_coeff[2]);

                input_report_sys += String::Format(L"\r\nGyroscope (Raw/Cal):\r\n");
                input_report_sys += String::Format(L"X: {0:X4}  {1,7:F2} rad/s\r\n", buf_reply[19] | (buf_reply[20] << 8) & 0xFF00,
                    (float)(uint16_to_int16(buf_reply[19] | (buf_reply[20] << 8) & 0xFF00) - uint16_to_int16(sensor_cal[1][0])) * gyro_cal_coeff[0]);
                input_report_sys += String::Format(L"Y: {0:X4}  {1,7:F2} rad/s\r\n", buf_reply[21] | (buf_reply[22] << 8) & 0xFF00,
                    (float)(uint16_to_int16(buf_reply[21] | (buf_reply[22] << 8) & 0xFF00) - uint16_to_int16(sensor_cal[1][1])) * gyro_cal_coeff[1]);
                input_report_sys += String::Format(L"Z: {0:X4}  {1,7:F2} rad/s\r\n", buf_reply[23] | (buf_reply[24] << 8) & 0xFF00,
                    (float)(uint16_to_int16(buf_reply[23] | (buf_reply[24] << 8) & 0xFF00) - uint16_to_int16(sensor_cal[1][2])) * gyro_cal_coeff[2]);
            }
            else if (buf_reply[0] == 0x3F) {
                input_report_cmd = L"";
                for (int i = 0; i < 17; i++)
                    input_report_cmd += String::Format(L"{0:X2} ", buf_reply[i]);
            }

            if (limit_output == 1) {
                set_session_text(L"textBox_btn_test_reply", input_report_cmd, false);
                set_session_text(L"textBox_btn_test_subreply", input_report_sys, false);
            }
            //Only update every 75ms for better readability. No need for real time parsing.
            else if (limit_output > 4) {
                limit_output = 0;
            }
            limit_output++;
        }
    }

    memset(buf_cmd, 0, sizeof(buf_cmd));
    hdr = (brcm_hdr *)buf_cmd;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x03;
    pkt->subcmd_arg.arg1 = 0x3F;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read_timeout(handle, buf_cmd, 0, 64);

    memset(buf_cmd, 0, sizeof(buf_cmd));
    hdr = (brcm_hdr *)buf_cmd;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x40;
    pkt->subcmd_arg.arg1 = 0x00;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read_timeout(handle, buf_cmd, 0, 64);

    return 0;
}


int play_tune(int tune_no) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    u8 buf2[49];

    //Enable Vibration
    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x48;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf2, 0, 120);
    // This needs to be changed for new bigger tunes.
    u32 *tune = new u32[6000];
    memset(tune, 0, sizeof(tune));
    int tune_size = 0;
    switch (tune_no) {
        case 0:
            memcpy(tune, &tune_SMB, sizeof(tune_SMB));
            tune_size = sizeof(tune_SMB) / sizeof(u32);
            break;
        case 1:
            memcpy(tune, &tune_SMO_OK, sizeof(tune_SMO_OK));
            tune_size = sizeof(tune_SMO_OK) / sizeof(u32);
            break;
    }

    for (int i = 0; i < tune_size; i++) {
        Sleep(15);
        memset(buf, 0, sizeof(buf));
        hdr = (brcm_hdr *)buf;
        pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x10;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        hdr->rumble_l[0] = (tune[i] >> 24) & 0xFF;
        hdr->rumble_l[1] = (tune[i] >> 16) & 0xFF;
        hdr->rumble_l[2] = (tune[i] >> 8) & 0xFF;
        hdr->rumble_l[3] = tune[i] & 0xFF;
        memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
        res = hid_write(handle, buf, sizeof(buf));
        // Joy-con does not reply when Output Report is 0x10

    }

    // Disable vibration
    Sleep(15);
    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0x00;
    hdr->rumble_l[1] = 0x01;
    hdr->rumble_l[2] = 0x40;
    hdr->rumble_l[3] = 0x40;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    pkt->subcmd = 0x48;
    pkt->subcmd_arg.arg1 = 0x00;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x30;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    delete[] tune;

    return 0;
}


int play_hd_rumble_file(int file_type, u16 sample_rate, int samples, int loop_start, int loop_end,
    int loop_wait, int loop_times, const u8 *loaded_file, const u8 *converted_file) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    u8 buf2[49];
    device_context *device = current_device_context();
    unsigned long long completed_samples = 0;
    unsigned long long total_samples = samples;
    if (file_type == 3 || file_type == 4)
        total_samples = loop_start +
            (unsigned long long)(loop_end - loop_start) * (1 + loop_times) +
            (samples - loop_end);

    auto update_progress = [&]() {
        if (total_samples > 0)
            device->task_progress = (int)min(100ULL, (completed_samples * 100ULL) / total_samples);
    };
    auto interruptible_sleep = [&](int milliseconds) {
        int remaining = milliseconds;
        while (remaining > 0 && !device->cancel_requested) {
            int wait = min(remaining, 20);
            Sleep(wait);
            remaining -= wait;
        }
        return !device->cancel_requested;
    };

    //Enable Vibration
    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x48;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf2, 0, 120);

    if (file_type == 1 || file_type == 2) {
        for (int i = 0; i < samples * 4; i = i + 4) {
            if (!interruptible_sleep(sample_rate))
                goto stop_playback;
            memset(buf, 0, sizeof(buf));
            hdr = (brcm_hdr *)buf;
            pkt = (brcm_cmd_01 *)(hdr + 1);
            hdr->cmd = 0x10;
            hdr->timer = timming_byte & 0xF;
            timming_byte++;
            if (file_type == 1) {
                hdr->rumble_l[0] = loaded_file[0x0A + i];
                hdr->rumble_l[1] = loaded_file[0x0B + i];
                hdr->rumble_l[2] = loaded_file[0x0C + i];
                hdr->rumble_l[3] = loaded_file[0x0D + i];
            }
            //file_type is simple bnvib
            else {
                hdr->rumble_l[0] = converted_file[0x0C + i];
                hdr->rumble_l[1] = converted_file[0x0D + i];
                hdr->rumble_l[2] = converted_file[0x0E + i];
                hdr->rumble_l[3] = converted_file[0x0F + i];
            }
            memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));

            res = hid_write(handle, buf, sizeof(*hdr));
            completed_samples++;
            update_progress();
        }
    }
    else if (file_type == 3 || file_type == 4) {
        u8 vib_off = 0;
        if (file_type == 3)
            vib_off = 8;
        else if (file_type == 4)
            vib_off = 12;

        for (int i = 0; i < loop_start * 4; i = i + 4) {
            if (!interruptible_sleep(sample_rate))
                goto stop_playback;
            memset(buf, 0, sizeof(buf));
            hdr = (brcm_hdr *)buf;
            pkt = (brcm_cmd_01 *)(hdr + 1);
            hdr->cmd = 0x10;
            hdr->timer = timming_byte & 0xF;
            timming_byte++;
            
            hdr->rumble_l[0] = loaded_file[0x0C + vib_off + i];
            hdr->rumble_l[1] = loaded_file[0x0D + vib_off + i];
            hdr->rumble_l[2] = loaded_file[0x0E + vib_off + i];
            hdr->rumble_l[3] = loaded_file[0x0F + vib_off + i];
            memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
            
            res = hid_write(handle, buf, sizeof(*hdr));
            completed_samples++;
            update_progress();
        }
        for (int j = 0; j < 1 + loop_times; j++) {
            for (int i = loop_start * 4; i < loop_end * 4; i = i + 4) {
                if (!interruptible_sleep(sample_rate))
                    goto stop_playback;
                memset(buf, 0, sizeof(buf));
                hdr = (brcm_hdr *)buf;
                pkt = (brcm_cmd_01 *)(hdr + 1);
                hdr->cmd = 0x10;
                hdr->timer = timming_byte & 0xF;
                timming_byte++;
                
                hdr->rumble_l[0] = loaded_file[0x0C + vib_off + i];
                hdr->rumble_l[1] = loaded_file[0x0D + vib_off + i];
                hdr->rumble_l[2] = loaded_file[0x0E + vib_off + i];
                hdr->rumble_l[3] = loaded_file[0x0F + vib_off + i];
                memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));

                res = hid_write(handle, buf, sizeof(*hdr));
                completed_samples++;
                update_progress();
            }
            if (!interruptible_sleep(sample_rate))
                goto stop_playback;
            // Disable vibration
            memset(buf, 0, sizeof(buf));
            hdr = (brcm_hdr *)buf;
            pkt = (brcm_cmd_01 *)(hdr + 1);
            hdr->cmd = 0x10;
            hdr->timer = timming_byte & 0xF;
            timming_byte++;
            hdr->rumble_l[0] = 0x00;
            hdr->rumble_l[1] = 0x01;
            hdr->rumble_l[2] = 0x40;
            hdr->rumble_l[3] = 0x40;
            memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
            res = hid_write(handle, buf, sizeof(*hdr));
            if (!interruptible_sleep(loop_wait * sample_rate))
                goto stop_playback;
        }
        for (int i = loop_end * 4; i < samples * 4; i = i + 4) {
            if (!interruptible_sleep(sample_rate))
                goto stop_playback;
            memset(buf, 0, sizeof(buf));
            hdr = (brcm_hdr *)buf;
            pkt = (brcm_cmd_01 *)(hdr + 1);
            hdr->cmd = 0x10;
            hdr->timer = timming_byte & 0xF;
            timming_byte++;
            
            hdr->rumble_l[0] = loaded_file[0x0C + vib_off + i];
            hdr->rumble_l[1] = loaded_file[0x0D + vib_off + i];
            hdr->rumble_l[2] = loaded_file[0x0E + vib_off + i];
            hdr->rumble_l[3] = loaded_file[0x0F + vib_off + i];
            memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));

            res = hid_write(handle, buf, sizeof(*hdr));
            completed_samples++;
            update_progress();
        }
    }
stop_playback:
    interruptible_sleep(sample_rate);
    // Disable vibration
    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x10;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0x00;
    hdr->rumble_l[1] = 0x01;
    hdr->rumble_l[2] = 0x40;
    hdr->rumble_l[3] = 0x40;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    res = hid_write(handle, buf, sizeof(buf));

    Sleep(sample_rate + 120);
    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    hdr->rumble_l[0] = 0x00;
    hdr->rumble_l[1] = 0x01;
    hdr->rumble_l[2] = 0x40;
    hdr->rumble_l[3] = 0x40;
    memcpy(hdr->rumble_r, hdr->rumble_l, sizeof(hdr->rumble_l));
    pkt->subcmd = 0x48;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    memset(buf, 0, sizeof(buf));
    hdr = (brcm_hdr *)buf;
    pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x30;
    pkt->subcmd_arg.arg1 = 0x01;
    res = hid_write(handle, buf, sizeof(buf));
    res = hid_read_timeout(handle, buf, 0, 64);

    return device->cancel_requested ? 2 : 0;
}


int ir_sensor_auto_exposure(int white_pixels_percent) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    u16 new_exposure = 0;
    int old_exposure = current_device_context()->ir_exposure;

    // Calculate new exposure;
    if (white_pixels_percent == 0)
        old_exposure += 10;
    else if (white_pixels_percent > 5)
        old_exposure -= (white_pixels_percent / 4) * 20;

    old_exposure = CLAMP(old_exposure, 0, 600);
    current_device_context()->ir_exposure = old_exposure;
    new_exposure = old_exposure * 31200 / 1000;

    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x21;

    pkt->subcmd_21_23_04.mcu_cmd    = 0x23; // Write register cmd
    pkt->subcmd_21_23_04.mcu_subcmd = 0x04; // Write register to IR mode subcmd
    pkt->subcmd_21_23_04.no_of_reg  = 0x03; // Number of registers to write. Max 9.

    pkt->subcmd_21_23_04.reg1_addr = 0x3001; // R: 0x0130 - Set Exposure time LSByte
    pkt->subcmd_21_23_04.reg1_val  = new_exposure & 0xFF;
    pkt->subcmd_21_23_04.reg2_addr = 0x3101; // R: 0x0131 - Set Exposure time MSByte
    pkt->subcmd_21_23_04.reg2_val  = (new_exposure & 0xFF00) >> 8;
    pkt->subcmd_21_23_04.reg3_addr = 0x0700; // R: 0x0007 - Finalize config - Without this, the register changes do not have any effect.
    pkt->subcmd_21_23_04.reg3_val  = 0x01;

    buf[48] = mcu_crc8_calc(buf + 12, 36);
    res = hid_write(handle, buf, sizeof(buf));

    return res;
}


int get_raw_ir_image(u8 show_status) {
    device_io_guard io_guard(current_device_context());
    std::stringstream ir_status;
    int session_slot = current_device_context()->slot;
    auto set_session_text = [&](String^ control, String^ value, bool append) {
        FormJoy::myform1->setSessionControlText(session_slot, control, value, append);
    };

    int elapsed_time = 0;
    int elapsed_time2 = 0;
    System::Diagnostics::Stopwatch^ sw = System::Diagnostics::Stopwatch::StartNew();

    u8 buf[49];
    u8 buf_reply[0x170];
    u8 *buf_image = new u8[19 * 4096]; // 8bpp greyscale image.
	uint16_t bad_signal = 0;
    int error_reading = 0;
    float noise_level = 0.0f;
    int avg_intensity_percent = 0.0f;
    int previous_frag_no = 0;
    int got_frag_no = 0;
    int missed_packet_no = 0;
    bool missed_packet = false;
    int initialization = 2;
    int max_pixels = ((ir_max_frag_no < 218 ? ir_max_frag_no : 217) + 1) * 300;
    int white_pixels_percent = 0;

    memset(buf_image, 0, sizeof(buf_image));

    memset(buf, 0, sizeof(buf));
    memset(buf_reply, 0, sizeof(buf_reply));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x11;
    pkt->subcmd = 0x03;
    buf[48] = 0xFF;

    // First ack
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    buf[14] = 0x0;
    buf[47] = mcu_crc8_calc(buf + 11, 36);
    hid_write(handle, buf, sizeof(buf));

    // IR Read/ACK loop for fragmented data packets. 
    // It also avoids requesting missed data fragments, we just skip it to not complicate things.
    while (enable_IRVideoPhoto || initialization) {
        if (current_device_context()->pending_ir_config_update.exchange(false)) {
            ir_image_config pending_config;
            EnterCriticalSection(&current_device_context()->ir_config_lock);
            pending_config = current_device_context()->pending_ir_config;
            LeaveCriticalSection(&current_device_context()->ir_config_lock);
            ir_sensor_config_live(pending_config);
        }
        memset(buf_reply, 0, sizeof(buf_reply));
        hid_read_timeout(handle, buf_reply, sizeof(buf_reply), 200);

        //Check if new packet
        if (buf_reply[0] == 0x31 && buf_reply[49] == 0x03) {
            got_frag_no = buf_reply[52];
            if (got_frag_no == (previous_frag_no + 1) % (ir_max_frag_no + 1)) {
                
                previous_frag_no = got_frag_no;

                // ACK for fragment
                hdr->timer = timming_byte & 0xF;
                timming_byte++;
                buf[14] = previous_frag_no;
                buf[47] = mcu_crc8_calc(buf + 11, 36);
                hid_write(handle, buf, sizeof(buf));

                memcpy(buf_image + (300 * got_frag_no), buf_reply + 59, 300);

                // Auto exposure.
                // TODO: Fix placement, so it doesn't drop next fragment.
                if (enable_IRAutoExposure && initialization < 2 && got_frag_no == 0){
                    white_pixels_percent = (int)((*(u16*)&buf_reply[55] * 100) / max_pixels);
                    ir_sensor_auto_exposure(white_pixels_percent);
                }

                // Status percentage
                ir_status.str("");
                ir_status.clear();
                if (initialization < 2) {
                    if (show_status == 2)
                        ir_status << "Status: Streaming.. ";
                    else
                        ir_status << "Status: Receiving.. ";
                }
                else
                    ir_status << "Status: Initializing.. ";
                ir_status << std::setfill(' ') << std::setw(3);
                ir_status << std::fixed << std::setprecision(0) << (float)got_frag_no / (float)(ir_max_frag_no + 1) * 100.0f;
                ir_status << "% - ";
                current_device_context()->task_progress = ((got_frag_no + 1) * 100) / (ir_max_frag_no + 1);

                //debug
               // printf("%02X Frag: Copy\n", got_frag_no);

                set_session_text(L"lbl_IRStatus", gcnew String(ir_status.str().c_str()) +
                    (sw->ElapsedMilliseconds - elapsed_time).ToString() + "ms", false);
                elapsed_time = sw->ElapsedMilliseconds;

                // Check if final fragment. Draw the frame.
                if (got_frag_no == ir_max_frag_no) {
                    // Update Viewport
                    elapsed_time2 = sw->ElapsedMilliseconds - elapsed_time2;
                    FormJoy::myform1->setIRPictureWindow(buf_image, true);

                    //debug
                    //printf("%02X Frag: Draw -------\n", got_frag_no);

                    // Stats/IR header parsing
                    // buf_reply[53]: Average Intensity. 0-255 scale.
                    // buf_reply[54]: Unknown. Shows up only when EXFilter is enabled.
                    // *(u16*)&buf_reply[55]: White pixels (pixels with 255 value). Max 65535. Uint16 constraints, even though max is 76800.
                    // *(u16*)&buf_reply[57]: Pixels with ambient noise from external light sources (sun, lighter, IR remotes, etc). Cleaned by External Light Filter.
                    noise_level = (float)(*(u16*)&buf_reply[57]) / ((float)(*(u16*)&buf_reply[55]) + 1.0f);
                    white_pixels_percent = (int)((*(u16*)&buf_reply[55] * 100) / max_pixels);
                    avg_intensity_percent = (int)((buf_reply[53] * 100) / 255);
                    set_session_text(L"lbl_IRHelp", String::Format("Amb Noise: {0:f2},  Int: {1:D}%,  FPS: {2:D} ({3:D}ms)\nEXFilter: {4:D},  White Px: {5:D}%,  EXF Int: {6:D}",
                        noise_level, avg_intensity_percent, (int)(1000 / elapsed_time2), elapsed_time2,
                        *(u16*)&buf_reply[57], white_pixels_percent, buf_reply[54]), false);

                    elapsed_time2 = sw->ElapsedMilliseconds;

                    if (initialization)
                        initialization--;
                }
            }
            // Repeat/Missed fragment
            else if (got_frag_no  || previous_frag_no) {
                // Check if repeat ACK should be send. Avoid writing to image buffer.
                if (got_frag_no == previous_frag_no) {
                    //debug
                    //printf("%02X Frag: Repeat\n", got_frag_no);

                    // ACK for fragment
                    hdr->timer = timming_byte & 0xF;
                    timming_byte++;
                    buf[14] = got_frag_no;
                    buf[47] = mcu_crc8_calc(buf + 11, 36);
                    hid_write(handle, buf, sizeof(buf));

                    missed_packet = false;
                }
                // Check if missed fragment and request it.
                else if(missed_packet_no != got_frag_no && !missed_packet) {
                    if (ir_max_frag_no != 0x03) {
                        //debug
                        //printf("%02X Frag: Missed %02X, Prev: %02X, PrevM: %02X\n", got_frag_no, previous_frag_no + 1, previous_frag_no, missed_packet_no);

                        // Missed packet
                        hdr->timer = timming_byte & 0xF;
                        timming_byte++;
                        //Request for missed packet. You send what the next fragment number will be, instead of the actual missed packet.
                        buf[12] = 0x1;
                        buf[13] = previous_frag_no + 1;
                        buf[14] = 0;
                        buf[47] = mcu_crc8_calc(buf + 11, 36);
                        hid_write(handle, buf, sizeof(buf));

                        buf[12] = 0x00;
                        buf[13] = 0x00;

                        memcpy(buf_image + (300 * got_frag_no), buf_reply + 59, 300);

                        previous_frag_no = got_frag_no;
                        missed_packet_no = got_frag_no - 1;
                        missed_packet = true;
                    }
                    // Check if missed fragment and res is 30x40. Don't request it.
                    else {
                        //debug
                        //printf("%02X Frag: Missed but res is 30x40\n", got_frag_no);

                        // ACK for fragment
                        hdr->timer = timming_byte & 0xF;
                        timming_byte++;
                        buf[14] = got_frag_no;
                        buf[47] = mcu_crc8_calc(buf + 11, 36);
                        hid_write(handle, buf, sizeof(buf));

                        memcpy(buf_image + (300 * got_frag_no), buf_reply + 59, 300);

                        previous_frag_no = got_frag_no;
                    }
                }
                // Got the requested missed fragments.
                else if (missed_packet_no == got_frag_no){
                    //debug
                    //printf("%02X Frag: Got missed %02X\n", got_frag_no, missed_packet_no);

                    // ACK for fragment
                    hdr->timer = timming_byte & 0xF;
                    timming_byte++;
                    buf[14] = got_frag_no;
                    buf[47] = mcu_crc8_calc(buf + 11, 36);
                    hid_write(handle, buf, sizeof(buf));

                    memcpy(buf_image + (300 * got_frag_no), buf_reply + 59, 300);

                    previous_frag_no = got_frag_no;
                    missed_packet = false;
                }
                // Repeat of fragment that is not max fragment.
                else {
                    //debug
                    //printf("%02X Frag: RepeatWoot M:%02X\n", got_frag_no, missed_packet_no);

                    // ACK for fragment
                    hdr->timer = timming_byte & 0xF;
                    timming_byte++;
                    buf[14] = got_frag_no;
                    buf[47] = mcu_crc8_calc(buf + 11, 36);
                    hid_write(handle, buf, sizeof(buf));
                }
                
                // Status percentage
                ir_status.str("");
                ir_status.clear();
                if (initialization < 2) {
                    if (show_status == 2)
                        ir_status << "Status: Streaming.. ";
                    else
                        ir_status << "Status: Receiving.. ";
                }
                else
                    ir_status << "Status: Initializing.. ";
                ir_status << std::setfill(' ') << std::setw(3);
                ir_status << std::fixed << std::setprecision(0) << (float)got_frag_no / (float)(ir_max_frag_no + 1) * 100.0f;
                ir_status << "% - ";
                current_device_context()->task_progress = ((got_frag_no + 1) * 100) / (ir_max_frag_no + 1);

                set_session_text(L"lbl_IRStatus", gcnew String(ir_status.str().c_str()) +
                    (sw->ElapsedMilliseconds - elapsed_time).ToString() + "ms", false);
                elapsed_time = sw->ElapsedMilliseconds;
            }
            
            // Streaming start
            else {
                // ACK for fragment
                hdr->timer = timming_byte & 0xF;
                timming_byte++;
                buf[14] = got_frag_no;
                buf[47] = mcu_crc8_calc(buf + 11, 36);
                hid_write(handle, buf, sizeof(buf));

                memcpy(buf_image + (300 * got_frag_no), buf_reply + 59, 300);

                //debug
                //printf("%02X Frag: 0 %02X\n", buf_reply[52], previous_frag_no);

                set_session_text(L"lbl_IRStatus", (sw->ElapsedMilliseconds - elapsed_time).ToString() + "ms", false);
                elapsed_time = sw->ElapsedMilliseconds;

                previous_frag_no = 0;
            }

        }
        // Empty IR report. Send Ack again. Otherwise, it fallbacks to high latency mode (30ms per data fragment)
        else if (buf_reply[0] == 0x31) {
            // ACK for fragment
            hdr->timer = timming_byte & 0xF;
            timming_byte++;

            // Send ACK again or request missed frag
            if (buf_reply[49] == 0xFF) {
                buf[14] = previous_frag_no;
            }
            else if (buf_reply[49] == 0x00) {
                buf[12] = 0x1;
                buf[13] = previous_frag_no + 1;
                buf[14] = 0;
               // printf("%02X Mode: Missed next packet %02X\n", buf_reply[49], previous_frag_no + 1);
            }

            buf[47] = mcu_crc8_calc(buf + 11, 36);
            hid_write(handle, buf, sizeof(buf));

            buf[12] = 0x00;
            buf[13] = 0x00;
        }
    }
    
    delete[] buf_image;

    return 0;
}


int ir_sensor(ir_image_config &ir_cfg) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[0x170];
    static int output_buffer_length = 49;
    int error_reading = 0;
    int res_get = 0;
    // Set input report to x31
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x31;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x0380)
                goto step1;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 1;
            goto step10;
        }
    }

step1:
    // Enable MCU
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x22;
        pkt->subcmd_arg.arg1 = 0x1;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x2280)
                goto step2;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 2;
            goto step10;
        }
    }

step2:
    // Request MCU mode status
    error_reading = 0;
    while (1) { // Not necessary, but we keep to make sure the MCU is ready.
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x01;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                //if (buf[49] == 0x01 && buf[56] == 0x06) // MCU state is Initializing
                // *(u16*)buf[52]LE x04 in lower than 3.89fw, x05 in 3.89
                // *(u16*)buf[54]LE x12 in lower than 3.89fw, x18 in 3.89
                // buf[56]: mcu mode state
                if (buf[49] == 0x01 && buf[56] == 0x01) // MCU state is Standby
                    goto step3;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 3;
            goto step10;
        }
    }

step3:
    // Set MCU mode
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x21;
        
        pkt->subcmd_21_21.mcu_cmd    = 0x21; // Set MCU mode cmd
        pkt->subcmd_21_21.mcu_subcmd = 0x00; // Set MCU mode cmd
        pkt->subcmd_21_21.mcu_mode   = 0x05; // MCU mode - 1: Standby, 4: NFC, 5: IR, 6: Initializing/FW Update?

        buf[48] = mcu_crc8_calc(buf + 12, 36);
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x21) {
                // *(u16*)buf[18]LE x04 in lower than 3.89fw, x05 in 3.89
                // *(u16*)buf[20]LE x12 in lower than 3.89fw, x18 in 3.89
                // buf[56]: mcu mode state
                if (buf[15] == 0x01 && *(u32*)&buf[22] == 0x01) // Mcu mode is Standby
                    goto step4;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 4;
            goto step10;
        }
    }

step4:
    // Request MCU mode status
    error_reading = 0;
    while (1) { // Not necessary, but we keep to make sure the MCU mode changed.
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x01;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                // *(u16*)buf[52]LE x04 in lower than 3.89fw, x05 in 3.89
                // *(u16*)buf[54]LE x12 in lower than 3.89fw, x18 in 3.89
                if (buf[49] == 0x01 && buf[56] == 0x05) // Mcu mode is IR
                    goto step5;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 5;
            goto step10;
        }
    }

step5:
    // Set IR mode and number of packets for each data blob. Blob size is packets * 300 bytes.
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;

        pkt->subcmd = 0x21;
        pkt->subcmd_21_23_01.mcu_cmd     = 0x23;
        pkt->subcmd_21_23_01.mcu_subcmd  = 0x01; // Set IR mode cmd
        pkt->subcmd_21_23_01.mcu_ir_mode = 0x07; // IR mode - 2: No mode/Disable?, 3: Moment, 4: Dpd (Wii-style pointing), 6: Clustering,
                                                 // 7: Image transfer, 8-10: Hand analysis (Silhouette, Image, Silhouette/Image), 0,1/5/10+: Unknown
        pkt->subcmd_21_23_01.no_of_frags = ir_max_frag_no; // Set number of packets to output per buffer
        pkt->subcmd_21_23_01.mcu_major_v = 0x0500; // Set required IR MCU FW v5.18. Major 0x0005.
        pkt->subcmd_21_23_01.mcu_minor_v = 0x1800; // Set required IR MCU FW v5.18. Minor 0x0018.

        buf[48] = mcu_crc8_calc(buf + 12, 36);
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x21) {
                // Mode set Ack
                if (buf[15] == 0x0b)
                    goto step6;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 6;
            goto step10;
        }
    }

step6:
    // Request IR mode status
    error_reading = 0;
    while (0) { // Not necessary
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;

        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x02;

        buf[47] = mcu_crc8_calc(buf + 11, 36);
        buf[48] = 0xFF;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                // mode set to 7: Image transfer
                if (buf[49] == 0x13 && *(u16*)&buf[50] == 0x0700)
                    goto step7;
            }
            retries++;
            if (retries > 4 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 7;
            goto step10;
        }
    }

step7:
    // Write to registers for the selected IR mode
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x21;

        pkt->subcmd_21_23_04.mcu_cmd    = 0x23; // Write register cmd
        pkt->subcmd_21_23_04.mcu_subcmd = 0x04; // Write register to IR mode subcmd
        pkt->subcmd_21_23_04.no_of_reg  = 0x09; // Number of registers to write. Max 9.      

        pkt->subcmd_21_23_04.reg1_addr  = 0x2e00; // R: 0x002e - Set Resolution based on sensor binning and skipping
        pkt->subcmd_21_23_04.reg1_val   = ir_cfg.ir_res_reg;
        pkt->subcmd_21_23_04.reg2_addr  = 0x3001; // R: 0x0130 - Set Exposure time LSByte - (31200 * us /1000) & 0xFF - Max: 600us, Max encoded: 0x4920.
        pkt->subcmd_21_23_04.reg2_val   = ir_cfg.ir_exposure & 0xFF;
        pkt->subcmd_21_23_04.reg3_addr  = 0x3101; // R: 0x0131 - Set Exposure time MSByte - ((31200 * us /1000) & 0xFF00) >> 8
        pkt->subcmd_21_23_04.reg3_val   = (ir_cfg.ir_exposure & 0xFF00) >> 8;
        pkt->subcmd_21_23_04.reg4_addr  = 0x3201; // R: 0x0132 - Enable Max exposure Time - 0: Manual exposure, 1: Max exposure
        pkt->subcmd_21_23_04.reg4_val   = 0x00;
        pkt->subcmd_21_23_04.reg5_addr  = 0x1000; // R: 0x0010 - Set IR Leds groups state - Only 3 LSB usable
        pkt->subcmd_21_23_04.reg5_val   = ir_cfg.ir_leds;
        pkt->subcmd_21_23_04.reg6_addr  = 0x2e01; // R: 0x012e - Set digital gain LSB 4 bits of the value - 0-0xff
        pkt->subcmd_21_23_04.reg6_val   = (ir_cfg.ir_digital_gain & 0xF) << 4;
        pkt->subcmd_21_23_04.reg7_addr  = 0x2f01; // R: 0x012f - Set digital gain MSB 4 bits of the value - 0-0x7
        pkt->subcmd_21_23_04.reg7_val   = (ir_cfg.ir_digital_gain & 0xF0) >> 4;
        pkt->subcmd_21_23_04.reg8_addr  = 0x0e00; // R: 0x00e0 - External light filter - LS o bit0: Off/On, bit1: 0x/1x, bit2: ??, bit4,5: ??.
        pkt->subcmd_21_23_04.reg8_val   = ir_cfg.ir_ex_light_filter;
        pkt->subcmd_21_23_04.reg9_addr  = 0x4301; // R: 0x0143 - ExLF/White pixel stats threshold - 200: Default
        pkt->subcmd_21_23_04.reg9_val   = 0xc8;

        buf[48] = mcu_crc8_calc(buf + 12, 36);
        res = hid_write(handle, buf, output_buffer_length);

        // Request IR mode status, before waiting for the x21 ack
        memset(buf, 0, sizeof(buf));
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x02;
        buf[47] = mcu_crc8_calc(buf + 11, 36);
        buf[48] = 0xFF;
        res = hid_write(handle, buf, output_buffer_length);

        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x21) {
                // Registers for mode 7: Image transfer set
                if (buf[15] == 0x13 && *(u16*)&buf[16] == 0x0700)
                    goto step8;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 8;
            goto step10;
        }
    }

step8:
    // Write to registers for the selected IR mode
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x21;

        pkt->subcmd_21_23_04.mcu_cmd    = 0x23; // Write register cmd
        pkt->subcmd_21_23_04.mcu_subcmd = 0x04; // Write register to IR mode subcmd
        pkt->subcmd_21_23_04.no_of_reg  = 0x08; // Number of registers to write. Max 9.      

        pkt->subcmd_21_23_04.reg1_addr  = 0x1100; // R: 0x0011 - Leds 1/2 Intensity - Max 0x0F.
        pkt->subcmd_21_23_04.reg1_val   = (ir_cfg.ir_leds_intensity >> 8) & 0xFF;
        pkt->subcmd_21_23_04.reg2_addr  = 0x1200; // R: 0x0012 - Leds 3/4 Intensity - Max 0x10.
        pkt->subcmd_21_23_04.reg2_val   = ir_cfg.ir_leds_intensity & 0xFF;
        pkt->subcmd_21_23_04.reg3_addr  = 0x2d00; // R: 0x002d - Flip image - 0: Normal, 1: Vertically, 2: Horizontally, 3: Both 
        pkt->subcmd_21_23_04.reg3_val   = ir_cfg.ir_flip;
        pkt->subcmd_21_23_04.reg4_addr  = 0x6701; // R: 0x0167 - Enable De-noise smoothing algorithms - 0: Disable, 1: Enable.
        pkt->subcmd_21_23_04.reg4_val   = (ir_cfg.ir_denoise >> 16) & 0xFF;
        pkt->subcmd_21_23_04.reg5_addr  = 0x6801; // R: 0x0168 - Edge smoothing threshold - Max 0xFF, Default 0x23
        pkt->subcmd_21_23_04.reg5_val   = (ir_cfg.ir_denoise >> 8) & 0xFF;
        pkt->subcmd_21_23_04.reg6_addr  = 0x6901; // R: 0x0169 - Color Interpolation threshold - Max 0xFF, Default 0x44
        pkt->subcmd_21_23_04.reg6_val   = ir_cfg.ir_denoise & 0xFF;
        pkt->subcmd_21_23_04.reg7_addr  = 0x0400; // R: 0x0004 - LSB Buffer Update Time - Default 0x32
        if (ir_cfg.ir_res_reg == 0x69)
            pkt->subcmd_21_23_04.reg7_val = 0x2d; // A value of <= 0x2d is fast enough for 30 x 40, so the first fragment has the updated frame.  
        else
            pkt->subcmd_21_23_04.reg7_val = 0x32; // All the other resolutions the default is enough. Otherwise a lower value can break hand analysis.
        pkt->subcmd_21_23_04.reg8_addr  = 0x0700; // R: 0x0007 - Finalize config - Without this, the register changes do not have any effect.
        pkt->subcmd_21_23_04.reg8_val   = 0x01;

        buf[48] = mcu_crc8_calc(buf + 12, 36);
        res = hid_write(handle, buf, output_buffer_length);

        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x21) {
                // Registers for mode 7: Image transfer set
                if (buf[15] == 0x13 && *(u16*)&buf[16] == 0x0700)
                    goto step9;
                // If the Joy-Con gets to reply to the previous x11 - x03 02 cmd before sending the above,
                // it will reply with the following if we do not send x11 - x03 02 again:
                else if (buf[15] == 0x23) // Got mcu mode config write.
                    goto step9;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 9;
            goto step10;
        }
    }

step9:
    // Stream or Capture images from NIR Camera
    if (enable_IRVideoPhoto)
        res_get = get_raw_ir_image(2);
    else
        res_get = get_raw_ir_image(1);

    //////
    // TODO: Should we send subcmd x21 with 'x230102' to disable IR mode before disabling MCU?
step10:
    // Disable MCU
    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 1;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x22;
    pkt->subcmd_arg.arg1 = 0x00;
    res = hid_write(handle, buf, output_buffer_length);
    res = hid_read_timeout(handle, buf, sizeof(buf), 64);  


    // Set input report back to x3f
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x3f;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x0380)
                goto stepf;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            goto stepf;
        }
    }

stepf:
    return res_get;
}


int get_ir_registers(int start_reg, int reg_group) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[0x170];
    static int output_buffer_length = 49;
    int error_reading = 0;
    int res_get = 0;

    // Get the IR registers
    error_reading = 0;
    int pos_ir_registers = start_reg;
    while (1) {
    repeat_send:
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        memset(buf, 0, sizeof(buf));
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x03;

        buf[12] = 0x1; // seems to be always 0x01

        buf[13] = pos_ir_registers; // 0-4 registers page/group
        buf[14] = 0x00; // offset. this plus the number of registers, must be less than x7f
        buf[15] = 0x7f; // Number of registers to show + 1

        buf[47] = mcu_crc8_calc(buf + 11, 36);

        res = hid_write(handle, buf, output_buffer_length);

        int tries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[49] == 0x1b && buf[51] == pos_ir_registers && buf[52] == 0x00) {
                error_reading = 0;
                printf("--->%02X, %02X : %02X:\n", buf[51], buf[52], buf[53]);
                for (int i = 0; i <= buf[52] + buf[53]; i++)
                    if ((i & 0xF) == 0xF)
                        printf("%02X | ", buf[54 + i]);
                    else
                        printf("%02X ", buf[54 + i]);
                printf("\n");
                break;
            }
            tries++;
            if (tries > 8) {
                error_reading++;
                if (error_reading > 5) {
                    return 1;
                }
                goto repeat_send;
            }

        }
        pos_ir_registers++;
        if (pos_ir_registers > reg_group) {
            break;
        }
        
    }
    printf("\n");

    return 0;
}


int ir_sensor_config_live(ir_image_config &ir_cfg) {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];

    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 0x01;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x21;

    pkt->subcmd_21_23_04.mcu_cmd    = 0x23; // Write register cmd
    pkt->subcmd_21_23_04.mcu_subcmd = 0x04; // Write register to IR mode subcmd
    pkt->subcmd_21_23_04.no_of_reg  = 0x09; // Number of registers to write. Max 9.

    pkt->subcmd_21_23_04.reg1_addr = 0x3001; // R: 0x0130 - Set Exposure time LSByte
    pkt->subcmd_21_23_04.reg1_val  = ir_cfg.ir_exposure & 0xFF;
    pkt->subcmd_21_23_04.reg2_addr = 0x3101; // R: 0x0131 - Set Exposure time MSByte
    pkt->subcmd_21_23_04.reg2_val  = (ir_cfg.ir_exposure & 0xFF00) >> 8;
    pkt->subcmd_21_23_04.reg3_addr = 0x1000; // R: 0x0010 - Set IR Leds groups state
    pkt->subcmd_21_23_04.reg3_val  = ir_cfg.ir_leds;
    pkt->subcmd_21_23_04.reg4_addr = 0x2e01; // R: 0x012e - Set digital gain LSB 4 bits
    pkt->subcmd_21_23_04.reg4_val  = (ir_cfg.ir_digital_gain & 0xF) << 4;
    pkt->subcmd_21_23_04.reg5_addr = 0x2f01; // R: 0x012f - Set digital gain MSB 4 bits
    pkt->subcmd_21_23_04.reg5_val  = (ir_cfg.ir_digital_gain & 0xF0) >> 4;
    pkt->subcmd_21_23_04.reg6_addr = 0x0e00; // R: 0x00e0 - External light filter
    pkt->subcmd_21_23_04.reg6_val  = ir_cfg.ir_ex_light_filter;
    pkt->subcmd_21_23_04.reg7_addr = (ir_cfg.ir_custom_register & 0xFF) << 8 | (ir_cfg.ir_custom_register >> 8) & 0xFF;
    pkt->subcmd_21_23_04.reg7_val  = (ir_cfg.ir_custom_register >> 16) & 0xFF;
    pkt->subcmd_21_23_04.reg8_addr = 0x1100; // R: 0x0011 - Leds 1/2 Intensity - Max 0x0F.
    pkt->subcmd_21_23_04.reg8_val  = (ir_cfg.ir_leds_intensity >> 8) & 0xFF;
    pkt->subcmd_21_23_04.reg9_addr = 0x1200; // R: 0x0012 - Leds 3/4 Intensity - Max 0x10.
    pkt->subcmd_21_23_04.reg9_val  = ir_cfg.ir_leds_intensity & 0xFF;

    buf[48] = mcu_crc8_calc(buf + 12, 36);
    res = hid_write(handle, buf, sizeof(buf));

    // Important. Otherwise we gonna have a dropped packet.
    Sleep(15);

    pkt->subcmd_21_23_04.no_of_reg = 0x06; // Number of registers to write. Max 9.

    pkt->subcmd_21_23_04.reg1_addr = 0x2d00; // R: 0x002d - Flip image - 0: Normal, 1: Vertically, 2: Horizontally, 3: Both 
    pkt->subcmd_21_23_04.reg1_val  = ir_cfg.ir_flip;
    pkt->subcmd_21_23_04.reg2_addr = 0x6701; // R: 0x0167 - Enable De-noise smoothing algorithms - 0: Disable, 1: Enable.
    pkt->subcmd_21_23_04.reg2_val  = (ir_cfg.ir_denoise >> 16) & 0xFF;
    pkt->subcmd_21_23_04.reg3_addr = 0x6801; // R: 0x0168 - Edge smoothing threshold - Max 0xFF, Default 0x23
    pkt->subcmd_21_23_04.reg3_val  = (ir_cfg.ir_denoise >> 8) & 0xFF;
    pkt->subcmd_21_23_04.reg4_addr = 0x6901; // R: 0x0169 - Color Interpolation threshold - Max 0xFF, Default 0x44
    pkt->subcmd_21_23_04.reg4_val  = ir_cfg.ir_denoise & 0xFF;
    pkt->subcmd_21_23_04.reg5_addr = 0x0400; // R: 0x0004 - LSB Buffer Update Time - Default 0x32
    if (ir_cfg.ir_res_reg == 0x69)
        pkt->subcmd_21_23_04.reg5_val = 0x2d; // A value of <= 0x2d is fast enough for 30 x 40, so the first fragment has the updated frame.  
    else
        pkt->subcmd_21_23_04.reg5_val = 0x32; // All the other resolutions the default is enough. Otherwise a lower value can break hand analysis.
    pkt->subcmd_21_23_04.reg6_addr = 0x0700; // R: 0x0007 - Finalize config - Without this, the register changes do not have any effect.
    pkt->subcmd_21_23_04.reg6_val  = 0x01;

    buf[48] = mcu_crc8_calc(buf + 12, 36);
    res = hid_write(handle, buf, sizeof(buf));

    // get_ir_registers(0,4); // Get all register pages
    // get_ir_registers((ir_cfg.ir_custom_register >> 8) & 0xFF, (ir_cfg.ir_custom_register >> 8) & 0xFF); // Get all registers based on changed register's page

    return res;
}


int nfc_tag_info() {
    device_io_guard io_guard(current_device_context());
    /////////////////////////////////////////////////////
    // Kudos to Eric Betts (https://github.com/bettse) //
    // for nfc comm starters                           //
    /////////////////////////////////////////////////////
    int res;
    u8 buf[0x170];
    u8 buf2[0x170];
    static int output_buffer_length = 49;
    int error_reading = 0;
    int res_get = 0;
    u8  tag_uid_buf[10];
    u8  tag_uid_size = 0;
    u8  ntag_buffer[924];
    u16 ntag_buffer_pos = 0;
    u8  ntag_pages = 0; // Max 231
    u8  tag_type = 0;
    u16 payload_size = 0;
    bool ntag_init_done = false;
    int session_slot = current_device_context()->slot;
    auto set_session_text = [&](String^ control, String^ value, bool append) {
        FormJoy::myform1->setSessionControlText(session_slot, control, value, append);
    };

    memset(tag_uid_buf, 0, sizeof(tag_uid_buf));
    memset(ntag_buffer, 0, sizeof(ntag_buffer));

    // Set input report to x31
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x31;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x0380)
                goto step1;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 1;
            goto step9;
        }
    }

step1:
    // Enable MCU
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x22;
        pkt->subcmd_arg.arg1 = 0x1;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x2280)
                goto step2;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 2;
            goto step9;
        }
    }

step2:
    // Request MCU mode status
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x01;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                //if (buf[49] == 0x01 && buf[56] == 0x06) // MCU state is Initializing
                // *(u16*)buf[52]LE x04 in lower than 3.89fw, x05 in 3.89
                // *(u16*)buf[54]LE x12 in lower than 3.89fw, x18 in 3.89
                // buf[56]: mcu mode state
                if (buf[49] == 0x01 && buf[56] == 0x01) // Mcu mode is Standby
                    goto step3;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 3;
            goto step9;
        }
    }

step3:
    // Set MCU mode
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x01;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x21;

        pkt->subcmd_21_21.mcu_cmd = 0x21; // Set MCU mode cmd
        pkt->subcmd_21_21.mcu_subcmd = 0x00; // Set MCU mode cmd
        pkt->subcmd_21_21.mcu_mode = 0x04; // MCU mode - 1: Standby, 4: NFC, 5: IR, 6: Initializing/FW Update?

        buf[48] = mcu_crc8_calc(buf + 12, 36);
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x21) {
                // *(u16*)buf[18]LE x04 in lower than 3.89fw, x05 in 3.89
                // *(u16*)buf[20]LE x12 in lower than 3.89fw, x18 in 3.89
                if (buf[15] == 0x01 && buf[22] == 0x01) // Mcu mode is standby
                    goto step4;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 4;
            goto step9;
        }
    }

step4:
    // Request MCU mode status
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x01;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                // *(u16*)buf[52]LE x04 in lower than 3.89fw, x05 in 3.89
                // *(u16*)buf[54]LE x12 in lower than 3.89fw, x18 in 3.89
                if (buf[49] == 0x01 && buf[56] == 0x04) // Mcu mode is NFC
                    goto step5;
            }
            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            res_get = 5;
            goto step9;
        }
    }

step5:
    // Request NFC mode status
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;

        pkt->subcmd = 0x02;
        pkt->subcmd_arg.arg1 = 0x04; // 0: Cancel all, 4: StartWaitingReceive
        pkt->subcmd_arg.arg2 = 0x00; // Count of the currecnt packet if the cmd is a series of packets.
        buf[13] = 0x00;
        buf[14] = 0x08; // 8: Last cmd packet, 0: More cmd packet should be  expected
        buf[15] = 0x00; // Length of data after cmd header

        buf[47] = mcu_crc8_calc(buf + 11, 36); //Without the last byte
        res = hid_write(handle, buf, output_buffer_length - 1);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                if (buf[49] == 0x2a && *(u16*)&buf[50] == 0x0500 && buf[55] == 0x31 && buf[56] == 0x0b)// buf[56] == 0x0b: Initializing/Busy
                    break;
                if (buf[49] == 0x2a && *(u16*)&buf[50] == 0x0500 && buf[55] == 0x31 && buf[56] == 0x00) // buf[56] == 0x00: Awaiting cmd
                    goto step6;
            }
            retries++;
            if (retries > 4 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 9) {
            res_get = 6;
            goto step9;
        }
    }

step6:
    // Request NFC mode status
    error_reading = 0;
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;

        pkt->subcmd = 0x02;
        pkt->subcmd_arg.arg1 = 0x01; // 1: Start polling, 2: Stop polling, 
        pkt->subcmd_arg.arg2 = 0x00; // Count of the currecnt packet if the cmd is a series of packets.
        buf[13] = 0x00;
        buf[14] = 0x08; // 8: Last cmd packet, 0: More cmd packet should be expected
        buf[15] = 0x05; // Length of data after cmd header
        buf[16] = 0x01; // 1: Enable Mifare support
        buf[17] = 0x00; // Unknown.
        buf[18] = 0x00; // Unknown.
        buf[19] = 0x2c; // Unknown. Some values work (0x07) other don't.
        buf[20] = 0x01; // Unknown. This is not needed but Switch sends it.

        buf[47] = mcu_crc8_calc(buf + 11, 36); //Without the last byte
        res = hid_write(handle, buf, output_buffer_length - 1);
        int retries = 0;
        while (1) {
            if (!enable_NFCScanning)
                goto step7;
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (buf[0] == 0x31) {
                // buf[49] == 0x2a: NFC MCU input report
                // buf[50] shows when there's error?
                // buf[51] == 0x05: NFC
                // buf[54] always 9?
                // buf[55] always x31?
                // buf[56]: MCU/NFC state
                // buf[62]: nfc tag IC
                // buf[63]: nfc tag Type
                // buf[64]: size of following data and it's the last NFC header byte
                if (buf[49] == 0x2a && *(u16*)&buf[50] == 0x0500 && buf[56] == 0x09) { // buf[56] == 0x09: Tag detected
                    tag_uid_size = buf[64];
                    set_session_text(L"txtBox_nfcUid", L"UID:  ", false);
                    for (int i = 0; i < tag_uid_size; i++) {
                        if (i < tag_uid_size - 1) {
                            tag_type = buf[62]; // Save tag type
                            tag_uid_buf[i] = buf[65 + i]; // Save UID
                            set_session_text(L"txtBox_nfcUid", String::Format("{0:X2}:", buf[65 + i]), true);
                        }
                        else {
                            set_session_text(L"txtBox_nfcUid", String::Format("{0:X2}", buf[65 + i]), true);
                        }
                    }
                    set_session_text(L"txtBox_nfcUid", String::Format("\r\nType: {0:s}", buf[62] == 0x2 ? "NTAG" : "MIFARE"), true);
                    goto step7;
                }
                else if (buf[49] == 0x2a)
                    break;
            }
            retries++;
            if (retries > 4 || res == 0) {
                break;
            }
        }
        error_reading++;
        if (error_reading > 100) {
            res_get = 7;
            if (ntag_init_done)
                set_session_text(L"txtBox_NFCTag", String::Format("Tag lost!"), false);
            else
                set_session_text(L"txtBox_NFCTag", String::Format("No Tag detected!"), false);
            goto step9;
        }
    }

step7:
    // Read NTAG contents
    error_reading = 0;
    while (1) {
        memset(buf2, 0, sizeof(buf2));
        auto hdr = (brcm_hdr *)buf2;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 0x11;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;

        pkt->subcmd = 0x02;
        pkt->subcmd_arg.arg1 = 0x06; // 6: Read Ntag data, 0xf: Read mifare data
        buf2[12] = 0x00;
        buf2[13] = 0x00;
        buf2[14] = 0x08;
        buf2[15] = 0x13; // Length of data after cmd header

        buf2[16] = 0xd0; // Unknown
        buf2[17] = 0x07; // Unknown or UID lentgh?
        buf2[18] = 0x00; // Only for Mifare cmds or should have a UID?

        buf2[19] = 0x00; //should have a UID?
        buf2[20] = 0x00; //should have a UID?
        buf2[21] = 0x00; //should have a UID?
        buf2[22] = 0x00; //should have a UID?
        buf2[23] = 0x00; //should have a UID?
        buf2[24] = 0x00; //should have a UID?

        buf2[25] = 0x00; // 1: Ntag215 only. 0: All tags, otherwise error x48 (Invalid format error)

        // https://www.tagnfc.com/en/info/11-nfc-tags-specs

        // If the following is selected wrongly, error x3e (Read error)
        switch (ntag_pages) {
            case 0:
                buf2[26] = 0x01;
                break;
                // Ntag213
            case 45:
                // The following 7 bytes should be decided with max the current ntag pages and what we want to read.
                buf2[26] = 0x01; // How many blocks to read. Each block should be <= 60 pages (240 bytes)? Min/Max values are 1/4, otherwise error x40 (Argument error)

                buf2[27] = 0x00; // Block 1 starting page
                buf2[28] = 0x2C; // Block 1 ending page
                buf2[29] = 0x00; // Block 2 starting page
                buf2[30] = 0x00; // Block 2 ending page
                buf2[31] = 0x00; // Block 3 starting page
                buf2[32] = 0x00; // Block 3 ending page
                buf2[33] = 0x00; // Block 4 starting page
                buf2[34] = 0x00; // Block 4 ending page
                break;
                // Ntag215
            case 135:
                // The following 7 bytes should be decided with max the current ntag pages and what we want to read.
                buf2[26] = 0x03; // How many page ranges to read. Each range should be <= 60 pages (240 bytes)? Max value is 4.

                buf2[27] = 0x00; // Block 1 starting page
                buf2[28] = 0x3b; // Block 1 ending page
                buf2[29] = 0x3c; // Block 2 starting page
                buf2[30] = 0x77; // Block 2 ending page
                buf2[31] = 0x78; // Block 3 starting page
                buf2[32] = 0x86; // Block 3 ending page
                buf2[33] = 0x00; // Block 4 starting page
                buf2[34] = 0x00; // Block 4 ending page
                break;
            case 231:
                // The following 7 bytes should be decided with max the current ntag pages and what we want to read.
                buf2[26] = 0x04; // How many page ranges to read. Each range should be <= 60 pages (240 bytes)? Max value is 4.

                buf2[27] = 0x00; // Block 1 starting page
                buf2[28] = 0x3b; // Block 1 ending page
                buf2[29] = 0x3c; // Block 2 starting page
                buf2[30] = 0x77; // Block 2 ending page
                buf2[31] = 0x78; // Block 3 starting page
                buf2[32] = 0xB3; // Block 3 ending page
                buf2[33] = 0xB4; // Block 4 starting page
                buf2[34] = 0xE6; // Block 4 ending page
                break;
            default:
                break;
        }

        buf2[47] = mcu_crc8_calc(buf2 + 11, 36);

        res = hid_write(handle, buf2, output_buffer_length - 1);

        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf2, sizeof(buf2), 64);
            if (buf2[0] == 0x31) {
                if ((buf2[49] == 0x3a || buf2[49] == 0x2a) && buf2[56] == 0x07) {
                    set_session_text(L"txtBox_NFCTag", String::Format("Error {0:X2}!", buf2[50]), false);
                    goto step9;///////////
                }
                else if (buf2[49] == 0x3a && buf2[51] == 0x07) {
                    if (ntag_init_done) {
                        payload_size = (buf2[54] << 8 | buf2[55]) & 0x7FF;
                        if (buf2[52] == 0x01) {
                            memcpy(ntag_buffer + ntag_buffer_pos, buf2 + 116, payload_size - 60);
                            ntag_buffer_pos += payload_size - 60;
                        }
                        else {
                            memcpy(ntag_buffer + ntag_buffer_pos, buf2 + 56, payload_size);
                        }
                    }
                    else if (buf2[52] == 0x01) {
                        if (tag_type == 2) {
                            switch (buf2[74]) {
                                case 0:
                                    ntag_pages = 135;
                                    break;
                                case 3:
                                    ntag_pages = 45;
                                    break;
                                case 4:
                                    ntag_pages = 231;
                                    break;
                                default:
                                    goto step9;///////////
                                    break;
                            }
                        }
                    }
                    break;
                }
                else if (buf2[49] == 0x2a && buf2[56] == 0x04) { // finished
                    if (ntag_init_done) {
                        FormJoy::myform1->show_ntag_contents(session_slot, ntag_buffer, ntag_pages);
                        goto step9;///////////
                    }
                    ntag_init_done = true;

                    memset(buf, 0, sizeof(buf));
                    auto hdr = (brcm_hdr *)buf;
                    auto pkt = (brcm_cmd_01 *)(hdr + 1);
                    hdr->cmd = 0x11;
                    hdr->timer = timming_byte & 0xF;
                    timming_byte++;

                    pkt->subcmd = 0x02;
                    pkt->subcmd_arg.arg1 = 0x02; // 0: Cancel all, 4: StartWaitingReceive
                    pkt->subcmd_arg.arg2 = 0x00; // Count of the currecnt packet if the cmd is a series of packets.
                    buf[13] = 0x00;
                    buf[14] = 0x08; // 8: Last cmd packet, 0: More cmd packet should be  expected
                    buf[15] = 0x00; // Length of data after cmd header

                    buf[47] = mcu_crc8_calc(buf + 11, 36); //Without the last byte
                    res = hid_write(handle, buf, output_buffer_length - 1);
                    Sleep(200);
                    goto step5;
                }
                else if (buf2[49] == 0x2a)
                    break;
            }
            retries++;
            if (retries > 4 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 9) {
            res_get = 8;
            if (buf[62] == 0x4)
                set_session_text(L"txtBox_NFCTag", String::Format("Mifare reading is not supported for now.."), false);
            goto step9;
        }
    }

step9:
    // Disable MCU
    memset(buf, 0, sizeof(buf));
    auto hdr = (brcm_hdr *)buf;
    auto pkt = (brcm_cmd_01 *)(hdr + 1);
    hdr->cmd = 1;
    hdr->timer = timming_byte & 0xF;
    timming_byte++;
    pkt->subcmd = 0x22;
    pkt->subcmd_arg.arg1 = 0x00;
    res = hid_write(handle, buf, output_buffer_length);
    res = hid_read_timeout(handle, buf, sizeof(buf), 64);


    // Set input report to x3f
    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x3f;
        res = hid_write(handle, buf, output_buffer_length);
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x0380)
                goto stepf;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 7) {
            goto stepf;
        }
    }
stepf:
    if (res_get > 0)
        return res_get;

    return 0;
}


int silence_input_report() {
    device_io_guard io_guard(current_device_context());
    int res;
    u8 buf[49];
    int error_reading = 0;

    while (1) {
        memset(buf, 0, sizeof(buf));
        auto hdr = (brcm_hdr *)buf;
        auto pkt = (brcm_cmd_01 *)(hdr + 1);
        hdr->cmd = 1;
        hdr->timer = timming_byte & 0xF;
        timming_byte++;
        pkt->subcmd = 0x03;
        pkt->subcmd_arg.arg1 = 0x3f;
        res = hid_write(handle, buf, sizeof(buf));
        int retries = 0;
        while (1) {
            res = hid_read_timeout(handle, buf, sizeof(buf), 64);
            if (*(u16*)&buf[0xD] == 0x0380)
                goto stepf;

            retries++;
            if (retries > 8 || res == 0)
                break;
        }
        error_reading++;
        if (error_reading > 4)
            break;
    }

stepf:
    return 0;
}


/*
//usb test
int usb_init(hid_device *handle) {
    int res;
    u8 buf_cmd[2];
    u8 buf_cmd2[2];
    memset(buf_cmd, 0, sizeof(buf_cmd));
    memset(buf_cmd2, 0, sizeof(buf_cmd2));

    Sleep(16);
    buf_cmd[0] = 1;
    buf_cmd[0] = 0x80;
    Sleep(16);
    buf_cmd[1] = 0x01;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);
    Sleep(16);
    buf_cmd[1] = 0x02;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);
    Sleep(16);
    buf_cmd[1] = 0x03;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);
    Sleep(16);
    buf_cmd[1] = 0x02;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);
    Sleep(16);
    buf_cmd[1] = 0x04;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);

    return 0;
}

int usb_deinit(hid_device *handle) {
    int res;
    u8 buf_cmd[2];
    u8 buf_cmd2[2];
    memset(buf_cmd, 0, sizeof(buf_cmd));
    memset(buf_cmd2, 0, sizeof(buf_cmd2));

    Sleep(16);
    buf_cmd[0] = 0x80;
    buf_cmd[1] = 0x05;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);

    return 0;
}

int usb_command(hid_device *handle) {
    int res;
    u8 buf_cmd[20];
    u8 buf_cmd2[1];
    memset(buf_cmd, 0, sizeof(buf_cmd));
    memset(buf_cmd2, 0, sizeof(buf_cmd2));

    buf_cmd[0] = 0x80;
    buf_cmd[1] = 0x92;
    buf_cmd[3] = 0x31;
    buf_cmd[8] = 0x01;

    Sleep(16);

    buf_cmd[9] = timming_byte & 0xF;
    timming_byte++;
    buf_cmd[18] = 0x40;
    buf_cmd[19] = 0x01;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);
    Sleep(16);

    buf_cmd[9] = timming_byte & 0xF;
    timming_byte++;
    buf_cmd[18] = 0x30;
    buf_cmd[19] = 0x1;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);

    Sleep(16);

    buf_cmd[9] = timming_byte & 0xF;
    timming_byte++;
    buf_cmd[18] = 0x38;
    buf_cmd[19] = 0x22;
    buf_cmd[9] = timming_byte & 0xF;
    timming_byte++;
    res = hid_write(handle, buf_cmd, sizeof(buf_cmd));
    res = hid_read(handle, buf_cmd2, 0);

    return 0;
}

void usb_device_print(struct hid_device_info *dev)
{
    const wchar_t *interface_name = L"";

    if (wcscmp(dev->serial_number, L"000000000001"))
        interface_name = L"BT-HID";
    else
        interface_name = L"USB-HID";

    printf("USB device info:\n  VID:          0x%04hX\n" \
        "  PID:          0x%04hX\n  Dev Path:     %s\n" \
        "  MAC:          %ls\n  Interface:    %ls (%d)\n  Manufacturer: %ls\n" \
        "  Product:      %ls\n\n", dev->vendor_id, dev->product_id,
        dev->path, dev->serial_number, interface_name, dev->interface_number,
        dev->manufacturer_string, dev->product_string);
}
*/

int test_chamber() {

    //Add your testing code.

    return 0;
    }

int device_connection(){
    device_context *device = current_device_context();
    if (!device || !device->hid_handle || !device->connected)
        return 0;
    return device->device_type;
}

static int simulator_self_test() {
    hid_device_info *devices = hid_enumerate(0x057E, 0x0000);
    int simulated_count = 0;
    int result = 0;
    for (hid_device_info *info = devices; info; info = info->next) {
        if (!info->path || strncmp(info->path, "jctool-sim://", 13) != 0)
            continue;
        ++simulated_count;
        hid_device *test_handle = hid_open_path(info->path);
        if (!test_handle) {
            result = 10 + simulated_count;
            break;
        }

        unsigned char command[49] = {};
        unsigned char reply[64] = {};
        command[0] = 0x01;
        command[10] = 0x02;
        if (hid_write(test_handle, command, sizeof(command)) < 0 ||
            hid_read_timeout(test_handle, reply, sizeof(reply), 50) < 25 ||
            reply[0] != 0x21 || reply[13] != 0x82 || reply[14] != 0x02) {
            hid_close(test_handle);
            result = 20 + simulated_count;
            break;
        }

        memset(command, 0, sizeof(command));
        memset(reply, 0, sizeof(reply));
        command[0] = 0x01;
        command[10] = 0x10;
        *(u32 *)&command[11] = 0x6050;
        command[15] = 12;
        if (hid_write(test_handle, command, sizeof(command)) < 0 ||
            hid_read_timeout(test_handle, reply, sizeof(reply), 50) < 32 ||
            reply[13] != 0x90 || reply[14] != 0x10 || *(u32 *)&reply[15] != 0x6050) {
            hid_close(test_handle);
            result = 30 + simulated_count;
            break;
        }
        hid_close(test_handle);
    }
    hid_free_enumeration(devices);
    if (result != 0)
        return result;
    return simulated_count == 4 ? 0 : 40 + simulated_count;
}

static int simulator_safe_write_self_test() {
    refresh_device_sessions();
    int simulated_count = 0;
    int result = 0;
    for (int i = 0; i < 4; ++i) {
        device_context *device = get_device_slot(i);
        if (!device || device->path.find("jctool-sim://") != 0)
            continue;
        simulated_count++;
        set_thread_device_context(device);
        u8 colors[12];
        memset(colors, 0, sizeof(colors));
        if (get_spi_data(0x6050, sizeof(colors), colors) != 0 ||
            write_spi_data(0x6050, sizeof(colors), colors) != 0) {
            result = 50 + simulated_count;
            break;
        }
        u8 blocked_value = 0;
        if (write_spi_data(0x0100, 1, &blocked_value) == 0) {
            result = 60 + simulated_count;
            break;
        }
        if (simulated_count == 1) {
            char temp_path[MAX_PATH] = {};
            char temp_file[MAX_PATH] = {};
            if (GetTempPathA(MAX_PATH, temp_path) == 0 ||
                GetTempFileNameA(temp_path, "jct", 0, temp_file) == 0) {
                result = 65;
                break;
            }
            cancel_spi_dump = false;
            int dump_result = dump_spi(temp_file);
            FILE *verify_file = nullptr;
            __int64 file_size = -1;
            if (dump_result == 0 && fopen_s(&verify_file, temp_file, "rb") == 0) {
                if (_fseeki64(verify_file, 0, SEEK_END) == 0)
                    file_size = _ftelli64(verify_file);
                fclose(verify_file);
            }
            DeleteFileA(temp_file);
            if (dump_result != 0 || file_size != 0x80000) {
                result = 66;
                break;
            }
        }
    }
    close_device_sessions();
    if (result != 0)
        return result;
    if (simulated_count != 4)
        return 70 + simulated_count;

    u8 reply[49] = {};
    reply[0] = 0x21;
    reply[0xD] = 0x90;
    reply[0xE] = 0x10;
    *(u32 *)&reply[0xF] = 0x6050;
    reply[0x13] = 12;
    if (!is_valid_spi_read_reply(reply, 32, 0x6050, 12) ||
        is_valid_spi_read_reply(reply, 31, 0x6050, 12))
        return 80;

    u8 debug_command[49] = {};
    debug_command[0] = 0x03;
    if (is_safe_debug_command(debug_command))
        return 81;
    debug_command[0] = 0x01;
    debug_command[10] = 0x11;
    if (is_safe_debug_command(debug_command))
        return 82;
    debug_command[10] = 0x21;
    if (is_safe_debug_command(debug_command))
        return 83;
    debug_command[10] = 0x06;
    debug_command[11] = 0x02;
    if (is_safe_debug_command(debug_command) || !is_safe_internal_command(debug_command))
        return 84;
    debug_command[12] = 0x01;
    if (is_safe_internal_command(debug_command))
        return 85;
    memset(debug_command, 0, sizeof(debug_command));
    debug_command[0] = 0x01;
    debug_command[10] = 0x10;
    *(u32 *)&debug_command[11] = 0x6050;
    debug_command[15] = 12;
    return is_safe_debug_command(debug_command) ? 0 : 86;
}

[STAThread]
int Main(array<String^>^ args) {
    /*
    BOOL chk = AllocConsole();
    if (chk) {
        freopen("CONOUT$", "w", stdout);
    }
    */
    check_connection_ok = true;
    bool run_simulator_test = false;
    bool run_ui_cache_test = false;
    for (int i = 0; i < args->Length; ++i) {
        String^ argument = args[i];
        if (argument == "-d")
            enable_traffic_dump = true;
        else if (argument == "-f")
            check_connection_ok = false;
        else if (argument == "--sim-self-test") {
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATOR_CONFIG", nullptr);
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATORS", L"L,R,P,L");
            run_simulator_test = true;
        }
        else if (argument == "--ui-cache-self-test") {
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATOR_CONFIG", nullptr);
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATORS", L"L,R,P,L");
            run_ui_cache_test = true;
        }
        else if (argument == "--simulate") {
            String^ profile = L"L,R,L,R";
            if (i + 1 < args->Length && !args[i + 1]->StartsWith(L"-"))
                profile = args[++i];
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATOR_CONFIG", nullptr);
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATORS", profile);
        }
        else if (argument->StartsWith(L"--simulate=")) {
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATOR_CONFIG", nullptr);
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATORS", argument->Substring(11));
        }
        else if (argument == "--simulate-config" && i + 1 < args->Length) {
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATORS", nullptr);
            Environment::SetEnvironmentVariable(L"JCTOOL_SIMULATOR_CONFIG", args[++i]);
        }
    }

    if (run_simulator_test) {
        int result = simulator_self_test();
        return result == 0 ? simulator_safe_write_self_test() : result;
    }

    refresh_device_sessions();
    if (current_device_context())
        timming_byte = 0x0;

    //test_chamber();

    Application::EnableVisualStyles();
    Application::SetCompatibleTextRenderingDefault(false);

    CppWinFormJoy::FormJoy^  myform1 = gcnew FormJoy();

    if (run_ui_cache_test) {
        int result = myform1->runGlobalCacheSelfTest() ? 0 : 50;
        delete myform1;
        close_device_sessions();
        return result;
    }

    /*
    //usb test
    usb_init(handle);
    usb_init(handle_l);
    usb_command(handle);
    usb_command(handle);
    Sleep(2000);
    if (handle_ok) {
        usb_deinit(handle);
        hid_close(handle);
        usb_deinit(handle_l);
        hid_close(handle_l);
    }
    hid_exit();
    */

    Application::Run(myform1);

    close_device_sessions();

    return 0;
}
