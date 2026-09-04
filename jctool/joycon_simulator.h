#pragma once

#include <stddef.h>
#include <wchar.h>

#include "hidapi.h"

#ifdef __cplusplus
extern "C" {
#endif

int jc_simulator_enabled(void);
int jc_simulator_is_path(const char *path);
struct hid_device_info *jc_simulator_enumerate(unsigned short vendor_id, unsigned short product_id);
void *jc_simulator_open(const char *path);
void jc_simulator_close(void *handle);
int jc_simulator_write(void *handle, const unsigned char *data, size_t length);
int jc_simulator_read_timeout(void *handle, unsigned char *data, size_t length, int milliseconds);
int jc_simulator_get_manufacturer_string(void *handle, wchar_t *string, size_t maxlen);
int jc_simulator_get_product_string(void *handle, wchar_t *string, size_t maxlen);
int jc_simulator_get_serial_number_string(void *handle, wchar_t *string, size_t maxlen);

#ifdef __cplusplus
}
#endif
