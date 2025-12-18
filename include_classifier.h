#ifndef INCLUDE_CLASSIFIER_H
#define INCLUDE_CLASSIFIER_H

#include <stddef.h>
#include "highlight.h"

/* Classify an include/import path for a given language.
 * Returns one of the HL_* include types (HL_INCLUDE_SYSTEM, HL_INCLUDE_THIRD_PARTY, HL_INCLUDE_LOCAL)
 * or HL_PREPROC for unknown/unclassified.
 */
enum HLType classify_include(const char *lang, const char *path, size_t len);

/* Test helpers: clear and inspect the in-memory include cache. Useful for unit tests. */
void include_cache_clear(void);
size_t include_cache_size(void);

/* Cache tuning (for tests or env setup). */
void include_cache_set_ttl_seconds(unsigned int sec);
void include_cache_set_max_entries(size_t max);

/* Persistence: enable persistence to the given path and save/load functions. */
int include_cache_enable_persistence(const char *path);
int include_cache_save_to_file(void);
int include_cache_load_from_file(void);

/* Async probes: enable/disable background probe worker. */
int include_cache_enable_async(void);
void include_cache_shutdown_async(void);
enum HLType classify_include_async(const char *lang, const char *path, size_t len);

/* Notification hooks: register a callback invoked when a cache entry is added/updated
 * or enable a signal to be raised (e.g., SIGUSR1) when entries are populated.
 */
void include_cache_register_update_callback(void (*cb)(const char *include, enum HLType val, void *userdata), void *userdata);
void include_cache_enable_update_signal(int signo);
void include_cache_disable_update_signal(void);

#endif
