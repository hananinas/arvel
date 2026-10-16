#ifndef WEB_AUTH_HOST_FIXTURE_H
#define WEB_AUTH_HOST_FIXTURE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call before the real core's first operation, or in a fresh exec process.
 * The fixture uses real OpenSSL crypto, host kernel entropy, a controllable
 * monotonic clock and a bounded file-backed NVS API. Commit failure models an
 * unchanged record; actual IDF NVS can complete a failed write on reboot, so
 * target power-loss/storage faults must be tested separately. */
enum fixture_failure { FIXTURE_NONE, FIXTURE_OPEN, FIXTURE_READ, FIXTURE_SET, FIXTURE_COMMIT };
void fixture_init(const char *record_path, bool nvs_available);
void fixture_fail_next(enum fixture_failure failure);
void fixture_nvs_available(bool available);
void fixture_advance_time(int64_t microseconds);
void fixture_derivation_delay(int64_t microseconds);
uint64_t fixture_derivation_count(void);
#ifdef __cplusplus
}
#endif
#endif
