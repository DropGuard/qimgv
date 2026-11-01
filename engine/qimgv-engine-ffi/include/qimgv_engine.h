/* C ABI for qimgv-engine. Mirrors engine/qimgv-engine-ffi/src/lib.rs.
 *
 * Contract:
 *  - All calls must come from a single thread (the Qt main thread).
 *  - No panic ever crosses this boundary; failures return negative/enum codes
 *    and qimgv_last_error() yields a thread-local diagnostic message valid
 *    until the next engine call on the same thread.
 *  - String out-buffers are filled with NUL-terminated UTF-8;
 *    QIMGV_ERR_TRUNCATED signals an undersized buffer.
 *  - `notify` is invoked from an engine thread after each event is queued.
 *    It must not call back into the engine and must not touch Qt objects —
 *    its job is to wake the Qt event loop (e.g. write to an eventfd).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qimgv_engine qimgv_engine;

/* status codes */
#define QIMGV_OK             0
#define QIMGV_ERR_IO         1
#define QIMGV_ERR_ENGINE_GONE 2
#define QIMGV_ERR_INVALID    3
#define QIMGV_ERR_PANIC      4
#define QIMGV_ERR_EMPTY      5
#define QIMGV_ERR_TRUNCATED  6
#define QIMGV_ERR_BAD_SORT   7
#define QIMGV_ERR_BAD_UTF8   8

/* event kinds (qimgv_event.kind) */
#define QIMGV_EVENT_NONE        0
#define QIMGV_EVENT_DIR_LISTED  1
#define QIMGV_EVENT_OPEN_FAILED 2
#define QIMGV_EVENT_DIR_MUTATED 3

/* mutation kinds (qimgv_mutation.kind) */
#define QIMGV_MUT_ADDED    1
#define QIMGV_MUT_MODIFIED 2
#define QIMGV_MUT_REMOVED  3

/* image formats (qimgv_probe_result.format) */
#define QIMGV_FMT_UNKNOWN 0
#define QIMGV_FMT_JPEG    1
#define QIMGV_FMT_PNG     2
#define QIMGV_FMT_GIF     3
#define QIMGV_FMT_WEBP    4
#define QIMGV_FMT_AVIF    5
#define QIMGV_FMT_JXL     6
#define QIMGV_FMT_BMP     7
#define QIMGV_FMT_TIFF    8

/* sort modes (maps qimgv's SortingMode; bridge maps explicitly) */
#define QIMGV_SORT_NAME       0
#define QIMGV_SORT_NAME_DESC  1
#define QIMGV_SORT_TIME       2
#define QIMGV_SORT_TIME_DESC  3
#define QIMGV_SORT_SIZE       4
#define QIMGV_SORT_SIZE_DESC  5

typedef struct qimgv_entry_meta {
    uint64_t handle;   /* opaque; valid until the next DirListed event */
    uint64_t size;
    int64_t mtime_ms;  /* ms since Unix epoch */
    int is_dir;
} qimgv_entry_meta;

typedef struct qimgv_event {
    int kind;          /* QIMGV_EVENT_* */
    int code;          /* QIMGV_ERR_* (QIMGV_OK for DIR_LISTED) */
    uint32_t epoch;    /* session; drop events older than your open_dir epoch */
} qimgv_event;

typedef struct qimgv_mutation {
    int32_t kind;      /* QIMGV_MUT_* */
    int32_t is_dir;
    uint64_t size;
    int64_t mtime_ms;
} qimgv_mutation;

typedef struct qimgv_probe_result {
    int32_t format;      /* QIMGV_FMT_* */
    int32_t animated;    /* 0/1 */
    int32_t orientation; /* EXIF 1..=8; 1 = normal/unknown */
    int32_t width;       /* 0 = unknown */
    int32_t height;
} qimgv_probe_result;

typedef void (*qimgv_notify_fn)(void *user);

const char *qimgv_engine_version(void);
const char *qimgv_last_error(void);

/* `formats_extensions_utf8`: '\n'-joined extension whitelist ("jpg\npng") or
 * NULL to accept everything. Sourced from Settings::supportedFormats(). */
qimgv_engine *qimgv_engine_new(qimgv_notify_fn notify, void *user,
                               int scan_threads,
                               const char *formats_extensions_utf8);
void qimgv_engine_free(qimgv_engine *engine);

/* Commands — return QIMGV_OK when accepted (asynchronous; results arrive as
 * events). */
/* sort_mode: QIMGV_SORT_* applied atomically with the scan, or -1 to keep
 * the engine's current sort. */
/* epoch_out (nullable) receives the session id for this open; events with a
 * smaller epoch belong to older sessions and should be dropped. */
int qimgv_engine_open_dir(qimgv_engine *engine, const char *path_utf8, int sort_mode,
                          uint32_t *epoch_out);

/* 1 when the engine watches directories itself (Linux) and DIR_MUTATED
 * events will fire; 0 means the caller keeps its platform watcher. */
int qimgv_engine_supports_watching(void);
int qimgv_engine_include_path(qimgv_engine *engine, const char *path_utf8);
int qimgv_engine_stop_watch(qimgv_engine *engine);
int qimgv_engine_set_sort(qimgv_engine *engine, int mode);
int qimgv_engine_set_show_hidden(qimgv_engine *engine, int show);

/* State reads — synchronous. count() returns -1 on error. */
int qimgv_engine_file_count(qimgv_engine *engine);
int qimgv_engine_dir_count(qimgv_engine *engine);
int qimgv_engine_file_entry_at(qimgv_engine *engine, int index,
                               qimgv_entry_meta *out,
                               char *name_buf, size_t name_cap,
                               char *path_buf, size_t path_cap);
int qimgv_engine_dir_entry_at(qimgv_engine *engine, int index,
                              qimgv_entry_meta *out,
                              char *name_buf, size_t name_cap,
                              char *path_buf, size_t path_cap);
/* Handle navigation: QIMGV_OK on success, QIMGV_ERR_EMPTY at either end. */
int qimgv_engine_next_file(qimgv_engine *engine, uint64_t handle, uint64_t *out);
int qimgv_engine_prev_file(qimgv_engine *engine, uint64_t handle, uint64_t *out);
int qimgv_engine_next_dir(qimgv_engine *engine, uint64_t handle, uint64_t *out);
int qimgv_engine_prev_dir(qimgv_engine *engine, uint64_t handle, uint64_t *out);

/* Returns 1 if an event was written, 0 if none pending, negative on error.
 * dir_buf receives the directory path (for DIR_MUTATED: the changed entry's
 * full path); mutation is filled for DIR_MUTATED, may be NULL otherwise. */
int qimgv_engine_take_event(qimgv_engine *engine, qimgv_event *out,
                            char *dir_buf, size_t dir_cap,
                            qimgv_mutation *mutation);

/* Pure function — thread-safe, does not touch the engine instance. One file
 * open, header-only parse. Parse failures are QIMGV_OK with FMT_UNKNOWN and
 * zero dimensions; only an unreadable file is ERR_IO. */
int qimgv_engine_probe(const char *path_utf8, qimgv_probe_result *out);

#ifdef __cplusplus
}
#endif
