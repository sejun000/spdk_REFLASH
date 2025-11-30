#pragma once

#include <cstddef>
#include <cstdint>

// Async IO completion callback
typedef void (*cache_device_io_cb)(void *cb_arg, int status);

class CacheDeviceInterface {
public:
    virtual ~CacheDeviceInterface() = default;

    // Sync APIs (for simulator)
    virtual int write_cache(uint64_t offset, const void *buf, size_t len) = 0;
    virtual int read_cache(uint64_t offset, void *buf, size_t len) = 0;
    virtual int reset_cache_region(uint64_t offset, size_t len) = 0;

    virtual int write_backend(uint64_t offset, const void *buf, size_t len) = 0;
    virtual int read_backend(uint64_t offset, void *buf, size_t len) = 0;
    virtual int trim_backend(uint64_t offset, size_t len) = 0;

    // Async APIs (for SPDK)
    virtual int write_cache_async(uint64_t offset, const void *buf, size_t len,
                                  cache_device_io_cb cb, void *cb_arg) {
        int rc = write_cache(offset, buf, len);
        if (cb) cb(cb_arg, rc);
        return 0;
    }
    virtual int read_cache_async(uint64_t offset, void *buf, size_t len,
                                 cache_device_io_cb cb, void *cb_arg) {
        int rc = read_cache(offset, buf, len);
        if (cb) cb(cb_arg, rc);
        return 0;
    }
    virtual int write_backend_async(uint64_t offset, const void *buf, size_t len,
                                    cache_device_io_cb cb, void *cb_arg) {
        int rc = write_backend(offset, buf, len);
        if (cb) cb(cb_arg, rc);
        return 0;
    }
    virtual int read_backend_async(uint64_t offset, void *buf, size_t len,
                                   cache_device_io_cb cb, void *cb_arg) {
        int rc = read_backend(offset, buf, len);
        if (cb) cb(cb_arg, rc);
        return 0;
    }
    virtual int reset_cache_region_async(uint64_t offset, size_t len,
                                         cache_device_io_cb cb, void *cb_arg) {
        int rc = reset_cache_region(offset, len);
        if (cb) cb(cb_arg, rc);
        return 0;
    }

    // ZNS Zone Open with ZRWA (Zone Random Write Area)
    // offset: byte offset within the zone to open
    virtual int open_zone_zrwa_async(uint64_t offset, cache_device_io_cb cb, void *cb_arg) {
        // Default: no-op for non-ZNS devices
        if (cb) cb(cb_arg, 0);
        return 0;
    }
};
