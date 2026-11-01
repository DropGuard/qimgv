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

use qimgv_engine::{Command, Engine, EngineConfig, EngineError, Event, NameFilter, Notify, SortMode};

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
}

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

pub struct QimgvEngine(Mutex<Engine>);

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
    if buf.is_null() {
        return Err(QIMGV_ERR_INVALID);
    }
    let bytes = s.as_bytes();
    if bytes.len() + 1 > cap {
        return Err(QIMGV_ERR_TRUNCATED);
    }
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr().cast::<c_char>(), buf, bytes.len());
        buf.add(bytes.len()).write(0);
    }
    Ok(())
}

/// # Safety
/// `engine` must be a live pointer returned by `qimgv_engine_new`, not yet freed.
fn with_engine<T>(engine: *mut QimgvEngine, f: impl FnOnce(&Engine) -> T) -> T {
    let eng = unsafe { &*engine };
    let guard = eng.0.lock().unwrap_or_else(|p| p.into_inner());
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
            NameFilter::from_extensions([""])
        } else {
            let joined = unsafe { CStr::from_ptr(formats_extensions_utf8) }
                .to_str()
                .map_err(|_| "extension list is not valid UTF-8".to_string())?;
            NameFilter::from_extensions(joined.split('\n').filter(|s| !s.is_empty()))
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
        Ok(QimgvEngine(Mutex::new(engine)))
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
pub unsafe extern "C" fn qimgv_engine_open_dir(engine: *mut QimgvEngine, path: *const c_char) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        with_engine(engine, |e| {
            e.send(Command::OpenDir { path })
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

// --- events -----------------------------------------------------------------

/// Returns 1 if an event was written to `out`, 0 if none is pending, negative
/// on error. `dir_buf` receives the event's directory path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_take_event(
    engine: *mut QimgvEngine,
    out: *mut QimgvEvent,
    dir_buf: *mut c_char,
    dir_cap: usize,
) -> c_int {
    if engine.is_null() || out.is_null() {
        set_last_error("invalid argument");
        return QIMGV_ERR_INVALID;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        with_engine(engine, |e| match e.try_event() {
            None => Ok(0),
            Some(Event::DirListed { dir }) => {
                write_str(dir_buf, dir_cap, &dir.to_string_lossy())?;
                unsafe {
                    *out = QimgvEvent { kind: QIMGV_EVENT_DIR_LISTED, code: QIMGV_OK };
                }
                Ok(1)
            }
            Some(Event::OpenFailed { dir, error }) => {
                write_str(dir_buf, dir_cap, &dir.to_string_lossy())?;
                let code = map_engine_error(&error);
                set_last_error(&error.to_string());
                unsafe {
                    *out = QimgvEvent { kind: QIMGV_EVENT_OPEN_FAILED, code };
                }
                Ok(1)
            }
        })
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
