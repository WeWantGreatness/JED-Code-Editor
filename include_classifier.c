#define _POSIX_C_SOURCE 200809L
#include "include_classifier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>
#include <time.h>
#include <signal.h>

/* Simple in-memory cache for classifier results to avoid repeated subprocess probes */
#define INC_CACHE_BUCKETS 1024

/* Cache entry with LRU pointers and last access time. */
struct IncCacheEntry {
    char *key;
    enum HLType val;
    struct IncCacheEntry *next; /* bucket chain */
    struct IncCacheEntry *prev; /* LRU prev */
    struct IncCacheEntry *lru_next; /* LRU next */
    time_t last_access;
};

static struct IncCacheEntry *g_inc_cache[INC_CACHE_BUCKETS];
static size_t g_inc_cache_count = 0;
/* LRU list: head is most recent, tail is least recent */
static struct IncCacheEntry *g_inc_lru_head = NULL;
static struct IncCacheEntry *g_inc_lru_tail = NULL;
static pthread_mutex_t g_inc_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* TTL and max entries */
#define INC_CACHE_DEFAULT_TTL 3600 /* seconds */
static unsigned int g_inc_cache_ttl = INC_CACHE_DEFAULT_TTL;
#define INC_CACHE_DEFAULT_MAX 4096
static size_t g_inc_cache_max_entries = INC_CACHE_DEFAULT_MAX;

/* For persistence */
static char *g_persist_path = NULL;
static int g_persist_enabled = 0;

/* For async worker */
static int g_async_enabled = 0;
static pthread_t g_async_thread;
static int g_async_thread_running = 0;

/* Update notification support (declared early so functions that set cache can use them) */
static void (*g_update_cb)(const char *include, enum HLType val, void *userdata) = NULL;
static void *g_update_cb_user = NULL;
static int g_update_signal = 0;

static unsigned long djb2_hash(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}

static void lru_move_to_head(struct IncCacheEntry *e) {
    if (!e || g_inc_lru_head == e) return;
    /* unlink from LRU list */
    if (e->prev) e->prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->prev = e->prev;
    if (g_inc_lru_tail == e) g_inc_lru_tail = e->prev;
    /* insert at head */
    e->prev = NULL;
    e->lru_next = g_inc_lru_head;
    if (g_inc_lru_head) g_inc_lru_head->prev = e;
    g_inc_lru_head = e;
    if (!g_inc_lru_tail) g_inc_lru_tail = e;
}

static void lru_insert_head(struct IncCacheEntry *e) {
    e->prev = NULL;
    e->lru_next = g_inc_lru_head;
    if (g_inc_lru_head) g_inc_lru_head->prev = e;
    g_inc_lru_head = e;
    if (!g_inc_lru_tail) g_inc_lru_tail = e;
}

static void lru_unlink(struct IncCacheEntry *e) {
    if (!e) return;
    if (e->prev) e->prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->prev = e->prev;
    if (g_inc_lru_head == e) g_inc_lru_head = e->lru_next;
    if (g_inc_lru_tail == e) g_inc_lru_tail = e->prev;
    e->prev = e->lru_next = NULL;
}

static int inc_cache_get(const char *key, enum HLType *out) {
    pthread_mutex_lock(&g_inc_cache_lock);
    unsigned long h = djb2_hash(key) % INC_CACHE_BUCKETS;
    for (struct IncCacheEntry *e = g_inc_cache[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            time_t now = time(NULL);
            if (g_inc_cache_ttl && (unsigned long)(now - e->last_access) > g_inc_cache_ttl) {
                /* expired: remove */
                /* unlink from bucket */
                struct IncCacheEntry **pp = &g_inc_cache[h];
                while (*pp && *pp != e) pp = &(*pp)->next;
                if (*pp == e) *pp = e->next;
                /* unlink from LRU */
                lru_unlink(e);
                free(e->key); free(e);
                g_inc_cache_count--;
                pthread_mutex_unlock(&g_inc_cache_lock);
                return 0;
            }
            e->last_access = now;
            lru_move_to_head(e);
            *out = e->val;
            pthread_mutex_unlock(&g_inc_cache_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_inc_cache_lock);
    return 0;
}

static void inc_cache_set(const char *key, enum HLType val) {
    pthread_mutex_lock(&g_inc_cache_lock);
    unsigned long h = djb2_hash(key) % INC_CACHE_BUCKETS;
    for (struct IncCacheEntry *e = g_inc_cache[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            e->val = val; e->last_access = time(NULL);
            lru_move_to_head(e);
            pthread_mutex_unlock(&g_inc_cache_lock);
            return;
        }
    }
    struct IncCacheEntry *n = malloc(sizeof(*n));
    n->key = strdup(key);
    n->val = val;
    n->next = g_inc_cache[h];
    g_inc_cache[h] = n;
    n->last_access = time(NULL);
    n->prev = n->lru_next = NULL;
    lru_insert_head(n);
    g_inc_cache_count++;
    /* Evict if over capacity */
    if (g_inc_cache_max_entries && g_inc_cache_count > g_inc_cache_max_entries) {
        struct IncCacheEntry *ev = g_inc_lru_tail;
        if (ev) {
            unsigned long eh = djb2_hash(ev->key) % INC_CACHE_BUCKETS;
            struct IncCacheEntry **pp = &g_inc_cache[eh];
            while (*pp && *pp != ev) pp = &(*pp)->next;
            if (*pp == ev) *pp = ev->next;
            lru_unlink(ev);
            free(ev->key); free(ev);
            g_inc_cache_count--;
        }
    }
    pthread_mutex_unlock(&g_inc_cache_lock);

    /* Notify update callback or signal if configured. Callbacks are invoked
     * after releasing the cache lock to avoid deadlocks. */
    if (g_update_cb) g_update_cb(key, val, g_update_cb_user);
    if (g_update_signal) kill(getpid(), g_update_signal);
}


void include_cache_register_update_callback(void (*cb)(const char *include, enum HLType val, void *userdata), void *userdata) {
    g_update_cb = cb; g_update_cb_user = userdata;
}

void include_cache_enable_update_signal(int signo) {
    g_update_signal = signo;
}

void include_cache_disable_update_signal(void) {
    g_update_signal = 0;
}

static void inc_cache_clear(void) {
    pthread_mutex_lock(&g_inc_cache_lock);
    for (int i = 0; i < INC_CACHE_BUCKETS; i++) {
        struct IncCacheEntry *e = g_inc_cache[i];
        while (e) { struct IncCacheEntry *nx = e->next; free(e->key); free(e); e = nx; }
        g_inc_cache[i] = NULL;
    }
    g_inc_lru_head = g_inc_lru_tail = NULL;
    g_inc_cache_count = 0;
    pthread_mutex_unlock(&g_inc_cache_lock);
}

/* Persistence format (binary):
 *  - magic "INCCACHE" (8 bytes)
 *  - uint32_t version (1)
 *  - uint32_t entry_count
 *  For each entry in MRU->LRU order:
 *    - uint32_t keylen
 *    - uint8_t val
 *    - uint64_t last_access
 *    - raw key bytes (no nul)
 */

int include_cache_enable_persistence(const char *path) {
    if (!path) return -1;
    pthread_mutex_lock(&g_inc_cache_lock);
    free(g_persist_path);
    g_persist_path = strdup(path);
    g_persist_enabled = 1;
    pthread_mutex_unlock(&g_inc_cache_lock);
    /* load existing */
    return include_cache_load_from_file();
}

static int write_u32(FILE *f, uint32_t v) { return fwrite(&v, sizeof(v), 1, f) == 1 ? 0 : -1; }
static int write_u64(FILE *f, uint64_t v) { return fwrite(&v, sizeof(v), 1, f) == 1 ? 0 : -1; }

int include_cache_save_to_file(void) {
    if (!g_persist_enabled || !g_persist_path) return -1;
    char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s.tmp", g_persist_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite("INCCACHE", 1, 8, f) != 8) { fclose(f); return -1; }
    if (write_u32(f, 1) != 0) { fclose(f); return -1; }
    /* count entries */
    pthread_mutex_lock(&g_inc_cache_lock);
    uint32_t cnt = 0; for (struct IncCacheEntry *e = g_inc_lru_head; e; e = e->lru_next) cnt++;
    write_u32(f, cnt);
    for (struct IncCacheEntry *e = g_inc_lru_head; e; e = e->lru_next) {
        uint32_t klen = (uint32_t)strlen(e->key);
        uint8_t val = (uint8_t)e->val;
        uint64_t ts = (uint64_t)e->last_access;
        write_u32(f, klen);
        if (fwrite(&val, 1, 1, f) != 1) { pthread_mutex_unlock(&g_inc_cache_lock); fclose(f); return -1; }
        write_u64(f, ts);
        if (fwrite(e->key, 1, klen, f) != klen) { pthread_mutex_unlock(&g_inc_cache_lock); fclose(f); return -1; }
    }
    pthread_mutex_unlock(&g_inc_cache_lock);
    fclose(f);
    /* atomic rename */
    if (rename(tmp, g_persist_path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int include_cache_load_from_file(void) {
    if (!g_persist_enabled || !g_persist_path) return -1;
    FILE *f = fopen(g_persist_path, "rb");
    if (!f) return -1;
    char mag[9] = {0}; if (fread(mag, 1, 8, f) != 8) { fclose(f); return -1; }
    if (strncmp(mag, "INCCACHE", 8) != 0) { fclose(f); return -1; }
    uint32_t ver = 0; if (fread(&ver, sizeof(ver), 1, f) != 1) { fclose(f); return -1; }
    uint32_t cnt = 0; if (fread(&cnt, sizeof(cnt), 1, f) != 1) { fclose(f); return -1; }
    pthread_mutex_lock(&g_inc_cache_lock);
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t klen = 0; if (fread(&klen, sizeof(klen), 1, f) != 1) break;
        uint8_t val = 0; if (fread(&val, 1, 1, f) != 1) break;
        uint64_t ts = 0; if (fread(&ts, sizeof(ts), 1, f) != 1) break;
        char *k = malloc(klen + 1); if (fread(k, 1, klen, f) != klen) { free(k); break; }
        k[klen] = '\0';
        /* respect TTL */
        time_t now = time(NULL);
        if (g_inc_cache_ttl && (unsigned long)(now - (time_t)ts) > g_inc_cache_ttl) { free(k); continue; }
        /* insert into cache (MRU order) */
        struct IncCacheEntry *n = malloc(sizeof(*n)); n->key = k; n->val = (enum HLType)val; n->next = NULL; n->last_access = (time_t)ts; n->prev = n->lru_next = NULL;
        unsigned long h = djb2_hash(n->key) % INC_CACHE_BUCKETS;
        n->next = g_inc_cache[h]; g_inc_cache[h] = n; lru_insert_head(n); g_inc_cache_count++;
        if (g_inc_cache_max_entries && g_inc_cache_count > g_inc_cache_max_entries) {
            struct IncCacheEntry *ev = g_inc_lru_tail;
            if (ev) {
                unsigned long eh = djb2_hash(ev->key) % INC_CACHE_BUCKETS;
                struct IncCacheEntry **pp = &g_inc_cache[eh];
                while (*pp && *pp != ev) pp = &(*pp)->next;
                if (*pp == ev) *pp = ev->next;
                lru_unlink(ev); free(ev->key); free(ev); g_inc_cache_count--;
            }
        }
    }
    pthread_mutex_unlock(&g_inc_cache_lock);
    fclose(f);
    return 0;
}

/* Register cache cleanup at exit */
static void __attribute__((constructor)) init_inc_cache(void) { atexit(inc_cache_clear); }

/* Public/test wrappers */
void include_cache_clear(void) { inc_cache_clear(); }

size_t include_cache_size(void) {
    return g_inc_cache_count;
}

void include_cache_set_ttl_seconds(unsigned int sec) { g_inc_cache_ttl = sec; }
void include_cache_set_max_entries(size_t max) { g_inc_cache_max_entries = max; }
/* Minimal helper classifiers used by tests and higher-level logic. These are lightweight
 * heuristics that avoid expensive probes for typical small inputs. More heavyweight
 * probing remains in the main classifier for languages where it's needed.
 */
static enum HLType classify_c_include(const char *path, size_t len) {
    if (!path) return HL_INCLUDE_THIRD_PARTY;
    for (size_t i = 0; i < len; i++) if (path[i] == '/') return HL_INCLUDE_THIRD_PARTY;
    const char *c_std[] = { "stdio.h", "stdlib.h", "string.h", "stddef.h", "stdint.h", "inttypes.h", "time.h", "unistd.h", NULL };
    for (int i = 0; c_std[i]; i++) { size_t l = strlen(c_std[i]); if (len == l && strncmp(path, c_std[i], l) == 0) return HL_INCLUDE_SYSTEM; }
    return HL_INCLUDE_SYSTEM;
}

static enum HLType classify_python_include(const char *path, size_t len);
static int is_go_standard(const char *path, size_t len);

static enum HLType classify_node_include(const char *path, size_t len) {
    if (!path) return HL_INCLUDE_THIRD_PARTY;
    const char *node_core[] = { "fs", "path", "http", "https", "util", "events", "stream", "net", "os", "tls", NULL };
    for (int i = 0; node_core[i]; i++) { size_t l = strlen(node_core[i]); if (len == l && strncmp(path, node_core[i], l) == 0) return HL_INCLUDE_SYSTEM; }
    return HL_INCLUDE_THIRD_PARTY;
}

/* compute classification without touching the cache (used by sync and async paths) */
static enum HLType compute_classify(const char *lang, const char *path, size_t len) {
    if (!lang || !path) return HL_PREPROC;
    /* Test hook for async tests */
    if (strcmp(lang, "asynctest") == 0) { sleep(1); return HL_INCLUDE_THIRD_PARTY; }
    if (strcmp(lang, "c") == 0 || strcmp(lang, "cpp") == 0 || strcmp(lang, "c++") == 0) {
        return classify_c_include(path, len);
    }
    if (strcmp(lang, "python") == 0) return classify_python_include(path, len);
    if (strcmp(lang, "node") == 0 || strcmp(lang, "javascript") == 0 || strcmp(lang, "js") == 0 || strcmp(lang, "typescript") == 0 || strcmp(lang, "ts") == 0) return classify_node_include(path, len);
    if (strcmp(lang, "java") == 0 || strcmp(lang, "kotlin") == 0 || strcmp(lang, "scala") == 0) {
        if (len >= 5 && strncmp(path, "java.", 5) == 0) return HL_INCLUDE_SYSTEM;
        if (len >= 6 && strncmp(path, "javax.", 6) == 0) return HL_INCLUDE_SYSTEM;
        return HL_INCLUDE_THIRD_PARTY;
    }
    if (strcmp(lang, "csharp") == 0 || strcmp(lang, "cs") == 0 || strcmp(lang, "dotnet") == 0) {
        if (len >= 7 && strncmp(path, "System.", 7) == 0) return HL_INCLUDE_SYSTEM;
        if (len >= 10 && strncmp(path, "Microsoft.", 10) == 0) return HL_INCLUDE_SYSTEM;
        return HL_INCLUDE_THIRD_PARTY;
    }
    /* Reuse the rest of the existing heuristics... */
    /* PHP, Ruby, Perl, Lua heuristic classifiers (curated std lists) */
    if (strcmp(lang, "php") == 0) {
        const char *php_std[] = { "PDO", "mysqli", "json", "SPL", "DateTime", "ZipArchive", NULL };
        for (int i = 0; php_std[i]; i++) { size_t l = strlen(php_std[i]); if (len == l && strncmp(path, php_std[i], l) == 0) return HL_INCLUDE_SYSTEM; }
        return HL_INCLUDE_THIRD_PARTY;
    }
    if (strcmp(lang, "ruby") == 0) {
        const char *ruby_std[] = { "json", "set", "thread", "socket", "digest", "fileutils", NULL };
        for (int i = 0; ruby_std[i]; i++) { size_t l = strlen(ruby_std[i]); if (len >= l && strncmp(path, ruby_std[i], l) == 0) return HL_INCLUDE_SYSTEM; }
        return HL_INCLUDE_THIRD_PARTY;
    }
    if (strcmp(lang, "perl") == 0) {
        const char *perl_std[] = { "strict", "warnings", "File::Spec", "Getopt::Long", "IO::Handle", NULL };
        for (int i = 0; perl_std[i]; i++) { size_t l = strlen(perl_std[i]); if (len == l && strncmp(path, perl_std[i], l) == 0) return HL_INCLUDE_SYSTEM; }
        return HL_INCLUDE_THIRD_PARTY;
    }
    if (strcmp(lang, "lua") == 0) {
        const char *lua_std[] = { "string", "table", "math", "os", "io", "coroutine", "package", NULL };
        for (int i = 0; lua_std[i]; i++) { size_t l = strlen(lua_std[i]); if (len == l && strncmp(path, lua_std[i], l) == 0) return HL_INCLUDE_SYSTEM; }
        return HL_INCLUDE_THIRD_PARTY;
    }
    if (strcmp(lang, "go") == 0) {
        if (is_go_standard(path, len)) return HL_INCLUDE_SYSTEM;
        char mod[512]; size_t mlen2 = len < sizeof(mod)-1 ? len : sizeof(mod)-1; memcpy(mod, path, mlen2); mod[mlen2] = '\0';
        char cmd[512]; snprintf(cmd, sizeof(cmd), "go list -f '{{.Standard}}' \"%s\" 2>/dev/null", mod);
        FILE *fp = popen(cmd, "r"); if (!fp) return HL_INCLUDE_THIRD_PARTY;
        char out[128]; int ok = 0; if (fgets(out, sizeof(out), fp)) { if (strstr(out, "true")) ok = 1; }
        pclose(fp);
        if (ok) return HL_INCLUDE_SYSTEM; return HL_INCLUDE_THIRD_PARTY;
    }
    if (strcmp(lang, "rust") == 0) {
        if (len == 3 && strncmp(path, "std", 3) == 0) return HL_INCLUDE_SYSTEM;
        if (len == 4 && strncmp(path, "core", 4) == 0) return HL_INCLUDE_SYSTEM;
        char mod[256]; size_t mlen2 = len < sizeof(mod)-1 ? len : sizeof(mod)-1; memcpy(mod, path, mlen2); mod[mlen2] = '\0';
        char cmd[512]; snprintf(cmd, sizeof(cmd), "cargo search %s --limit 1 2>/dev/null", mod);
        FILE *fp = popen(cmd, "r"); if (!fp) return HL_INCLUDE_THIRD_PARTY;
        char out[512]; int found = 0; if (fgets(out, sizeof(out), fp)) { if (strstr(out, "=") && strstr(out, mod)) found = 1; }
        pclose(fp);
        return found ? HL_INCLUDE_THIRD_PARTY : HL_INCLUDE_THIRD_PARTY;
    }
    /* The remainder of languages use existing heuristics from earlier implementation */
    /* For brevity reuse classify_include by falling back to the original code path (non-cached) */
    /* We'll handle rest by calling classify_include recursively only when safe — but to avoid recursion, implement remaining heuristics inline. */
    /* Fallback heuristic: slashes => third-party */
    for (size_t i = 0; i < len; i++) if (path[i] == '/') return HL_INCLUDE_THIRD_PARTY;
    return HL_INCLUDE_SYSTEM;
}

/* Async worker queue */
struct AsyncJob { char *key; char *lang; char *path; size_t len; struct AsyncJob *next; };
static struct AsyncJob *g_job_head = NULL;
static struct AsyncJob *g_job_tail = NULL;
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;

static void *async_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_job_lock);
    while (1) {
        while (!g_job_head && g_async_enabled) pthread_cond_wait(&g_job_cond, &g_job_lock);
        if (!g_async_enabled && !g_job_head) { pthread_mutex_unlock(&g_job_lock); return NULL; }
        struct AsyncJob *job = g_job_head; if (job) { g_job_head = job->next; if (!g_job_head) g_job_tail = NULL; }
        pthread_mutex_unlock(&g_job_lock);
        if (job) {
            enum HLType r = compute_classify(job->lang, job->path, job->len);
            inc_cache_set(job->key, r);
            free(job->key); free(job->lang); free(job->path); free(job);
        }
        pthread_mutex_lock(&g_job_lock);
    }
}

int include_cache_enable_async(void) {
    if (g_async_enabled) return 0;
    g_async_enabled = 1;
    if (pthread_create(&g_async_thread, NULL, async_worker, NULL) != 0) { g_async_enabled = 0; return -1; }
    g_async_thread_running = 1;
    return 0;
}

void include_cache_shutdown_async(void) {
    if (!g_async_enabled) return;
    pthread_mutex_lock(&g_job_lock);
    g_async_enabled = 0;
    pthread_cond_signal(&g_job_cond);
    pthread_mutex_unlock(&g_job_lock);
    if (g_async_thread_running) pthread_join(g_async_thread, NULL);
    g_async_thread_running = 0;
}

enum HLType classify_include_async(const char *lang, const char *path, size_t len) {
    if (!lang || !path) return HL_PREPROC;
    char pbuf[1024]; size_t mlen = len < sizeof(pbuf)-1 ? len : sizeof(pbuf)-1; memcpy(pbuf, path, mlen); pbuf[mlen] = '\0';
    char key[1150]; snprintf(key, sizeof(key), "%s|%s", lang, pbuf);
    enum HLType cached;
    if (inc_cache_get(key, &cached)) return cached;
    /* enqueue job if async worker enabled */
    if (g_async_enabled) {
        struct AsyncJob *job = malloc(sizeof(*job));
        job->key = strdup(key);
        job->lang = strdup(lang);
        job->path = strdup(pbuf);
        job->len = strlen(pbuf);
        job->next = NULL;
        pthread_mutex_lock(&g_job_lock);
        if (g_job_tail) g_job_tail->next = job; else g_job_head = job; g_job_tail = job;
        pthread_cond_signal(&g_job_cond);
        pthread_mutex_unlock(&g_job_lock);
    }
    return HL_INCLUDE_THIRD_PARTY; /* fallback immediate */
}


static enum HLType classify_python_include(const char *path, size_t len) {
    if (!path) return HL_INCLUDE_THIRD_PARTY;
    const char *py_std[] = { "os", "sys", "re", "json", "math", "itertools", "subprocess", "typing", NULL };
    for (int i = 0; py_std[i]; i++) { size_t l = strlen(py_std[i]); if (len == l && strncmp(path, py_std[i], l) == 0) return HL_INCLUDE_SYSTEM; }
    return HL_INCLUDE_THIRD_PARTY;
}

static int is_go_standard(const char *path, size_t len) {
    if (!path) return 0;
    const char *go_std[] = { "fmt", "io", "net", "strings", "os", "path", "sort", "strconv", NULL };
    for (int i = 0; go_std[i]; i++) { size_t l = strlen(go_std[i]); if (len == l && strncmp(path, go_std[i], l) == 0) return 1; }
    return 0;
}

enum HLType classify_include(const char *lang, const char *path, size_t len) {
    if (!lang || !path) return HL_PREPROC;

    /* Build a stable cache key: "lang|path" */
    char pbuf[1024]; size_t mlen = len < sizeof(pbuf)-1 ? len : sizeof(pbuf)-1; memcpy(pbuf, path, mlen); pbuf[mlen] = '\0';
    char key[1150]; snprintf(key, sizeof(key), "%s|%s", lang, pbuf);
    enum HLType cached;
    if (inc_cache_get(key, &cached)) return cached;

    enum HLType res = HL_INCLUDE_THIRD_PARTY;

    if (strcmp(lang, "c") == 0 || strcmp(lang, "cpp") == 0 || strcmp(lang, "c++") == 0) {
        res = classify_c_include(path, len);
        goto done;
    }
    if (strcmp(lang, "python") == 0) { res = classify_python_include(path, len); goto done; }
    if (strcmp(lang, "node") == 0 || strcmp(lang, "javascript") == 0 || strcmp(lang, "js") == 0 || strcmp(lang, "typescript") == 0 || strcmp(lang, "ts") == 0) { res = classify_node_include(path, len); goto done; }
    if (strcmp(lang, "java") == 0 || strcmp(lang, "kotlin") == 0 || strcmp(lang, "scala") == 0) {
        /* Java/JVM: packages starting with java. or javax. are system */
        if (len >= 5 && strncmp(path, "java.", 5) == 0) res = HL_INCLUDE_SYSTEM;
        else if (len >= 6 && strncmp(path, "javax.", 6) == 0) res = HL_INCLUDE_SYSTEM;
        else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "csharp") == 0 || strcmp(lang, "cs") == 0 || strcmp(lang, "dotnet") == 0) {
        /* C#/.NET: System.* and Microsoft.* are framework */
        if (len >= 7 && strncmp(path, "System.", 7) == 0) res = HL_INCLUDE_SYSTEM;
        else if (len >= 10 && strncmp(path, "Microsoft.", 10) == 0) res = HL_INCLUDE_SYSTEM;
        else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }

    /* PHP, Ruby, Perl, Lua heuristic classifiers (curated std lists) */
    if (strcmp(lang, "php") == 0) {
        /* simple curated set of known builtin extensions/classes */
        const char *php_std[] = { "PDO", "mysqli", "json", "SPL", "DateTime", "ZipArchive", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; php_std[i]; i++) { size_t l = strlen(php_std[i]); if (len == l && strncmp(path, php_std[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "ruby") == 0) {
        const char *ruby_std[] = { "json", "set", "thread", "socket", "digest", "fileutils", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; ruby_std[i]; i++) { size_t l = strlen(ruby_std[i]); if (len >= l && strncmp(path, ruby_std[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "perl") == 0) {
        const char *perl_std[] = { "strict", "warnings", "File::Spec", "Getopt::Long", "IO::Handle", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; perl_std[i]; i++) { size_t l = strlen(perl_std[i]); if (len == l && strncmp(path, perl_std[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "lua") == 0) {
        const char *lua_std[] = { "string", "table", "math", "os", "io", "coroutine", "package", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; lua_std[i]; i++) { size_t l = strlen(lua_std[i]); if (len == l && strncmp(path, lua_std[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "go") == 0) {
        /* quick check against curated std package list */
        if (is_go_standard(path, len)) { res = HL_INCLUDE_SYSTEM; goto done; }
        /* Go package: use `go list -f '{{.Standard}}' pkg` to detect stdlib */
        char mod[512]; size_t mlen2 = len < sizeof(mod)-1 ? len : sizeof(mod)-1; memcpy(mod, path, mlen2); mod[mlen2] = '\0';
        char cmd[512]; snprintf(cmd, sizeof(cmd), "go list -f '{{.Standard}}' \"%s\" 2>/dev/null", mod);
        FILE *fp = popen(cmd, "r");
        if (!fp) { res = HL_INCLUDE_THIRD_PARTY; goto done; }
        char out[128]; int ok = 0; if (fgets(out, sizeof(out), fp)) { if (strstr(out, "true")) ok = 1; }
        pclose(fp);
        if (ok) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "rust") == 0) {
        /* Rust crate: builtin crates like 'std' or 'core' are SYSTEM; otherwise probe crates.io via `cargo search` */
        if (len == 3 && strncmp(path, "std", 3) == 0) { res = HL_INCLUDE_SYSTEM; goto done; }
        if (len == 4 && strncmp(path, "core", 4) == 0) { res = HL_INCLUDE_SYSTEM; goto done; }
        char mod[256]; size_t mlen2 = len < sizeof(mod)-1 ? len : sizeof(mod)-1; memcpy(mod, path, mlen2); mod[mlen2] = '\0';
        char cmd[512]; snprintf(cmd, sizeof(cmd), "cargo search %s --limit 1 2>/dev/null", mod);
        FILE *fp = popen(cmd, "r"); if (!fp) { res = HL_INCLUDE_THIRD_PARTY; goto done; }
        char out[512]; int found = 0; if (fgets(out, sizeof(out), fp)) { if (strstr(out, "=") && strstr(out, mod)) found = 1; }
        pclose(fp);
        if (found) res = HL_INCLUDE_THIRD_PARTY; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "swift") == 0) {
        /* Swift: Foundation, Swift, and Darwin frameworks are system */
        if (len >= 6 && (strncmp(path, "Foundation", 10) == 0 || strncmp(path, "Swift", 5) == 0)) res = HL_INCLUDE_SYSTEM;
        else if (len >= 6 && (strncmp(path, "Darwin", 6) == 0 || strncmp(path, "UIKit", 5) == 0 || strncmp(path, "AppKit", 6) == 0)) res = HL_INCLUDE_SYSTEM;
        else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "objective-c") == 0 || strcmp(lang, "objc") == 0) {
        /* Objective-C frameworks */
        if (strstr(path, "Foundation") || strstr(path, "UIKit") || strstr(path, "AppKit") || strstr(path, "CoreFoundation")) res = HL_INCLUDE_SYSTEM;
        else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "r") == 0) {
        /* Try Rscript to determine whether a package is base/recommended */
        char mod[256]; size_t mlen2 = len < sizeof(mod)-1 ? len : sizeof(mod)-1; memcpy(mod, path, mlen2); mod[mlen2] = '\0';
        char cmd[512]; snprintf(cmd, sizeof(cmd), "Rscript -e \"pkg='%s'; ip=installed.packages(priority='base'); if (pkg %%in%% rownames(ip)) cat('BASE') else if (requireNamespace(pkg, quietly=TRUE)) cat('INSTALLED') else cat('NONE')\" 2>/dev/null", mod);
        FILE *fp = popen(cmd, "r"); if (!fp) { res = HL_INCLUDE_THIRD_PARTY; goto done; }
        char out[128]; if (!fgets(out, sizeof(out), fp)) { pclose(fp); res = HL_INCLUDE_THIRD_PARTY; goto done; }
        pclose(fp);
        if (strncmp(out, "BASE", 4) == 0) res = HL_INCLUDE_SYSTEM; else if (strncmp(out, "INSTALLED", 9) == 0) res = HL_INCLUDE_THIRD_PARTY; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "matlab") == 0) {
        /* heuristic curated list for core MATLAB functions */
        const char *matlab_std[] = { "plot", "sin", "cos", "disp", "fft", "mean", "sum", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; matlab_std[i]; i++) { size_t l = strlen(matlab_std[i]); if (len == l && strncmp(path, matlab_std[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }

    if (strcmp(lang, "delphi") == 0 || strcmp(lang, "pascal") == 0 || strcmp(lang, "objectpascal") == 0) {
        /* Delphi / Object Pascal: common RTL/VCL units are system */
        const char *d_units[] = { "SysUtils", "Classes", "Windows", "SysInit", "Types", "Math", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; d_units[i]; i++) { size_t l = strlen(d_units[i]); if (len == l && strncmp(path, d_units[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "fortran") == 0) {
        /* Fortran: intrinsic modules */
        const char *f_intr[] = { "iso_c_binding", "iso_fortran_env", "ieee_arithmetic", "iso_cs", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; f_intr[i]; i++) { size_t l = strlen(f_intr[i]); if (len == l && strncmp(path, f_intr[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "ada") == 0) {
        /* Ada: core packages */
        const char *a_core[] = { "Ada.Text_IO", "Ada.Integer_Text_IO", "System", "Ada.Strings.Unbounded", NULL };
        res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; a_core[i]; i++) { size_t l = strlen(a_core[i]); if (len >= l && strncmp(path, a_core[i], l) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }
    if (strcmp(lang, "julia") == 0) {
        /* Julia: Base and Core are system */
        if ((len == 4 && strncmp(path, "Base", 4) == 0) || (len == 4 && strncmp(path, "Core", 4) == 0)) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "haskell") == 0) {
        /* Haskell: Prelude is system */
        if (len == 7 && strncmp(path, "Prelude", 7) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "visualbasic") == 0 || strcmp(lang, "vb") == 0) {
        /* Visual Basic: Microsoft namespaces are system */
        if (len >= 10 && (strncmp(path, "Microsoft", 9) == 0 || strncmp(path, "VB", 2) == 0)) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "sql") == 0) {
        /* SQL: functions vs extensions are difficult; fallback: treat catalog names like 'pg_' or 'mysql_' as third-party */
        if (strncmp(path, "pg_", 3) == 0 || strncmp(path, "mysql_", 6) == 0) res = HL_INCLUDE_THIRD_PARTY; else res = HL_INCLUDE_SYSTEM;
        goto done;
    }
    /* Remaining top-50 heuristics */
    if (strcmp(lang, "dart") == 0) {
        /* dart:core, dart:io, dart:async are system */
        if (len > 5 && strncmp(path, "dart:", 5) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "elixir") == 0) {
        /* Elixir: Kernel, Enum, Mix, Application are system */
        if (len >= 4 && (strncmp(path, "Kernel", 6) == 0 || strncmp(path, "Enum", 4) == 0 || strncmp(path, "Mix", 3) == 0)) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "solidity") == 0) {
        /* Solidity: imports with @ or from openzeppelin imply third-party */
        if (strchr(path, '@') || strstr(path, "openzeppelin")) res = HL_INCLUDE_THIRD_PARTY; else res = HL_INCLUDE_SYSTEM;
        goto done;
    }
    if (strcmp(lang, "powershell") == 0 || strcmp(lang, "ps") == 0) {
        if (len >= 7 && (strncmp(path, "Microsoft", 9) == 0 || strncmp(path, "PSScriptTools", 12) == 0)) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "bash") == 0 || strcmp(lang, "shell") == 0) {
        /* Bash: scripts sourced with ./ or / are local, builtin commands considered system via small list */
        if (len >= 2 && (path[0] == '.' || path[0] == '/')) { res = HL_INCLUDE_LOCAL; goto done; }
        res = HL_INCLUDE_THIRD_PARTY;
        const char *bash_builtins[] = {"echo","printf","test","[", "cd","pwd","type","command", NULL};
        for (int i = 0; bash_builtins[i]; i++) { if (strcmp(path, bash_builtins[i]) == 0) { res = HL_INCLUDE_SYSTEM; break; } }
        goto done;
    }

    /* More top-50 language heuristics (COBOL, Prolog, SAS, Lisp, FoxPro, ABAP, VBScript, Ladder, Zig, Apex, LabVIEW, Wolfram, Erlang, ML, RPG, Assembly, Scratch) */
    if (strcmp(lang, "cobol") == 0) {
        /* COBOL: intrinsic libraries (e.g., 'ibm', 'gnucobol' artifacts) are third-party, treat default as system */
        if (strstr(path, "ibm") || strstr(path, "gnucobol")) res = HL_INCLUDE_THIRD_PARTY; else res = HL_INCLUDE_SYSTEM;
        goto done;
    }
    if (strcmp(lang, "prolog") == 0) {
        /* Prolog: core libraries like 'lists', 'apply' are system */
        const char *pl_std[] = {"lists","apply","system","io","strings", NULL}; res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; pl_std[i]; i++) if (strcmp(path, pl_std[i]) == 0) { res = HL_INCLUDE_SYSTEM; break; }
        goto done;
    }
    if (strcmp(lang, "sas") == 0) {
        /* SAS: built-in procedures are system */
        if (strncmp(path, "proc.", 5) == 0 || strncmp(path, "data.", 5) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "lisp") == 0 || strcmp(lang, "common-lisp") == 0) {
        const char *lisp_std[] = {"cl","alexandria","sb-introspect", NULL}; res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; lisp_std[i]; i++) if (strcmp(path, lisp_std[i]) == 0) { res = HL_INCLUDE_SYSTEM; break; }
        goto done;
    }
    if (strcmp(lang, "foxpro") == 0 || strcmp(lang, "visualfoxpro") == 0) {
        /* treat VFP-specific includes as system */
        if (strstr(path, "VFP") || strncmp(path, "vfp", 3) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "abap") == 0) {
        /* ABAP: SAP standard namespaces */
        if (strstr(path, "SAP") || strncmp(path, "CL_", 3) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "vbscript") == 0) {
        if (strstr(path, "Microsoft") || strncmp(path, "VBScript", 8) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "ladder") == 0 || strcmp(lang, "ladderlogic") == 0) { res = HL_INCLUDE_THIRD_PARTY; goto done; }
    if (strcmp(lang, "zig") == 0) {
        /* Zig: std is 'std' */
        if (len == 3 && strncmp(path, "std", 3) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "apex") == 0) {
        /* Salesforce Apex: system namespaces start with 'System.' */
        if (len >= 7 && strncmp(path, "System.", 7) == 0) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "labview") == 0) { res = HL_INCLUDE_THIRD_PARTY; goto done; }
    if (strcmp(lang, "wolfram") == 0 || strcmp(lang, "mathematica") == 0) {
        /* core symbols: System`*` patterns */
        if (strstr(path, "System`")) res = HL_INCLUDE_SYSTEM; else res = HL_INCLUDE_THIRD_PARTY;
        goto done;
    }
    if (strcmp(lang, "erlang") == 0) {
        /* Erlang core modules */
        const char *erlang_std[] = {"lists","io","erl_eval","string", NULL}; res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; erlang_std[i]; i++) if (strcmp(path, erlang_std[i]) == 0) { res = HL_INCLUDE_SYSTEM; break; }
        goto done;
    }
    if (strcmp(lang, "ml") == 0 || strcmp(lang, "ocaml") == 0) {
        const char *ml_std[] = {"Pervasives","List","Array","String", NULL}; res = HL_INCLUDE_THIRD_PARTY;
        for (int i = 0; ml_std[i]; i++) if (strcmp(path, ml_std[i]) == 0) { res = HL_INCLUDE_SYSTEM; break; }
        goto done;
    }
    if (strcmp(lang, "rpg") == 0) { res = HL_INCLUDE_THIRD_PARTY; goto done; }
    if (strcmp(lang, "assembly") == 0 || strcmp(lang, "asm") == 0) { res = HL_INCLUDE_SYSTEM; goto done; }
    if (strcmp(lang, "scratch") == 0) { res = HL_INCLUDE_THIRD_PARTY; goto done; }

    /* fallback heuristic: slashes => third-party, quotes/local handled by caller */
    res = HL_INCLUDE_SYSTEM;
    for (size_t i = 0; i < len; i++) if (path[i] == '/') { res = HL_INCLUDE_THIRD_PARTY; break; }

done:
    inc_cache_set(key, res);
    return res;
}