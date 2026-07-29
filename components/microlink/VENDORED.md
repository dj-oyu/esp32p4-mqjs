# Vendored: microlink (Tailscale-compatible VPN client)

Upstream: <https://github.com/CamM2325/microlink> (MIT), HEAD ~2026-03-17.

Vendored as a patched copy (not a git submodule / managed component) because the
upstream targets ESP-IDF 5.x and does **not** build unmodified on ESP-IDF 6.0 /
ESP32-P4. The bundled `components/wireguard_lwip` is vendored alongside it (it is
a nested component upstream; flattened to a sibling here for Windows/IDF
component discovery). Rationale and the full de-risking record:
[`docs/history/tailscale-microlink-plan.md`](../../docs/history/tailscale-microlink-plan.md).

## Local patches vs upstream (IDF 6.0 / ESP32-P4 / GCC 15)

1. **cJSON** — `CMakeLists.txt`: `REQUIRES json` → `cjson`. IDF 6.0 removed cJSON
   from core; it is now the managed component `espressif/cjson` (registers as
   `cjson`). Declared in `idf_component.yml`. `#include "cJSON.h"` unchanged.

2. **IDF 6.0 driver split** — `CMakeLists.txt`: added `esp_driver_uart` +
   `esp_driver_gpio` to `REQUIRES` (cellular `ml_cellular.c` / `ml_at_socket.c`
   include `driver/uart.h` / `driver/gpio.h`). Cellular is unused here and these
   two sources may later be excluded to drop the dependency.

3. **GCC 15 / strict warnings** — both this component and `wireguard_lwip` add
   `-Wno-error=unterminated-string-initialization` (fixed-size `unsigned char`
   arrays initialised from string literals without room for the NUL). This
   component also adds `-Wno-error=stringop-truncation` (`ml_peer_nvs.c` /
   `ml_wg_mgr.c` `strncpy` into fixed hostname buffers — intentional
   truncation; this project promotes the warning to an error, upstream does
   not).

4. **mbedTLS 4 (PSA) port** — IDF 6.0 ships Mbed TLS 4.0 (PSA / TF-PSA-Crypto)
   which dropped the legacy crypto modules microlink used:
   - `include/microlink_internal.h`: `mbedtls/entropy.h` + `ctr_drbg.h` →
     `psa/crypto.h`; removed the `mbedtls_entropy_context` /
     `mbedtls_ctr_drbg_context` fields from the DERP state struct.
   - `src/ml_noise.c`: ChaCha20-Poly1305 via PSA one-shot
     `psa_aead_encrypt` / `psa_aead_decrypt` (`PSA_KEY_TYPE_CHACHA20` +
     `PSA_ALG_CHACHA20_POLY1305`). PSA output is ciphertext‖tag contiguous,
     matching the original layout (no temp buffer).
   - `src/ml_derp.c`: TLS randomness via PSA-global RNG — `psa_crypto_init()`
     before TLS setup; removed `mbedtls_entropy_*` / `mbedtls_ctr_drbg_*` /
     `mbedtls_ssl_conf_rng()` (the latter no longer exists in mbedTLS 4).

   Runtime caveat: PSA ChaCha20-Poly1305 must be enabled in sdkconfig
   (`CONFIG_MBEDTLS_CHACHAPOLY_C` / `PSA_WANT_ALG_CHACHA20_POLY1305`); compile
   passes regardless but `psa_aead_*` returns `PSA_ERROR_NOT_SUPPORTED` if off.

5. **Default DERP home region Dallas(9) → Tokyo(7)** —
   `include/microlink_internal.h`: `ML_DERP_REGION 9`→`7`, `ML_DERP_HOST
   "derp9e..."`→`"derp7e.tailscale.com"`. The control plane sets our HomeDERP
   from the `PreferredDERP` we advertise (NetInfo), and microlink has no
   latency-based DERP auto-selection (real Tailscale clients ping all regions
   and pick the nearest). With the upstream default a device in Japan relays
   through Dallas — high RTT and enough CPU load to trip `ml_wg_mgr`'s task
   watchdog even for same-LAN peers. Region 7 (`tok`) is Tokyo. `ML_DERP_HOST`
   is only the pre-DERPMap bootstrap host; once the control plane's DERPMap is
   parsed, `ml_derp_connect()` looks up the home region's nodes by `RegionID`.
