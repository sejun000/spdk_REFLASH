#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/uio.h>

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
    // Scatter-gather read from cache (for read coalescing)
    virtual int readv_cache_async(uint64_t offset, struct iovec *iovs, int iovcnt,
                                  size_t total_len, cache_device_io_cb cb, void *cb_arg) {
        // Default: sequential reads
        for (int i = 0; i < iovcnt; i++) {
            int rc = read_cache(offset, iovs[i].iov_base, iovs[i].iov_len);
            if (rc != 0) {
                if (cb) cb(cb_arg, rc);
                return 0;
            }
            offset += iovs[i].iov_len;
        }
        if (cb) cb(cb_arg, 0);
        return 0;
    }
    virtual int write_backend_async(uint64_t offset, const void *buf, size_t len,
                                    cache_device_io_cb cb, void *cb_arg) {
        int rc = write_backend(offset, buf, len);
        if (cb) cb(cb_arg, rc);
        return 0;
    }
    // Scatter-gather write to backend (for LBA coalescing)
    virtual int writev_backend_async(uint64_t offset, struct iovec *iovs, int iovcnt,
                                     size_t total_len, cache_device_io_cb cb, void *cb_arg) {
        // Default: fall back to sequential writes
        for (int i = 0; i < iovcnt; i++) {
            int rc = write_backend(offset, iovs[i].iov_base, iovs[i].iov_len);
            if (rc != 0) {
                if (cb) cb(cb_arg, rc);
                return 0;
            }
            offset += iovs[i].iov_len;
        }
        if (cb) cb(cb_arg, 0);
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
