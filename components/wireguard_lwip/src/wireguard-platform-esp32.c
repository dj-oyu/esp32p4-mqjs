/**
 * @file wireguard-platform-esp32.c
 * @brief ESP32 platform implementation for wireguard-lwip
 */

#include "wireguard-platform.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sys.h"
#include <string.h>
#include <sys/time.h>

/* ============================================================================
 * Time Functions
 * ========================================================================== */

uint32_t wireguard_sys_now() {
    // Use lwIP's built-in time function
    return sys_now();
}

/* TAI64N goes into every handshake initiation we send, and the responder
 * REJECTS any initiation whose timestamp is not strictly greater than the
 * greatest one it has already seen from us (WireGuard's replay defence). It
 * must therefore be WALL-CLOCK time: uptime restarts at zero on every boot, so
 * a peer that once stored "1970 + 175 s" from us silently refuses every
 * initiation for the first 175 seconds of the next boot -- and a peer we talk
 * to often (which therefore never trims the state) refuses us for as long as
 * its stored value stands. Device-verified 2026-08-25: the broker PC ignored
 * ten consecutive initiations while other peers, whose stored value happened
 * to be lower than that boot's uptime, answered the first one. The clock is
 * guaranteed here: tailscale_adapter only starts microlink after SNTP has set
 * it (time_is_valid()). If it is somehow unset we still send something
 * monotonic rather than nothing, but it will be rejected the same way. */
void wireguard_tai64n_now(uint8_t *output) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t seconds = (uint64_t)tv.tv_sec;
    uint32_t nanoseconds = (uint32_t)tv.tv_usec * 1000;

    if (tv.tv_sec < 1700000000) {   /* SNTP has not set the clock (~2023-11) */
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW("wg_plat", "TAI64N from an unset clock (%lld) -- peers that "
                     "hold a newer timestamp for us will reject the handshake",
                     (long long)tv.tv_sec);
        }
    }

    // TAI64 starts at 1970-01-01 00:00:10 TAI (Unix epoch + 10 seconds)
    // Add TAI offset: 2^62 + Unix time
    seconds += 0x400000000000000AULL;

    // Write in big-endian format
    output[0] = (seconds >> 56) & 0xFF;
    output[1] = (seconds >> 48) & 0xFF;
    output[2] = (seconds >> 40) & 0xFF;
    output[3] = (seconds >> 32) & 0xFF;
    output[4] = (seconds >> 24) & 0xFF;
    output[5] = (seconds >> 16) & 0xFF;
    output[6] = (seconds >> 8) & 0xFF;
    output[7] = seconds & 0xFF;

    output[8] = (nanoseconds >> 24) & 0xFF;
    output[9] = (nanoseconds >> 16) & 0xFF;
    output[10] = (nanoseconds >> 8) & 0xFF;
    output[11] = nanoseconds & 0xFF;
}

/* ============================================================================
 * Random Number Generation
 * ========================================================================== */

void wireguard_random_bytes(void *bytes, size_t size) {
    // Use ESP32 hardware RNG
    esp_fill_random(bytes, size);
}

/* ============================================================================
 * Load Management
 * ========================================================================== */

bool wireguard_is_under_load() {
    // For now, always return false (not under load)
    // Could be enhanced to check:
    // - Free heap memory
    // - CPU usage
    // - Number of active connections
    return false;
}
