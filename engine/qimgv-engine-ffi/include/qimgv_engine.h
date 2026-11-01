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
} qimgv_event;

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
int qimgv_engine_open_dir(qimgv_engine *engine, const char *path_utf8);
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

/* Returns 1 if an event was written, 0 if none pending, negative on error. */
int qimgv_engine_take_event(qimgv_engine *engine, qimgv_event *out,
                            char *dir_buf, size_t dir_cap);

#ifdef __cplusplus
}
#endif
