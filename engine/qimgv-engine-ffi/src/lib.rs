//! C ABI for qimgv-engine.
//!
//! Discipline (agreed in stack discussion):
//! - No panic ever crosses the boundary: every entry point is wrapped in
//!   `catch_unwind` and folds panics into `QIMGV_ERR_PANIC`.
//! - Strings cross as NUL-terminated UTF-8 into caller-provided buffers;
//!   `QIMGV_ERR_TRUNCATED` signals an undersized buffer.
//! - All calls must come from a single thread (the Qt main thread).
//!
//! The normative safety contract for every exported function is
//! `include/qimgv_engine.h` (read by the C++ consumer); it is intentionally
//! not duplicated per-function here.
#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use qimgv_engine::{Command, Engine, EngineConfig, EngineError, Event, NameFilter, Notify, SortMode, ThumbReq};

// --- status codes (mirror include/qimgv_engine.h) ---------------------------
pub const QIMGV_OK: c_int = 0;
pub const QIMGV_ERR_IO: c_int = 1;
pub const QIMGV_ERR_ENGINE_GONE: c_int = 2;
pub const QIMGV_ERR_INVALID: c_int = 3;
pub const QIMGV_ERR_PANIC: c_int = 4;
pub const QIMGV_ERR_EMPTY: c_int = 5;
pub const QIMGV_ERR_TRUNCATED: c_int = 6;
pub const QIMGV_ERR_BAD_SORT: c_int = 7;
pub const QIMGV_ERR_BAD_UTF8: c_int = 8;

// --- event kinds ------------------------------------------------------------
pub const QIMGV_EVENT_NONE: c_int = 0;
pub const QIMGV_EVENT_DIR_LISTED: c_int = 1;
pub const QIMGV_EVENT_OPEN_FAILED: c_int = 2;
pub const QIMGV_EVENT_DIR_MUTATED: c_int = 3;

// --- mutation kinds (qimgv_mutation.kind) -----------------------------------
pub const QIMGV_MUT_ADDED: c_int = 1;
pub const QIMGV_MUT_MODIFIED: c_int = 2;
pub const QIMGV_MUT_REMOVED: c_int = 3;
// One entry, a new path. `subject_buf` holds the path it has now; the path it
// had is the event's second subject and rides in `old_buf` (see take_event).
pub const QIMGV_MUT_RENAMED: c_int = 4;

// --- events (continued) ------------------------------------------------------
pub const QIMGV_EVENT_THUMB_NEXT: c_int = 4;

// --- image formats (mirror meta::ImageFormat) -------------------------------
pub const QIMGV_FMT_UNKNOWN: c_int = 0;
pub const QIMGV_FMT_JPEG: c_int = 1;
pub const QIMGV_FMT_PNG: c_int = 2;
pub const QIMGV_FMT_GIF: c_int = 3;
pub const QIMGV_FMT_WEBP: c_int = 4;
pub const QIMGV_FMT_AVIF: c_int = 5;
pub const QIMGV_FMT_JXL: c_int = 6;
pub const QIMGV_FMT_BMP: c_int = 7;
pub const QIMGV_FMT_TIFF: c_int = 8;

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

fn set_last_error(msg: &str) {
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new(msg).ok();
    });
}

fn set_last_error_by_code(code: c_int) {
    let msg = match code {
        QIMGV_ERR_IO => "I/O error",
        QIMGV_ERR_ENGINE_GONE => "engine thread is gone",
        QIMGV_ERR_INVALID => "invalid argument",
        QIMGV_ERR_EMPTY => "no such entry or nothing pending",
        QIMGV_ERR_TRUNCATED => "output buffer too small",
        QIMGV_ERR_BAD_SORT => "unknown sort mode",
        QIMGV_ERR_BAD_UTF8 => "input is not valid UTF-8",
        _ => "unknown error",
    };
    set_last_error(msg);
}

#[repr(C)]
pub struct QimgvEntryMeta {
    pub handle: u64,
    pub size: u64,
    pub mtime_ms: i64,
    pub is_dir: c_int,
}

#[repr(C)]
pub struct QimgvEvent {
    pub kind: c_int,
    pub code: c_int,
    /// Session the event belongs to; open_dir hands out the session id and
    /// consumers drop events from older sessions.
    pub epoch: u32,
    /// For DIR_MUTATED: the engine's own ordinal of this mutation, assigned
    /// before the event could be dropped anywhere downstream. 0 for the other
    /// kinds. It carries no authority — nothing compares it, and a listing does
    /// not have to be the same size at the same number — it exists so a line
    /// printed by the engine and a line printed by the C++ mirror can be
    /// matched by a human reading both. Truncated from the engine's u64 for
    /// that reason: at 4 billion mutations it wraps, and a wrap changes which
    /// number is printed, never which event follows which.
    pub seq: u32,
}

#[repr(C)]
pub struct QimgvProbeResult {
    pub format: c_int,
    pub animated: c_int,
    pub orientation: c_int, // EXIF 1..=8; 1 = normal/unknown
    pub width: c_int,       // 0 = unknown
    pub height: c_int,
}

#[repr(C)]
pub struct QimgvMutation {
    pub kind: c_int, // QIMGV_MUT_*
    pub is_dir: c_int,
    pub size: u64,
    pub mtime_ms: i64,
    /// The entry's position in the engine's own order at the time of the
    /// event: where an Added entry was inserted, where a Modified entry sits
    /// after its metadata re-sort, where a Removed entry lived. The mirror's
    /// only order decision is to splice at this index.
    pub index: c_int,
}

#[repr(C)]
pub struct QimgvThumbReq {
    pub size: u32,
    pub crop: c_int,
}

// ABI layout pins. The C header asserts the same numbers
// (qimgv_engine.h, "Compile-time layout pinning") and tests/architecture_rules.py
// R5 fails if the two literals disagree. This side needs its own assert because
// #[repr(C)] does not freeze a declaration: reorder two fields or widen one and
// everything still compiles here, the symbol names still match, and the consumer
// reads arguments from the wrong bytes.
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<QimgvEntryMeta>() == 32);
    assert!(offset_of!(QimgvEntryMeta, handle) == 0);
    assert!(offset_of!(QimgvEntryMeta, size) == 8);
    assert!(offset_of!(QimgvEntryMeta, mtime_ms) == 16);
    assert!(offset_of!(QimgvEntryMeta, is_dir) == 24);
    assert!(size_of::<QimgvEvent>() == 16);
    assert!(offset_of!(QimgvEvent, kind) == 0);
    assert!(offset_of!(QimgvEvent, code) == 4);
    assert!(offset_of!(QimgvEvent, epoch) == 8);
    assert!(offset_of!(QimgvEvent, seq) == 12);
    assert!(size_of::<QimgvMutation>() == 32);
    assert!(offset_of!(QimgvMutation, kind) == 0);
    assert!(offset_of!(QimgvMutation, is_dir) == 4);
    assert!(offset_of!(QimgvMutation, size) == 8);
    assert!(offset_of!(QimgvMutation, mtime_ms) == 16);
    assert!(offset_of!(QimgvMutation, index) == 24);
    assert!(size_of::<QimgvThumbReq>() == 8);
    assert!(offset_of!(QimgvThumbReq, size) == 0);
    assert!(offset_of!(QimgvThumbReq, crop) == 4);
    assert!(size_of::<QimgvProbeResult>() == 20);
    assert!(offset_of!(QimgvProbeResult, format) == 0);
    assert!(offset_of!(QimgvProbeResult, animated) == 4);
    assert!(offset_of!(QimgvProbeResult, orientation) == 8);
    assert!(offset_of!(QimgvProbeResult, width) == 12);
    assert!(offset_of!(QimgvProbeResult, height) == 16);
};

/// Raw pointers are not Send; the notify callback ships one to engine threads.
struct SendPtr(*mut c_void);

impl SendPtr {
    // Method (not field) access so the notify closure captures the whole
    // wrapper — a bare `user.0` projection would capture the raw pointer
    // itself, which is neither Send nor Sync.
    fn get(&self) -> *mut c_void {
        self.0
    }
}

unsafe impl Send for SendPtr {}
unsafe impl Sync for SendPtr {}

type NotifyCb = extern "C" fn(user: *mut c_void);

pub struct QimgvEngine {
    engine: Mutex<Engine>,
    /// One event that the caller's buffer could not hold, kept for the next
    /// call. See qimgv_engine_take_event: an event that is popped and then lost
    /// is indistinguishable from one that never happened.
    stashed: Mutex<Option<Event>>,
}

fn map_engine_error(e: &EngineError) -> c_int {
    match e {
        EngineError::Io { .. } | EngineError::NotADirectory(_) => QIMGV_ERR_IO,
        EngineError::EngineGone => QIMGV_ERR_ENGINE_GONE,
        EngineError::BadSortMode(_) => QIMGV_ERR_BAD_SORT,
    }
}

// --- minimal, audited unsafe blocks -----------------------------------------

/// # Safety
/// `ptr` must point to a valid NUL-terminated string for the duration of the call.
fn read_path(ptr: *const c_char) -> Result<PathBuf, c_int> {
    if ptr.is_null() {
        return Err(QIMGV_ERR_INVALID);
    }
    let s = unsafe { CStr::from_ptr(ptr) };
    match s.to_str() {
        Ok(s) if !s.is_empty() => Ok(PathBuf::from(s)),
        Ok(_) => Err(QIMGV_ERR_INVALID),
        Err(_) => Err(QIMGV_ERR_BAD_UTF8),
    }
}

fn write_str(buf: *mut c_char, cap: usize, s: &str) -> Result<(), c_int> {
    fits(buf, cap, s)?;
    let bytes = s.as_bytes();
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr().cast::<c_char>(), buf, bytes.len());
        buf.add(bytes.len()).write(0);
    }
    Ok(())
}

/// The checks `write_str` makes, without the copy. take_event promises that a
/// refused event "has written nothing", so a call site that needs two buffers
/// validates both before touching the first one.
fn fits(buf: *mut c_char, cap: usize, s: &str) -> Result<(), c_int> {
    if buf.is_null() {
        return Err(QIMGV_ERR_INVALID);
    }
    if s.as_bytes().len() + 1 > cap {
        return Err(QIMGV_ERR_TRUNCATED);
    }
    Ok(())
}

/// # Safety
/// `engine` must be a live pointer returned by `qimgv_engine_new`, not yet freed.
fn with_engine<T>(engine: *mut QimgvEngine, f: impl FnOnce(&Engine) -> T) -> T {
    let eng = unsafe { &*engine };
    let guard = eng.engine.lock().unwrap_or_else(|p| p.into_inner());
    f(&guard)
}

fn mtime_ms(t: &SystemTime) -> i64 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

// --- lifecycle --------------------------------------------------------------

/// `formats_extensions_utf8`: '\n'-joined extension whitelist (e.g. "jpg\npng"),
/// or NULL for "accept everything". Sourced from Settings::supportedFormats().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_new(
    notify: Option<NotifyCb>,
    user: *mut c_void,
    scan_threads: c_int,
    formats_extensions_utf8: *const c_char,
) -> *mut QimgvEngine {
    let attempt = || -> Result<QimgvEngine, String> {
        let filter = if formats_extensions_utf8.is_null() {
            // documented as "NULL to accept everything" in qimgv_engine.h
            NameFilter::from_extensions(std::iter::empty::<&str>())
        } else {
            let joined = unsafe { CStr::from_ptr(formats_extensions_utf8) }
                .to_str()
                .map_err(|_| "extension list is not valid UTF-8".to_string())?;
            NameFilter::from_extensions(joined.split('\n'))
        };
        let notify_fn: Notify = match notify {
            Some(cb) => {
                let user = SendPtr(user);
                Arc::new(move || cb(user.get()))
            }
            None => Arc::new(|| {}),
        };
        let engine = Engine::spawn(
            EngineConfig {
                scan_threads: scan_threads.max(1) as usize,
                show_hidden: Arc::new(AtomicBool::new(false)),
                filter,
                initial_sort: SortMode::Name,
            },
            notify_fn,
        );
        Ok(QimgvEngine { engine: Mutex::new(engine), stashed: Mutex::new(None) })
    };

    match catch_unwind(attempt) {
        Ok(Ok(engine)) => Box::into_raw(Box::new(engine)),
        Ok(Err(msg)) => {
            set_last_error(&msg);
            std::ptr::null_mut()
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_new");
            std::ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_free(engine: *mut QimgvEngine) {
    if engine.is_null() {
        return;
    }
    if catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(engine)) })).is_err() {
        set_last_error("panic in qimgv_engine_free");
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn qimgv_engine_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr().cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn qimgv_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| {
        slot.borrow().as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null())
    })
}

// --- commands ---------------------------------------------------------------

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_open_dir(
    engine: *mut QimgvEngine,
    path: *const c_char,
    sort_mode: c_int,
    epoch_out: *mut u32,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        let sort = if sort_mode < 0 {
            None // keep the engine's current sort
        } else {
            Some(SortMode::try_from(sort_mode).map_err(|_| QIMGV_ERR_BAD_SORT)?)
        };
        with_engine(engine, |e| {
            let epoch = e.next_epoch();
            e.send(Command::OpenDir { path, sort, epoch })
                .map(|_| {
                    if !epoch_out.is_null() {
                        unsafe { *epoch_out = epoch as u32 };
                    }
                    QIMGV_OK
                })
                .map_err(|er| map_engine_error(&er))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_open_dir");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_set_sort(engine: *mut QimgvEngine, mode: c_int) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let mode = SortMode::try_from(mode).map_err(|_| QIMGV_ERR_BAD_SORT)?;
        with_engine(engine, |e| {
            e.send(Command::SetSort(mode))
                .map(|_| QIMGV_OK)
                .map_err(|e| map_engine_error(&e))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_set_sort");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_set_show_hidden(engine: *mut QimgvEngine, show: c_int) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        with_engine(engine, |e| {
            e.set_show_hidden(show != 0);
            Ok(QIMGV_OK)
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_set_show_hidden");
            QIMGV_ERR_PANIC
        }
    }
}

// --- state reads ------------------------------------------------------------

fn count(engine: *mut QimgvEngine, dirs: bool) -> c_int {
    if engine.is_null() {
        set_last_error("null engine");
        return -1;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        with_engine(engine, |e| {
            let st = e.state.read().unwrap_or_else(|p| p.into_inner());
            (if dirs { st.dir_count() } else { st.file_count() }) as c_int
        })
    }));
    match r {
        Ok(n) => n,
        Err(_) => {
            set_last_error("engine panic");
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_file_count(engine: *mut QimgvEngine) -> c_int {
    count(engine, false)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_dir_count(engine: *mut QimgvEngine) -> c_int {
    count(engine, true)
}

#[allow(clippy::too_many_arguments)] // out-struct + two buffers is the FFI shape
fn entry_at(
    engine: *mut QimgvEngine,
    index: c_int,
    dirs: bool,
    out: *mut QimgvEntryMeta,
    name_buf: *mut c_char,
    name_cap: usize,
    path_buf: *mut c_char,
    path_cap: usize,
) -> c_int {
    if engine.is_null() || out.is_null() {
        set_last_error("invalid argument");
        return QIMGV_ERR_INVALID;
    }
    if index < 0 {
        set_last_error("negative index");
        return QIMGV_ERR_INVALID;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        with_engine(engine, |e| {
            let st = e.state.read().unwrap_or_else(|p| p.into_inner());
            let entry = if dirs {
                st.dir_at(index as usize)
            } else {
                st.file_at(index as usize)
            };
            let Some(entry) = entry else { return Err(QIMGV_ERR_EMPTY) };
            write_str(name_buf, name_cap, &entry.data.name)?;
            write_str(path_buf, path_cap, &entry.data.path)?;
            unsafe {
                *out = QimgvEntryMeta {
                    handle: qimgv_engine::key_to_ffi(entry.handle),
                    size: entry.data.size,
                    mtime_ms: mtime_ms(&entry.data.mtime),
                    is_dir: if dirs { 1 } else { 0 },
                };
            }
            Ok(QIMGV_OK)
        })
    }));
    match r {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("engine panic");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_file_entry_at(
    engine: *mut QimgvEngine,
    index: c_int,
    out: *mut QimgvEntryMeta,
    name_buf: *mut c_char,
    name_cap: usize,
    path_buf: *mut c_char,
    path_cap: usize,
) -> c_int {
    entry_at(engine, index, false, out, name_buf, name_cap, path_buf, path_cap)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_dir_entry_at(
    engine: *mut QimgvEngine,
    index: c_int,
    out: *mut QimgvEntryMeta,
    name_buf: *mut c_char,
    name_cap: usize,
    path_buf: *mut c_char,
    path_cap: usize,
) -> c_int {
    entry_at(engine, index, true, out, name_buf, name_cap, path_buf, path_cap)
}

fn neighbor(
    engine: *mut QimgvEngine,
    handle: u64,
    dirs: bool,
    next: bool,
    out: *mut u64,
) -> c_int {
    if engine.is_null() || out.is_null() {
        set_last_error("invalid argument");
        return QIMGV_ERR_INVALID;
    }
    let Some(key) = qimgv_engine::key_from_ffi(handle) else {
        set_last_error("null handle");
        return QIMGV_ERR_INVALID;
    };
    let r = catch_unwind(AssertUnwindSafe(|| {
        with_engine(engine, |e| {
            let st = e.state.read().unwrap_or_else(|p| p.into_inner());
            let n = match (dirs, next) {
                (false, true) => st.next_file(key),
                (false, false) => st.prev_file(key),
                (true, true) => st.next_dir(key),
                (true, false) => st.prev_dir(key),
            };
            let Some(n) = n else { return Err(QIMGV_ERR_EMPTY) };
            unsafe { *out = qimgv_engine::key_to_ffi(n) };
            Ok(QIMGV_OK)
        })
    }));
    match r {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("engine panic");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_next_file(engine: *mut QimgvEngine, handle: u64, out: *mut u64) -> c_int {
    neighbor(engine, handle, false, true, out)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_prev_file(engine: *mut QimgvEngine, handle: u64, out: *mut u64) -> c_int {
    neighbor(engine, handle, false, false, out)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_next_dir(engine: *mut QimgvEngine, handle: u64, out: *mut u64) -> c_int {
    neighbor(engine, handle, true, true, out)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_prev_dir(engine: *mut QimgvEngine, handle: u64, out: *mut u64) -> c_int {
    neighbor(engine, handle, true, false, out)
}

// --- thumbnail scheduling -----------------------------------------------------

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_thumb_request(
    engine: *mut QimgvEngine,
    path: *const c_char,
    size: u32,
    crop: c_int,
    replace_batch: c_int,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        with_engine(engine, |e| {
            e.send(Command::ThumbRequest {
                req: ThumbReq { path, size, crop: crop != 0 },
                replace_batch: replace_batch != 0,
            })
            .map(|_| QIMGV_OK)
            .map_err(|er| map_engine_error(&er))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_thumb_request");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_thumb_done(
    engine: *mut QimgvEngine,
    path: *const c_char,
    size: u32,
    crop: c_int,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        with_engine(engine, |e| {
            e.send(Command::ThumbDone {
                req: ThumbReq { path, size, crop: crop != 0 },
            })
            .map(|_| QIMGV_OK)
            .map_err(|er| map_engine_error(&er))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => code,
        Err(_) => {
            set_last_error("panic in qimgv_engine_thumb_done");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_thumb_clear(engine: *mut QimgvEngine) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        with_engine(engine, |e| {
            e.send(Command::ThumbClear)
                .map(|_| QIMGV_OK)
                .map_err(|er| map_engine_error(&er))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => code,
        Err(_) => {
            set_last_error("panic in qimgv_engine_thumb_clear");
            QIMGV_ERR_PANIC
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_thumb_config(engine: *mut QimgvEngine, max_in_flight: u32) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        with_engine(engine, |e| {
            e.send(Command::ThumbConfig { max_in_flight })
                .map(|_| QIMGV_OK)
                .map_err(|er| map_engine_error(&er))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => code,
        Err(_) => {
            set_last_error("panic in qimgv_engine_thumb_config");
            QIMGV_ERR_PANIC
        }
    }
}

// --- bulk listing export ------------------------------------------------------

/// Serializes the current listing into `buf` in one lock pass (LE wire format,
/// see model::write_listing). If `cap` is too small, returns QIMGV_ERR_TRUNCATED
/// with `needed_out` set; counts are always written.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_export_listing(
    engine: *mut QimgvEngine,
    buf: *mut u8,
    cap: usize,
    needed_out: *mut u32,
    file_count_out: *mut u32,
    dir_count_out: *mut u32,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        if engine.is_null() || needed_out.is_null() || file_count_out.is_null() || dir_count_out.is_null()
        {
            return Err(QIMGV_ERR_INVALID);
        }
        with_engine(engine, |e| {
            // cap==0 with a null buf is the size-probe call
            let slice = if buf.is_null() && cap == 0 {
                &mut [][..]
            } else if buf.is_null() {
                return Err(QIMGV_ERR_INVALID);
            } else {
                unsafe { std::slice::from_raw_parts_mut(buf, cap) }
            };
            let (needed, fc, dc) = e.export_listing(slice);
            unsafe {
                *needed_out = needed as u32;
                *file_count_out = fc as u32;
                *dir_count_out = dc as u32;
            }
            if slice.len() < needed {
                Err(QIMGV_ERR_TRUNCATED)
            } else {
                Ok(QIMGV_OK)
            }
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_export_listing");
            QIMGV_ERR_PANIC
        }
    }
}

// --- header probe -----------------------------------------------------------

/// Pure function (thread-safe, does not touch the engine instance): one file
/// open, header-only parse. Parse failures are QIMGV_OK with FMT_UNKNOWN and
/// zero dimensions; only an unreadable file is ERR_IO.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_probe(path: *const c_char, out: *mut QimgvProbeResult) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        if out.is_null() {
            return Err(QIMGV_ERR_INVALID);
        }
        let p = read_path(path)?;
        let info = qimgv_engine::meta::probe(&p).map_err(|e| map_engine_error(&e))?;
        unsafe {
            *out = QimgvProbeResult {
                format: info.format as c_int,
                animated: info.animated as c_int,
                orientation: info.orientation as c_int,
                width: info.width as c_int,
                height: info.height as c_int,
            };
        }
        Ok(QIMGV_OK)
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_probe");
            QIMGV_ERR_PANIC
        }
    }
}

// --- watcher ----------------------------------------------------------------

/// Force-includes a path in the current listing (app-side mime fallback);
/// persists across rescans until the file disappears. Returns OK even if the
/// file is gone — the state simply won't change.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_include_path(engine: *mut QimgvEngine, path: *const c_char) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        with_engine(engine, |e| {
            e.send(Command::IncludePath(path))
                .map(|_| QIMGV_OK)
                .map_err(|e| map_engine_error(&e))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_include_path");
            QIMGV_ERR_PANIC
        }
    }
}

/// Stops watching the current directory (after a listing reset).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_stop_watch(engine: *mut QimgvEngine) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        with_engine(engine, |e| {
            e.send(Command::StopWatch)
                .map(|_| QIMGV_OK)
                .map_err(|e| map_engine_error(&e))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => code,
        Err(_) => {
            set_last_error("panic in qimgv_engine_stop_watch");
            QIMGV_ERR_PANIC
        }
    }
}

/// 1 when the engine ships its own inotify watcher (Linux); 0 means the
/// caller keeps using its platform watcher and the mutation events never fire.
#[unsafe(no_mangle)]
pub extern "C" fn qimgv_engine_supports_watching() -> c_int {
    if Engine::supports_watching() { 1 } else { 0 }
}

// --- events -----------------------------------------------------------------

/// Returns 1 if an event was written to `out`, 0 if none is pending, otherwise
/// a positive `QIMGV_ERR_*` code (the codes are enum values, not negated — so
/// compare against 1, and treat anything else as "wrote nothing"). `subject_buf`
/// receives the event's subject, which is the directory for DIR_LISTED and
/// OPEN_FAILED and the *full path* for DIR_MUTATED and
/// THUMB_NEXT (the table lives in qimgv_engine.h next to the declaration);
/// `mutation` is filled for DIR_MUTATED, `thumb` for THUMB_NEXT.
///
/// `old_buf` is written for ONE mutation kind, QIMGV_MUT_RENAMED, and holds the
/// path the entry had. A rename has two subjects and one slot, so the second
/// buffer exists rather than the old path being guessed by the consumer: a
/// rename delivered without it cannot be applied in place, and applying it as an
/// add leaves a row behind for a file that is still on disk under that name.
/// Every other kind leaves `old_buf` untouched, and a NULL `old_buf` refuses a
/// rename (it stays queued) instead of reporting it incompletely.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_take_event(
    engine: *mut QimgvEngine,
    out: *mut QimgvEvent,
    subject_buf: *mut c_char,
    subject_cap: usize,
    old_buf: *mut c_char,
    old_cap: usize,
    mutation: *mut QimgvMutation,
    thumb: *mut QimgvThumbReq,
) -> c_int {
    if engine.is_null() || out.is_null() {
        set_last_error("invalid argument");
        return QIMGV_ERR_INVALID;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        let eng = unsafe { &*engine };
        // Lock order is stashed then engine, everywhere; take_event is the only
        // function that touches both.
        let mut stash = eng.stashed.lock().unwrap_or_else(|p| p.into_inner());
        let pending: Option<Event> = match stash.take() {
            Some(ev) => Some(ev),
            None => with_engine(engine, |e| e.try_event()),
        };
        // An inner closure: the arms' `?` has to mean "this event did not fit",
        // not "leave take_event" — the latter would skip the put-it-back below,
        // which is the whole point of the stash.
        let outcome: Result<c_int, c_int> = (|| -> Result<c_int, c_int> {
        match pending.as_ref() {
            None => Ok(0),
            Some(ev) => match ev {
            Event::DirListed { dir, epoch } => {
                write_str(subject_buf, subject_cap, &dir.to_string_lossy())?;
                unsafe {
                    *out = QimgvEvent { kind: QIMGV_EVENT_DIR_LISTED, code: QIMGV_OK, epoch: *epoch as u32, seq: 0 };
                }
                Ok(1)
            }
            Event::OpenFailed { dir, error, epoch } => {
                write_str(subject_buf, subject_cap, &dir.to_string_lossy())?;
                let code = map_engine_error(error);
                set_last_error(&error.to_string());
                unsafe {
                    *out = QimgvEvent { kind: QIMGV_EVENT_OPEN_FAILED, code, epoch: *epoch as u32, seq: 0 };
                }
                Ok(1)
            }
            Event::ThumbNext { req } => {
                write_str(subject_buf, subject_cap, &req.path.to_string_lossy())?;
                if !thumb.is_null() {
                    unsafe {
                        *thumb = QimgvThumbReq { size: req.size, crop: req.crop as c_int };
                    }
                }
                unsafe {
                    *out = QimgvEvent { kind: QIMGV_EVENT_THUMB_NEXT, code: QIMGV_OK, epoch: 0, seq: 0 };
                }
                Ok(1)
            }
            Event::Mutated { seq, kind, path, name: _, is_dir, size, mtime_ms, index, epoch } => {
                // A rename is the one mutation with two subjects, and both have
                // to fit before either is written: a caller that cannot receive
                // the old path is left with an event it can only mis-apply, so
                // the event stays queued instead.
                let from = match kind {
                    qimgv_engine::WatchChangeKind::Renamed { from } => Some(from),
                    _ => None,
                };
                if let Some(from) = from {
                    fits(old_buf, old_cap, from)?;
                }
                write_str(subject_buf, subject_cap, path)?;
                if let Some(from) = from {
                    write_str(old_buf, old_cap, from)?;
                }
                let mkind = match kind {
                    qimgv_engine::WatchChangeKind::Added => QIMGV_MUT_ADDED,
                    qimgv_engine::WatchChangeKind::Modified => QIMGV_MUT_MODIFIED,
                    qimgv_engine::WatchChangeKind::Removed => QIMGV_MUT_REMOVED,
                    qimgv_engine::WatchChangeKind::Renamed { .. } => QIMGV_MUT_RENAMED,
                };
                if !mutation.is_null() {
                    unsafe {
                        *mutation = QimgvMutation {
                            kind: mkind,
                            is_dir: *is_dir as c_int,
                            size: *size,
                            mtime_ms: *mtime_ms,
                            index: *index as c_int,
                        };
                    }
                }
                unsafe {
                    *out = QimgvEvent { kind: QIMGV_EVENT_DIR_MUTATED, code: QIMGV_OK, epoch: *epoch as u32, seq: *seq as u32 };
                }
                Ok(1)
            }
            },
        }
        })();
        if outcome.is_err() {
            // Nothing was written: every arm checks the buffer before touching an
            // out-parameter. Putting the event back keeps take_event total — a
            // DirListed swallowed by a short buffer looks exactly like an open
            // that never answers, which is the worse of the two failures.
            *stash = pending;
        }
        outcome
    }));
    match r {
        Ok(Ok(n)) => n,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_take_event");
            QIMGV_ERR_PANIC
        }
    }
}
