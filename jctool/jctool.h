#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <Windows.h>

#include "hidapi.h"

using namespace System;

template <typename T> T CLAMP(const T& value, const T& low, const T& high)
{
    return value < low ? low : (value > high ? high : value);
}

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#pragma pack(push, 1)

struct brcm_hdr {
    u8 cmd;
    u8 timer;
    u8 rumble_l[4];
    u8 rumble_r[4];
};

struct brcm_cmd_01 {
    u8 subcmd;
    union {
        struct {
            u32 offset;
            u8 size;
        } spi_data;

        struct {
            u8 arg1;
            u8 arg2;
        } subcmd_arg;

        struct {
            u8  mcu_cmd;
            u8  mcu_subcmd;
            u8  mcu_mode;
        } subcmd_21_21;

        struct {
            u8  mcu_cmd;
            u8  mcu_subcmd;
            u8  no_of_reg;
            u16 reg1_addr;
            u8  reg1_val;
            u16 reg2_addr;
            u8  reg2_val;
            u16 reg3_addr;
            u8  reg3_val;
            u16 reg4_addr;
            u8  reg4_val;
            u16 reg5_addr;
            u8  reg5_val;
            u16 reg6_addr;
            u8  reg6_val;
            u16 reg7_addr;
            u8  reg7_val;
            u16 reg8_addr;
            u8  reg8_val;
            u16 reg9_addr;
            u8  reg9_val;
        } subcmd_21_23_04;

        struct {
            u8  mcu_cmd;
            u8  mcu_subcmd;
            u8  mcu_ir_mode;
            u8  no_of_frags;
            u16 mcu_major_v;
            u16 mcu_minor_v;
        } subcmd_21_23_01;
    };
};

struct ir_image_config {
    u8  ir_res_reg;
    u16 ir_exposure;
    u8  ir_leds; // Leds to enable, Strobe/Flashlight modes
    u16 ir_leds_intensity; // MSByte: Leds 1/2, LSB: Leds 3/4
    u8  ir_digital_gain;
    u8  ir_ex_light_filter;
    u32 ir_custom_register; // MSByte: Enable/Disable, Middle Byte: Edge smoothing, LSB: Color interpolation
    u16 ir_buffer_update_time;
    u8  ir_hand_analysis_mode;
    u8  ir_hand_analysis_threshold;
    u32 ir_denoise; // MSByte: Enable/Disable, Middle Byte: Edge smoothing, LSB: Color interpolation
    u8  ir_flip;
};

#pragma pack(pop)

extern s16 uint16_to_int16(u16 a);
extern void decode_stick_params(u16 *decoded_stick_params, u8 *encoded_stick_params);
extern void encode_stick_params(u8 *encoded_stick_params, u16 *decoded_stick_params);

extern std::string get_sn(u32 offset, const u16 read_len);
extern int get_spi_data(u32 offset, const u16 read_len, u8 *test_buf);
extern int write_spi_data(u32 offset, const u16 write_len, u8* test_buf);
extern int get_device_info(u8* test_buf);
extern int get_battery(u8* test_buf);
extern int get_temperature(u8* test_buf);
extern int dump_spi(const char *dev_name);
extern int send_rumble();
extern int play_tune(int tune_no);
extern int play_hd_rumble_file(int file_type, u16 sample_rate, int samples, int loop_start, int loop_end,
    int loop_wait, int loop_times, const u8 *loaded_file, const u8 *converted_file);
extern int send_custom_command(const u8* arg, size_t arg_len);
template <size_t N>
int send_custom_command(const u8 (&arg)[N]) {
    return send_custom_command(arg, N);
}
extern int send_internal_command(const u8* arg, size_t arg_len);
template <size_t N>
int send_internal_command(const u8 (&arg)[N]) {
    return send_internal_command(arg, N);
}
extern int device_connection();
extern int set_led_busy();
extern int button_test();
extern int ir_sensor(ir_image_config &ir_cfg);
extern int ir_sensor_config_live(ir_image_config &ir_cfg);
extern int nfc_tag_info();
extern int silence_input_report();

enum device_task_kind {
    DEVICE_TASK_NONE = 0,
    DEVICE_TASK_RUMBLE,
    DEVICE_TASK_SPI_BACKUP,
    DEVICE_TASK_BUTTON_TEST,
    DEVICE_TASK_IR,
    DEVICE_TASK_NFC,
    DEVICE_TASK_RESTORE
};

struct device_context {
    int slot;
    int device_type;
    unsigned short product_id;
    std::string path;
    std::wstring serial;
    hid_device *hid_handle;
    std::atomic<bool> connected;
    std::atomic<bool> present;
    std::atomic<bool> initialized;
    std::atomic<bool> cancel_requested;
    std::atomic<bool> enable_button_test_state;
    std::atomic<bool> enable_ir_video_photo_state;
    std::atomic<bool> enable_ir_auto_exposure_state;
    std::atomic<bool> enable_nfc_scanning_state;
    std::atomic<bool> cancel_spi_dump_state;
    std::atomic<int> task_kind;
    std::atomic<int> task_progress;
    std::atomic<int> task_result;
    std::atomic<unsigned long long> task_started_ms;
    std::atomic<unsigned long long> task_total_ms;
    u8 packet_counter;
    u8 ir_max_fragment;
    int ir_image_width;
    int ir_image_height;
    int ir_color_mode;
    int ir_exposure;
    CRITICAL_SECTION io_lock;
    CRITICAL_SECTION ir_config_lock;
    ir_image_config pending_ir_config;
    std::atomic<bool> pending_ir_config_update;

    device_context()
        : slot(-1), device_type(0), product_id(0), hid_handle(nullptr),
          connected(false), present(false), initialized(false), cancel_requested(false),
          enable_button_test_state(false), enable_ir_video_photo_state(false),
          enable_ir_auto_exposure_state(false), enable_nfc_scanning_state(false),
          cancel_spi_dump_state(false), task_kind(DEVICE_TASK_NONE),
          task_progress(0), task_result(0), task_started_ms(0), task_total_ms(0),
          packet_counter(0), ir_max_fragment(0), ir_image_width(0), ir_image_height(0),
          ir_color_mode(0), ir_exposure(0), pending_ir_config{}, pending_ir_config_update(false) {
        InitializeCriticalSection(&io_lock);
        InitializeCriticalSection(&ir_config_lock);
    }

    ~device_context() {
        DeleteCriticalSection(&io_lock);
        DeleteCriticalSection(&ir_config_lock);
    }
};

extern bool check_connection_ok;
extern device_context *device_slots[4];

extern device_context *current_device_context();
extern device_context *get_device_slot(int slot);
extern void set_active_device_context(device_context *device);
extern void set_thread_device_context(device_context *device);
extern int refresh_device_sessions();
extern void close_device_sessions();

// The legacy protocol code is expressed in terms of one controller. These
// aliases bind it to the session owned by the current UI or worker thread.
#define handle (current_device_context()->hid_handle)
#define handle_ok (current_device_context()->device_type)
#define enable_button_test (current_device_context()->enable_button_test_state)
#define enable_IRVideoPhoto (current_device_context()->enable_ir_video_photo_state)
#define enable_IRAutoExposure (current_device_context()->enable_ir_auto_exposure_state)
#define enable_NFCScanning (current_device_context()->enable_nfc_scanning_state)
#define cancel_spi_dump (current_device_context()->cancel_spi_dump_state)
#define timming_byte (current_device_context()->packet_counter)
#define ir_max_frag_no (current_device_context()->ir_max_fragment)

namespace CppWinFormJoy {
    class images
    {
        //For annoying designer..
        //Todo.
    };
}
