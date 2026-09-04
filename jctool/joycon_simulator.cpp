#include "joycon_simulator.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JC_SIM_DEVICE_COUNT 4
#define JC_SIM_SPI_SIZE 0x80000
#define JC_SIM_REPORT_SIZE 64

struct jc_sim_device {
    CRITICAL_SECTION lock;
    unsigned short product_id;
    wchar_t serial[32];
    char path[32];
    unsigned char spi[JC_SIM_SPI_SIZE];
    int battery_percent;
    BOOL present;
};

struct jc_sim_handle {
    jc_sim_device *device;
    unsigned char pending[JC_SIM_REPORT_SIZE];
    size_t pending_length;
    unsigned char input_mode;
    unsigned char timer;
};

static INIT_ONCE simulator_init_once = INIT_ONCE_STATIC_INIT;
static jc_sim_device simulator_devices[JC_SIM_DEVICE_COUNT];

static void encode_stick_pair(unsigned char *target, unsigned short x, unsigned short y) {
    target[0] = (unsigned char)(x & 0xFF);
    target[1] = (unsigned char)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    target[2] = (unsigned char)((y >> 4) & 0xFF);
}

static void write_u16(unsigned char *target, unsigned short value) {
    target[0] = (unsigned char)(value & 0xFF);
    target[1] = (unsigned char)(value >> 8);
}

static void write_u32(unsigned char *target, unsigned long value) {
    target[0] = (unsigned char)(value & 0xFF);
    target[1] = (unsigned char)((value >> 8) & 0xFF);
    target[2] = (unsigned char)((value >> 16) & 0xFF);
    target[3] = (unsigned char)((value >> 24) & 0xFF);
}

static unsigned long read_u32(const unsigned char *source) {
    return (unsigned long)source[0]
        | ((unsigned long)source[1] << 8)
        | ((unsigned long)source[2] << 16)
        | ((unsigned long)source[3] << 24);
}

static void initialize_spi(jc_sim_device *device, int index) {
    memset(device->spi, 0xFF, sizeof(device->spi));

    static const unsigned char backup_magic[20] = {
        0x01, 0x08, 0x00, 0xF0, 0x00, 0x00, 0x62, 0x08, 0xC0, 0x5D,
        0x89, 0xFD, 0x04, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x40, 0x06
    };
    memcpy(device->spi, backup_magic, sizeof(backup_magic));

    char serial[16];
    sprintf_s(serial, sizeof(serial), "SIM%012d", index + 1);
    memset(device->spi + 0x6000, 0, 0x10);
    memcpy(device->spi + 0x6001, serial, 15);
    device->spi[0x6012] = device->product_id == 0x2006 ? 0x01
        : (device->product_id == 0x2007 ? 0x02 : 0x03);
    device->spi[0x15] = static_cast<unsigned char>(index + 1);
    device->spi[0x16] = 0x10;
    device->spi[0x17] = 0x00;
    device->spi[0x18] = 0x00;
    device->spi[0x19] = 0x00;
    device->spi[0x1A] = 0x02;

    static const unsigned char colors[JC_SIM_DEVICE_COUNT][12] = {
        { 0x00, 0xC3, 0xE3, 0x00, 0x1E, 0x1E, 0x00, 0xC3, 0xE3, 0x00, 0xC3, 0xE3 },
        { 0xFF, 0x45, 0x5C, 0x1E, 0x0A, 0x0A, 0xFF, 0x45, 0x5C, 0xFF, 0x45, 0x5C },
        { 0xE6, 0xE6, 0xE6, 0x32, 0x32, 0x32, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46 },
        { 0xFF, 0xCC, 0x00, 0x28, 0x28, 0x28, 0xFF, 0xCC, 0x00, 0xFF, 0xCC, 0x00 }
    };
    memcpy(device->spi + 0x6050, colors[index], sizeof(colors[index]));

    memset(device->spi + 0x6020, 0, 0x18);
    write_u16(device->spi + 0x6026, 0x4000);
    write_u16(device->spi + 0x6028, 0x4000);
    write_u16(device->spi + 0x602A, 0x4000);
    write_u16(device->spi + 0x6032, 0x343B);
    write_u16(device->spi + 0x6034, 0x343B);
    write_u16(device->spi + 0x6036, 0x343B);

    unsigned char *stick = device->spi + 0x603D;
    encode_stick_pair(stick + 0, 0x600, 0x600);
    encode_stick_pair(stick + 3, 0x800, 0x800);
    encode_stick_pair(stick + 6, 0x600, 0x600);
    encode_stick_pair(stick + 9, 0x800, 0x800);
    encode_stick_pair(stick + 12, 0x600, 0x600);
    encode_stick_pair(stick + 15, 0x600, 0x600);

    memset(device->spi + 0x6080, 0, 0x2A);
    encode_stick_pair(device->spi + 0x6086, 0x100, 0x100);
    device->spi[0x6089] = 0xAE;
    encode_stick_pair(device->spi + 0x608A, 0x600, 0x600);
}

static BOOL CALLBACK initialize_simulator(PINIT_ONCE, PVOID, PVOID *) {
    for (int i = 0; i < JC_SIM_DEVICE_COUNT; ++i) {
        jc_sim_device *device = &simulator_devices[i];
        InitializeCriticalSection(&device->lock);
        sprintf_s(device->path, sizeof(device->path), "jctool-sim://%d", i + 1);
        swprintf_s(device->serial, _countof(device->serial), L"SIM-%04d", i + 1);
        device->product_id = (i % 2 == 0) ? 0x2006 : 0x2007;
        device->battery_percent = 100 - i * 20;
        device->present = FALSE;
        initialize_spi(device, i);
    }
    return TRUE;
}

static void ensure_initialized(void) {
    InitOnceExecuteOnce(&simulator_init_once, initialize_simulator, NULL, NULL);
}

static unsigned short type_to_product(const wchar_t *type) {
    if (_wcsicmp(type, L"L") == 0 || _wcsicmp(type, L"LEFT") == 0)
        return 0x2006;
    if (_wcsicmp(type, L"R") == 0 || _wcsicmp(type, L"RIGHT") == 0)
        return 0x2007;
    if (_wcsicmp(type, L"P") == 0 || _wcsicmp(type, L"PRO") == 0)
        return 0x2009;
    return 0;
}

static void sync_spi_serial(jc_sim_device *device) {
    char serial[16] = {};
    WideCharToMultiByte(CP_ACP, 0, device->serial, -1, serial, sizeof(serial), NULL, NULL);
    memset(device->spi + 0x6000, 0, 0x10);
    memcpy(device->spi + 0x6001, serial, strnlen_s(serial, 15));
}

static void load_list_configuration(const wchar_t *list) {
    wchar_t value[256];
    wcsncpy_s(value, _countof(value), list, _TRUNCATE);
    wchar_t *context = NULL;
    wchar_t *token = wcstok_s(value, L",; ", &context);
    int index = 0;
    while (token && index < JC_SIM_DEVICE_COUNT) {
        unsigned short product_id = type_to_product(token);
        EnterCriticalSection(&simulator_devices[index].lock);
        if (product_id != 0) {
            simulator_devices[index].product_id = product_id;
            simulator_devices[index].present = TRUE;
        }
        LeaveCriticalSection(&simulator_devices[index].lock);
        token = wcstok_s(NULL, L",; ", &context);
        ++index;
    }
}

static void load_ini_configuration(const wchar_t *path) {
    for (int i = 0; i < JC_SIM_DEVICE_COUNT; ++i) {
        wchar_t section[16];
        wchar_t type[16];
        wchar_t serial[32];
        swprintf_s(section, _countof(section), L"device%d", i + 1);
        GetPrivateProfileStringW(section, L"type", (i % 2 == 0) ? L"L" : L"R",
            type, _countof(type), path);
        GetPrivateProfileStringW(section, L"serial", simulator_devices[i].serial,
            serial, _countof(serial), path);

        unsigned short product_id = type_to_product(type);
        int connected = GetPrivateProfileIntW(section, L"connected", 0, path);
        int battery_percent = GetPrivateProfileIntW(section, L"battery", 100, path);
        if (battery_percent < 0)
            battery_percent = 0;
        if (battery_percent > 100)
            battery_percent = 100;

        EnterCriticalSection(&simulator_devices[i].lock);
        simulator_devices[i].present = connected != 0;
        if (product_id != 0)
            simulator_devices[i].product_id = product_id;
        wcsncpy_s(simulator_devices[i].serial, _countof(simulator_devices[i].serial), serial, _TRUNCATE);
        simulator_devices[i].battery_percent = battery_percent;
        sync_spi_serial(&simulator_devices[i]);
        LeaveCriticalSection(&simulator_devices[i].lock);
    }
}

static BOOL reload_configuration(void) {
    ensure_initialized();
    wchar_t ini_path[MAX_PATH];
    wchar_t list[256];
    DWORD ini_length = GetEnvironmentVariableW(L"JCTOOL_SIMULATOR_CONFIG", ini_path, _countof(ini_path));
    DWORD list_length = GetEnvironmentVariableW(L"JCTOOL_SIMULATORS", list, _countof(list));
    if (ini_length == 0 && list_length == 0)
        return FALSE;

    for (int i = 0; i < JC_SIM_DEVICE_COUNT; ++i) {
        EnterCriticalSection(&simulator_devices[i].lock);
        simulator_devices[i].present = FALSE;
        LeaveCriticalSection(&simulator_devices[i].lock);
    }

    if (ini_length > 0 && ini_length < _countof(ini_path))
        load_ini_configuration(ini_path);
    else if (list_length > 0 && list_length < _countof(list))
        load_list_configuration(list);
    return TRUE;
}

static hid_device_info *make_device_info(const jc_sim_device *device) {
    hid_device_info *info = (hid_device_info *)calloc(1, sizeof(hid_device_info));
    if (!info)
        return NULL;
    info->path = _strdup(device->path);
    info->vendor_id = 0x057E;
    info->product_id = device->product_id;
    info->serial_number = _wcsdup(device->serial);
    info->release_number = 0x0100;
    info->manufacturer_string = _wcsdup(L"Nintendo");
    if (device->product_id == 0x2006)
        info->product_string = _wcsdup(L"Joy-Con (L) [Simulator]");
    else if (device->product_id == 0x2007)
        info->product_string = _wcsdup(L"Joy-Con (R) [Simulator]");
    else
        info->product_string = _wcsdup(L"Pro Controller [Simulator]");
    info->usage_page = 0x01;
    info->usage = 0x05;
    info->interface_number = -1;
    return info;
}

int jc_simulator_enabled(void) {
    return reload_configuration() ? 1 : 0;
}

int jc_simulator_is_path(const char *path) {
    return path && strncmp(path, "jctool-sim://", 13) == 0;
}

hid_device_info *jc_simulator_enumerate(unsigned short vendor_id, unsigned short product_id) {
    if (!reload_configuration() || (vendor_id != 0 && vendor_id != 0x057E))
        return NULL;

    hid_device_info *root = NULL;
    hid_device_info *tail = NULL;
    for (int i = 0; i < JC_SIM_DEVICE_COUNT; ++i) {
        jc_sim_device *device = &simulator_devices[i];
        EnterCriticalSection(&device->lock);
        BOOL matches = device->present && (product_id == 0 || product_id == device->product_id);
        hid_device_info *info = matches ? make_device_info(device) : NULL;
        LeaveCriticalSection(&device->lock);
        if (!info)
            continue;
        if (tail)
            tail->next = info;
        else
            root = info;
        tail = info;
    }
    return root;
}

void *jc_simulator_open(const char *path) {
    if (!reload_configuration() || !jc_simulator_is_path(path))
        return NULL;
    int index = atoi(path + 13) - 1;
    if (index < 0 || index >= JC_SIM_DEVICE_COUNT)
        return NULL;
    EnterCriticalSection(&simulator_devices[index].lock);
    BOOL present = simulator_devices[index].present;
    LeaveCriticalSection(&simulator_devices[index].lock);
    if (!present)
        return NULL;
    jc_sim_handle *handle = (jc_sim_handle *)calloc(1, sizeof(jc_sim_handle));
    if (handle)
        handle->device = &simulator_devices[index];
    return handle;
}

void jc_simulator_close(void *opaque_handle) {
    free(opaque_handle);
}

static unsigned char battery_code(int percent) {
    if (percent <= 5) return 0;
    if (percent <= 25) return 2;
    if (percent <= 50) return 4;
    if (percent <= 75) return 6;
    return 8;
}

static unsigned short battery_voltage(int percent) {
    if (percent <= 5) return 0x0550;
    if (percent <= 25) return 0x05B0;
    if (percent <= 50) return 0x0600;
    if (percent <= 75) return 0x0630;
    return 0x0660;
}

static void begin_reply(jc_sim_handle *handle, unsigned char subcommand, unsigned char ack) {
    memset(handle->pending, 0, sizeof(handle->pending));
    handle->pending[0] = 0x21;
    handle->pending[1] = handle->timer++;
    handle->pending[2] = (unsigned char)((battery_code(handle->device->battery_percent) << 4) | 0x06);
    encode_stick_pair(handle->pending + 6, 0x800, 0x800);
    encode_stick_pair(handle->pending + 9, 0x800, 0x800);
    handle->pending[13] = ack;
    handle->pending[14] = subcommand;
    handle->pending_length = 49;
}

static int device_index(const jc_sim_device *device) {
    return (int)(device - simulator_devices);
}

int jc_simulator_write(void *opaque_handle, const unsigned char *data, size_t length) {
    jc_sim_handle *handle = (jc_sim_handle *)opaque_handle;
    if (!handle || !handle->device || !data || length == 0)
        return -1;

    EnterCriticalSection(&handle->device->lock);
    if (data[0] == 0x01 && length > 10) {
        unsigned char subcommand = data[10];
        unsigned char ack = 0x80;
        if (subcommand == 0x02) ack = 0x82;
        if (subcommand == 0x10) ack = 0x90;
        if (subcommand == 0x43) ack = 0xC0;
        if (subcommand == 0x50) ack = 0xD0;
        begin_reply(handle, subcommand, ack);

        if (subcommand == 0x02) {
            int index = device_index(handle->device);
            handle->pending[15] = 0x04;
            handle->pending[16] = 0x21;
            handle->pending[17] = handle->device->product_id == 0x2006 ? 0x01
                : (handle->device->product_id == 0x2007 ? 0x02 : 0x03);
            handle->pending[18] = 0x02;
            handle->pending[19] = 0x02;
            handle->pending[20] = 0x00;
            handle->pending[21] = 0x00;
            handle->pending[22] = 0x00;
            handle->pending[23] = 0x10;
            handle->pending[24] = (unsigned char)(index + 1);
        }
        else if (subcommand == 0x03 && length > 11) {
            handle->input_mode = data[11];
        }
        else if (subcommand == 0x10 && length > 15) {
            unsigned long offset = read_u32(data + 11);
            size_t requested = data[15];
            size_t available = offset < JC_SIM_SPI_SIZE ? JC_SIM_SPI_SIZE - offset : 0;
            size_t count = requested < available ? requested : available;
            if (count > 29) count = 29;
            write_u32(handle->pending + 15, offset);
            handle->pending[19] = (unsigned char)count;
            if (count > 0)
                memcpy(handle->pending + 20, handle->device->spi + offset, count);
            handle->pending_length = 20 + count;
        }
        else if (subcommand == 0x11 && length > 16) {
            unsigned long offset = read_u32(data + 11);
            size_t requested = data[15];
            size_t available = offset < JC_SIM_SPI_SIZE ? JC_SIM_SPI_SIZE - offset : 0;
            size_t count = requested < available ? requested : available;
            if (count > length - 16) count = length - 16;
            if (count > 0)
                memcpy(handle->device->spi + offset, data + 16, count);
        }
        else if (subcommand == 0x43) {
            if (length > 12 && data[11] == 0x10)
                handle->pending[17] = 0x10;
            else {
                handle->pending[17] = 0x00;
                handle->pending[18] = 0x00;
            }
        }
        else if (subcommand == 0x50) {
            write_u16(handle->pending + 15, battery_voltage(handle->device->battery_percent));
        }
    }
    else {
        handle->pending_length = 0;
    }
    LeaveCriticalSection(&handle->device->lock);
    return (int)length;
}

static size_t make_input_report(jc_sim_handle *handle) {
    memset(handle->pending, 0, sizeof(handle->pending));
    handle->pending[0] = handle->input_mode == 0 ? 0x30 : handle->input_mode;
    handle->pending[1] = handle->timer++;
    handle->pending[2] = (unsigned char)((battery_code(handle->device->battery_percent) << 4) | 0x06);
    const int input_step = (handle->timer / 8) % 4;
    if (handle->device->product_id == 0x2006) {
        handle->pending[5] = input_step == 0 ? 0x42 : (unsigned char)(1u << (input_step - 1));
    }
    else if (handle->device->product_id == 0x2007) {
        handle->pending[3] = input_step == 0 ? 0x08 : (unsigned char)(1u << (input_step - 1));
    }
    else {
        handle->pending[3] = input_step == 0 ? 0x08 : 0x00;
        handle->pending[5] = input_step == 0 ? 0x02 : (unsigned char)(1u << (input_step - 1));
    }
    encode_stick_pair(handle->pending + 6, 0x800, 0x800);
    encode_stick_pair(handle->pending + 9, 0x800, 0x800);
    write_u16(handle->pending + 17, 0x4000);
    handle->pending_length = 49;
    return handle->pending_length;
}

int jc_simulator_read_timeout(void *opaque_handle, unsigned char *data, size_t length, int milliseconds) {
    jc_sim_handle *handle = (jc_sim_handle *)opaque_handle;
    if (!handle || !handle->device || (!data && length > 0))
        return -1;

    EnterCriticalSection(&handle->device->lock);
    if (handle->pending_length == 0 && handle->input_mode >= 0x30 && handle->input_mode <= 0x3F)
        make_input_report(handle);
    size_t copy_length = length < handle->pending_length ? length : handle->pending_length;
    if (copy_length > 0)
        memcpy(data, handle->pending, copy_length);
    handle->pending_length = 0;
    LeaveCriticalSection(&handle->device->lock);

    if (copy_length == 0 && milliseconds > 0)
        Sleep((DWORD)(milliseconds < 8 ? milliseconds : 8));
    return (int)copy_length;
}

static int copy_string(const wchar_t *source, wchar_t *target, size_t maxlen) {
    if (!target || maxlen == 0)
        return -1;
    wcsncpy_s(target, maxlen, source, _TRUNCATE);
    return 0;
}

int jc_simulator_get_manufacturer_string(void *, wchar_t *string, size_t maxlen) {
    return copy_string(L"Nintendo", string, maxlen);
}

int jc_simulator_get_product_string(void *opaque_handle, wchar_t *string, size_t maxlen) {
    jc_sim_handle *handle = (jc_sim_handle *)opaque_handle;
    if (!handle || !handle->device)
        return -1;
    if (handle->device->product_id == 0x2006)
        return copy_string(L"Joy-Con (L) [Simulator]", string, maxlen);
    if (handle->device->product_id == 0x2007)
        return copy_string(L"Joy-Con (R) [Simulator]", string, maxlen);
    return copy_string(L"Pro Controller [Simulator]", string, maxlen);
}

int jc_simulator_get_serial_number_string(void *opaque_handle, wchar_t *string, size_t maxlen) {
    jc_sim_handle *handle = (jc_sim_handle *)opaque_handle;
    if (!handle || !handle->device)
        return -1;
    return copy_string(handle->device->serial, string, maxlen);
}
