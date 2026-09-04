#pragma once

#include <wchar.h>

#ifdef JCTOOL_NATIVE_EXPORTS
#define JCTOOL_NATIVE_API __declspec(dllexport)
#else
#define JCTOOL_NATIVE_API __declspec(dllimport)
#endif

struct jc_device_snapshot
{
    unsigned int product_id;
    wchar_t serial_number[64];
    wchar_t product_name[96];
    wchar_t device_key[192];
    int details_available;
    wchar_t firmware[16];
    wchar_t mac_address[32];
    int battery_percent;
    int battery_charging;
    float battery_voltage;
    float temperature_celsius;
    int temperature_available;
    int colors_available;
    unsigned char colors[12];
};

struct jc_input_snapshot
{
    unsigned int buttons;
    int left_x;
    int left_y;
    int right_x;
    int right_y;
    short acceleration_x;
    short acceleration_y;
    short acceleration_z;
    short gyroscope_x;
    short gyroscope_y;
    short gyroscope_z;
    int connection_type;
    int battery_level;
    int charging;
};

struct jc_ir_config
{
    int resolution;
    int exposure_microseconds;
    int digital_gain;
    int leds_enabled;
    int external_light_filter;
    int flip_horizontal;
    int denoise;
};

struct jc_ir_frame_info
{
    int width;
    int height;
    int progress;
    int frame_ready;
    int average_intensity;
    int white_pixels;
    int ambient_pixels;
    unsigned int frame_number;
};

struct jc_nfc_tag
{
    unsigned char uid[10];
    int uid_length;
    int tag_type;
    int tag_model;
    int data_length;
};

struct jc_diagnostic_reply
{
    unsigned char data[0x170];
    int length;
    int matched;
    int accepted;
};

extern "C" JCTOOL_NATIVE_API int __cdecl jc_get_devices(
    jc_device_snapshot* devices,
    int capacity);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_read_spi(
    const wchar_t* device_key,
    unsigned int offset,
    unsigned char* output,
    int length);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_write_colors(
    const wchar_t* device_key,
    const unsigned char* colors,
    int length);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_write_spi(
    const wchar_t* device_key,
    unsigned int offset,
    const unsigned char* data,
    int length);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_identify(const wchar_t* device_key);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_play_rumble_raw(
    const wchar_t* device_key,
    const unsigned char* samples,
    int sample_count,
    int sample_rate_ms);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_stop_rumble(const wchar_t* device_key);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_read_input(
    const wchar_t* device_key,
    jc_input_snapshot* snapshot);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_start_input_stream(
    const wchar_t* device_key);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_read_input_stream(
    const wchar_t* device_key,
    jc_input_snapshot* snapshot,
    int timeout_ms);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_stop_input_stream(
    const wchar_t* device_key);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_start_ir_stream(
    const wchar_t* device_key,
    const jc_ir_config* config);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_read_ir_frame_fragment(
    const wchar_t* device_key,
    unsigned char* frame,
    int frame_capacity,
    jc_ir_frame_info* frame_info,
    int timeout_ms);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_stop_ir_stream(
    const wchar_t* device_key);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_scan_nfc(
    const wchar_t* device_key,
    jc_nfc_tag* tag,
    unsigned char* tag_data,
    int tag_data_capacity,
    int timeout_ms);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_cancel_nfc(
    const wchar_t* device_key);

extern "C" JCTOOL_NATIVE_API int __cdecl jc_send_diagnostic(
    const wchar_t* device_key,
    int internal_command,
    const unsigned char* arguments,
    int argument_length,
    jc_diagnostic_reply* reply);
