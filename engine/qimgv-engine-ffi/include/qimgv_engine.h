/* C ABI for qimgv-engine. Mirrors engine/qimgv-engine-ffi/src/lib.rs.
 *
 * Contract:
 *  - All calls must come from a single thread (the Qt main thread).
 *  - No panic ever crosses this boundary; failures return negative/enum codes
 *    and qimgv_last_error() yields a thread-local diagnostic message valid
 *    until the next engine call on the same thread.
 *  - String out-buffers are filled with NUL-terminated UTF-8;
 *    QIMGV_ERR_TRUNCATED signals an undersized buffer. A take_event that
 *    truncates has written nothing and has NOT consumed the event: the engine
 *    holds it and returns it on the next call, so growing the buffer and
 *    retrying is always safe. (export_listing instead answers with the size it
 *    needs and writes nothing until it fits — two different protocols, both
 *    lossless.)
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
#define QIMGV_EVENT_THUMB_NEXT  4

/* mutation kinds (qimgv_mutation.kind) */
#define QIMGV_MUT_ADDED    1
#define QIMGV_MUT_MODIFIED 2
#define QIMGV_MUT_REMOVED  3
/* One entry, a new path: subject_buf is where it is now and old_buf is the
 * path it had. The only kind with a second subject. */
#define QIMGV_MUT_RENAMED  4

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
    /* For DIR_MUTATED: the engine's ordinal of this mutation, assigned before
     * the event could be dropped downstream. 0 for the other kinds. Nothing
     * compares it — a listing does not have to be the same size at the same
     * number — so it is a log key, not a version: it lets a line printed by the
     * engine and a line printed by the consumer be matched by someone reading
     * both, which is the difference between "the mirror did not apply it" and
     * "the engine never emitted it". Set QIMGV_ENGINE_TRACE=1 to get both. */
    uint32_t seq;
} qimgv_event;

typedef struct qimgv_mutation {
    int32_t kind;      /* QIMGV_MUT_* */
    int32_t is_dir;
    uint64_t size;
    int64_t mtime_ms;
    /* The entry's position in the engine's own order at the time of the
     * event: where an Added entry was inserted, where a Modified entry sits
     * after its metadata re-sort, where a Removed entry lived. The mirror's
     * only order decision is to splice at this index. */
    int32_t index;
} qimgv_mutation;

typedef struct qimgv_thumb_req {
    uint32_t size;
    int32_t crop;
} qimgv_thumb_req;

typedef struct qimgv_probe_result {
    int32_t format;      /* QIMGV_FMT_* */
    int32_t animated;    /* 0/1 */
    int32_t orientation; /* EXIF 1..=8; 1 = normal/unknown */
    int32_t width;       /* 0 = unknown */
    int32_t height;
} qimgv_probe_result;

typedef void (*qimgv_notify_fn)(void *user);

/* Compile-time layout pinning. These five structs cross by value, so their
 * layout IS the ABI — and unlike a symbol name, a layout mismatch does not fail
 * at link time: the call resolves, the arguments are simply read from the wrong
 * bytes. The same numbers are asserted on the Rust side
 * (engine/qimgv-engine-ffi/src/lib.rs, "ABI layout pins") and
 * tests/architecture_rules.py R5 fails if the two literals disagree.
 *
 * They live here rather than in a test file because the production build must
 * check them too: whoever includes this header compiles the contract. */
#ifdef __cplusplus
#  define QIMGV_PIN(what, cond) static_assert(cond, what " layout drift")
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define QIMGV_PIN(what, cond) _Static_assert(cond, what " layout drift")
#else
#  define QIMGV_PIN(what, cond) /* no compile-time assert available */
#endif

/* (u64,u64,i64,int) */
QIMGV_PIN("qimgv_entry_meta", sizeof(qimgv_entry_meta) == 32);
QIMGV_PIN("qimgv_entry_meta.handle", offsetof(qimgv_entry_meta, handle) == 0);
QIMGV_PIN("qimgv_entry_meta.size", offsetof(qimgv_entry_meta, size) == 8);
QIMGV_PIN("qimgv_entry_meta.mtime_ms", offsetof(qimgv_entry_meta, mtime_ms) == 16);
QIMGV_PIN("qimgv_entry_meta.is_dir", offsetof(qimgv_entry_meta, is_dir) == 24);

/* (int,int,u32,u32) */
QIMGV_PIN("qimgv_event", sizeof(qimgv_event) == 16);
QIMGV_PIN("qimgv_event.kind", offsetof(qimgv_event, kind) == 0);
QIMGV_PIN("qimgv_event.code", offsetof(qimgv_event, code) == 4);
QIMGV_PIN("qimgv_event.epoch", offsetof(qimgv_event, epoch) == 8);
QIMGV_PIN("qimgv_event.seq", offsetof(qimgv_event, seq) == 12);

/* (int32,int32,u64,int64,int32) — index packs at 24, size pads to 32 */
QIMGV_PIN("qimgv_mutation", sizeof(qimgv_mutation) == 32);
QIMGV_PIN("qimgv_mutation.kind", offsetof(qimgv_mutation, kind) == 0);
QIMGV_PIN("qimgv_mutation.is_dir", offsetof(qimgv_mutation, is_dir) == 4);
QIMGV_PIN("qimgv_mutation.size", offsetof(qimgv_mutation, size) == 8);
QIMGV_PIN("qimgv_mutation.mtime_ms", offsetof(qimgv_mutation, mtime_ms) == 16);
QIMGV_PIN("qimgv_mutation.index", offsetof(qimgv_mutation, index) == 24);

/* (u32,i32) */
QIMGV_PIN("qimgv_thumb_req", sizeof(qimgv_thumb_req) == 8);
QIMGV_PIN("qimgv_thumb_req.size", offsetof(qimgv_thumb_req, size) == 0);
QIMGV_PIN("qimgv_thumb_req.crop", offsetof(qimgv_thumb_req, crop) == 4);

/* 5 x int32 */
QIMGV_PIN("qimgv_probe_result", sizeof(qimgv_probe_result) == 20);
QIMGV_PIN("qimgv_probe_result.format", offsetof(qimgv_probe_result, format) == 0);
QIMGV_PIN("qimgv_probe_result.animated", offsetof(qimgv_probe_result, animated) == 4);
QIMGV_PIN("qimgv_probe_result.orientation", offsetof(qimgv_probe_result, orientation) == 8);
QIMGV_PIN("qimgv_probe_result.width", offsetof(qimgv_probe_result, width) == 12);
QIMGV_PIN("qimgv_probe_result.height", offsetof(qimgv_probe_result, height) == 16);

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

/* Returns 1 if an event was written, 0 if none pending, otherwise a POSITIVE
 * QIMGV_ERR_* code — so a caller must compare against 1, not against 0: any
 * other value than those two wrote NOTHING, and out/mutation/subject_buf still
 * hold the previous event. Reading a refusal as "an event arrived" re-applies
 * that stale event.
 *
 * subject_buf receives ONE STRING whose subject depends on out->kind — read
 * this table before using it, because the value alone does not say which it is:
 *
 *   QIMGV_EVENT_DIR_LISTED     the directory that is now listed
 *   QIMGV_EVENT_OPEN_FAILED    the directory whose open failed
 *   QIMGV_EVENT_DIR_MUTATED    the FULL PATH of the changed entry (never a
 *                              directory; the directory it lives in is not sent)
 *   QIMGV_EVENT_THUMB_NEXT     the FULL PATH of the file to render
 *
 * mutation is filled for DIR_MUTATED and may be NULL otherwise. thumb is filled
 * for THUMB_NEXT.
 *
 * old_buf is written for exactly ONE mutation kind and left untouched for every
 * other event, so it must be read only after checking mutation->kind:
 *
 *   QIMGV_MUT_RENAMED    the FULL PATH the entry had, before it moved to the
 *                        path in subject_buf
 *
 * A rename is refused — QIMGV_ERR_INVALID, event still queued — when old_buf is
 * NULL or too small: without the path it came from, a consumer can only guess at
 * the row to move, and guessing wrong leaves a row behind for a file that still
 * exists under that name.
 *
 * Two more fields are only meaningful in context:
 *   out->epoch == 0    the event belongs to no open session (THUMB_NEXT), so a
 *                      receiver must not filter it against the current one.
 *   mutation->index    a position in the engine's listing at the instant the
 *                      event was emitted — not a stable id. Consumers splice at
 *                      it; none may re-derive an order of its own.
 */
int qimgv_engine_take_event(qimgv_engine *engine, qimgv_event *out,
                            char *subject_buf, size_t subject_cap,
                            char *old_buf, size_t old_cap,
                            qimgv_mutation *mutation,
                            qimgv_thumb_req *thumb);

/* Bulk listing export: serializes the current listing in ONE lock pass
 * (little-endian: u32 file_count, u32 dir_count, then per entry
 * u64 handle, u64 size, i64 mtime_ms, u32 name_len, u32 path_len,
 * name bytes, path bytes — files first, then dirs). Call with cap=0 to
 * query `needed_out`; returns QIMGV_ERR_TRUNCATED when cap is too small. */
int qimgv_engine_export_listing(qimgv_engine *engine, unsigned char *buf,
                                size_t cap, uint32_t *needed_out,
                                uint32_t *file_count_out, uint32_t *dir_count_out);

/* Thumbnail scheduling: the engine holds the priority queue; the caller
 * executes dispatched requests (QIMGV_EVENT_THUMB_NEXT) and reports
 * completion. replace_batch=1 on the FIRST request of a fresh viewport
 * batch drops every still-pending request from the previous batch. */
int qimgv_engine_thumb_request(qimgv_engine *engine, const char *path_utf8,
                               uint32_t size, int crop, int replace_batch);
int qimgv_engine_thumb_done(qimgv_engine *engine, const char *path_utf8,
                            uint32_t size, int crop);
int qimgv_engine_thumb_clear(qimgv_engine *engine);
int qimgv_engine_thumb_config(qimgv_engine *engine, uint32_t max_in_flight);

/* Pure function — thread-safe, does not touch the engine instance. One file
 * open, header-only parse. Parse failures are QIMGV_OK with FMT_UNKNOWN and
 * zero dimensions; only an unreadable file is ERR_IO. */
int qimgv_engine_probe(const char *path_utf8, qimgv_probe_result *out);

#ifdef __cplusplus
}
#endif
