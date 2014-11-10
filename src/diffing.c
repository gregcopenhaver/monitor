/* Cuckoo Sandbox - Automated Malware Analysis.
Copyright (C) 2010-2014 Cuckoo Foundation.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "misc.h"

typedef struct _module_t {
    uintptr_t base;
    uintptr_t end;
    uint64_t  hash;
} module_t;

#define MAX_MODULE_COUNT 256

static uint32_t g_module_count, g_list_length;
static module_t g_modules[MAX_MODULE_COUNT];
static uint64_t *g_list;

static uint64_t _get_module_hash(HMODULE module_handle)
{
    wchar_t *module_path = get_unicode_buffer();
    wchar_t *full_path = get_unicode_buffer();

    GetModuleFileNameW(module_handle, module_path, MAX_PATH_W);
    uint32_t length = path_get_full_pathW(module_path, full_path);

    return hash_stringW(full_path, length);
}

static uint64_t _address_hash(uintptr_t addr)
{
    // TODO Use divide and conquer here as well.
    for (uint32_t idx = 0; idx < g_module_count; idx++) {
        if(addr >= g_modules[idx].base && addr < g_modules[idx].end) {
            uint64_t ret = addr - g_modules[idx].base;
            ret = g_modules[idx].hash ^ hash_buffer(&ret, sizeof(uint32_t));
            return ret == 0 ? 1 : ret;
        }
    }
    return 0;
}

static uint64_t _stacktrace_hash()
{
    uintptr_t return_addresses[32], count = 0, hashcnt = 0;
    uint64_t hashes[64];

#if !__x86_64__
    count = stacktrace(get_ebp(), return_addresses,
        sizeof(return_addresses) / sizeof(uintptr_t));
#endif

    for (uint32_t idx = 0; idx < count; idx++) {
        uint64_t hash = _address_hash(return_addresses[idx]);
        if(hash == 0) {
            // If we can't find the module hash then we have to create one.
            const uint8_t *module_address =
                module_from_address((const uint8_t *) return_addresses[idx]);

            // If there's no module associated with this address then we
            // automatically tag this address as interesting.
            if(module_address == NULL) {
                return 0;
            }

            // Add an entry for this module. TODO Spinlock around this code.
            g_modules[g_module_count].base = (uintptr_t) module_address;
            g_modules[g_module_count].end = (uintptr_t) module_address +
                module_image_size(module_address);
            g_modules[g_module_count++].hash =
                _get_module_hash((HMODULE) module_address);

            hash = _address_hash(return_addresses[idx]);
        }

        hashes[hashcnt++] = hash;
    }

    return hash_buffer(hashes, sizeof(uint64_t) * hashcnt);
}

static uint64_t _parameter_hash(const char *fmt, va_list args)
{
    uint32_t value, hashcnt = 0, *valueptr;
    uint64_t hashes[64];

    while (*fmt != 0) {
        switch (*fmt++) {
        case 's':
            hashes[hashcnt++] = hash_string(va_arg(args, const char *), -1);
            break;

        case 'S':
            value = va_arg(args, uint32_t);
            hashes[hashcnt++] =
                hash_string(va_arg(args, const char *), value);
            break;

        case 'u':
            hashes[hashcnt++] =
                hash_stringW(va_arg(args, const wchar_t *), -1);
            break;

        case 'U':
            value = va_arg(args, uint32_t);
            hashes[hashcnt++] =
                hash_stringW(va_arg(args, const wchar_t *), value);
            break;

        case 'i': case 'l': case 'p':
            value = va_arg(args, uint32_t);
            hashes[hashcnt++] = hash_buffer(&value, sizeof(uint32_t));
            break;

        case 'I': case 'L': case 'P':
            valueptr = va_arg(args, uint32_t *);
            if(valueptr != NULL) {
                hashes[hashcnt++] = hash_buffer(valueptr, sizeof(uint32_t));
            }
            break;

        case 'b':
            value = va_arg(args, uint32_t);
            hashes[hashcnt++] = hash_buffer(va_arg(args, void *), value);
            break;
        }
    }

    return hash_buffer(hashes, sizeof(uint64_t) * hashcnt);
}

static int _value_in_list(uint64_t value, uint64_t *list, uint32_t length)
{
    uint32_t low = 0, high = length;

    // No list is available.
    if(list == NULL || length == 0) {
        return 1;
    }

    while (high - low > 1) {
        uint32_t index = low + (high - low) / 2;
        if(value == list[index]) {
            return 1;
        }

        if(value > list[index]) {
            low = index;
            continue;
        }

        if(value < list[index]) {
            high = index;
            continue;
        }
    }

    if(value == list[low] || value == list[high]) {
        return 1;
    }

    return 0;
}

static int _sort_uint64(const void *a, const void *b)
{
    uint64_t _a = *(const uint64_t *) a;
    uint64_t _b = *(const uint64_t *) b;
    return _a - _b;
}

void diffing_init(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if(fp != NULL) {
        fseek(fp, 0, SEEK_END);
        uint32_t filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        g_list = (uint64_t *) malloc(filesize);
        if(g_list != NULL) {
            fread(g_list, filesize, 1, fp);

            g_list_length = filesize / sizeof(uint64_t);
            qsort(g_list, g_list_length, sizeof(uint64_t), &_sort_uint64);
        }

        fclose(fp);
        DeleteFile(path);
    }
}

uint64_t call_hash(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    uint64_t ret = _stacktrace_hash() ^ _parameter_hash(fmt, args);

    va_end(args);
    return ret;
}

int is_interesting_hash(uint64_t hash)
{
    return _value_in_list(hash, g_list, g_list_length);
}
