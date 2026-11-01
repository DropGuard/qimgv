//! Contract tests for the C ABI surface, run from the Rust side.
//!
//! Ownership rule (agreed): business semantics live in the qimgv-engine
//! tests; THIS file owns the C-ABI semantics — status codes, buffer
//! truncation, null rejection, event marshalling. The C++ consumption side
//! (symbols, struct layout, linkage) is verified by
//! qimgv/tests/test_engine_contract.cpp. One behavior, one owner.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use qimgv_engine_ffi::*;

extern "C" fn set_flag(user: *mut c_void) {
    let flag = unsafe { &*(user as *const AtomicBool) };
    flag.store(true, Ordering::SeqCst);
}

fn wait_flag(flag: &AtomicBool, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while !flag.load(Ordering::SeqCst) {
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    true
}

/// Drains events until one arrives or the deadline passes.
/// Returns (kind, code, dir) of the first event, if any.
fn take_event(engine: *mut QimgvEngine, timeout: Duration) -> Option<(c_int, c_int, String)> {
    let deadline = Instant::now() + timeout;
    loop {
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK };
        let mut buf = [0 as c_char; 512];
        let r = unsafe { qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len()) };
        if r > 0 {
            let dir = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned();
            return Some((ev.kind, ev.code, dir));
        }
        if r < 0 || Instant::now() >= deadline {
            return None;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}

fn new_engine(extensions: Option<&str>, notify: bool, flag: &AtomicBool) -> *mut QimgvEngine {
    let exts = extensions.map(CString::new).map(Result::unwrap);
    unsafe {
        qimgv_engine_new(
            if notify { Some(set_flag) } else { None },
            flag as *const AtomicBool as *mut c_void,
            2,
            exts.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
        )
    }
}

#[test]
fn version_and_error_symbols_are_valid() {
    let v = unsafe { CStr::from_ptr(qimgv_engine_version()) };
    assert!(!v.to_str().unwrap().is_empty());
}

#[test]
fn fresh_engine_state_and_free() {
    let flag = AtomicBool::new(false);
    let engine = new_engine(None, true, &flag);
    assert!(!engine.is_null(), "qimgv_engine_new failed: {:?}", {
        let p = qimgv_last_error();
        if p.is_null() { None } else { Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()) }
    });

    assert!(!wait_flag(&flag, Duration::from_secs(1)), "no notify without events");

    unsafe {
        assert_eq!(qimgv_engine_file_count(engine), 0);
        assert_eq!(qimgv_engine_dir_count(engine), 0);
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK };
        let mut buf = [0 as c_char; 64];
        assert_eq!(qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len()), 0);
        qimgv_engine_free(engine);
    }
}

#[test]
fn argument_validation_and_last_error() {
    let flag = AtomicBool::new(false);
    let engine = new_engine(None, false, &flag);
    assert!(!engine.is_null());
    unsafe {
        assert_eq!(qimgv_engine_open_dir(engine, std::ptr::null()), QIMGV_ERR_INVALID);
        assert_eq!(qimgv_engine_set_sort(engine, 99), QIMGV_ERR_BAD_SORT);

        // no directory opened yet → any index is out of range
        let mut meta = QimgvEntryMeta { handle: 0, size: 0, mtime_ms: 0, is_dir: 0 };
        let mut name = [0 as c_char; 64];
        let mut path = [0 as c_char; 512];
        assert_eq!(
            qimgv_engine_file_entry_at(engine, 0, &mut meta, name.as_mut_ptr(), name.len(), path.as_mut_ptr(), path.len()),
            QIMGV_ERR_EMPTY
        );

        // all-zero handle is the documented null handle
        let mut out = 0u64;
        assert_eq!(qimgv_engine_next_file(engine, 0, &mut out), QIMGV_ERR_INVALID);
        // non-zero garbage handle decodes, then fails lookup
        assert_eq!(qimgv_engine_next_file(engine, 0xdead_beef, &mut out), QIMGV_ERR_EMPTY);

        // an error must leave a readable diagnostic
        assert!(!qimgv_last_error().is_null());
        qimgv_engine_free(engine);
    }
}

#[test]
fn bad_utf8_path_rejected() {
    let flag = AtomicBool::new(false);
    let engine = new_engine(None, false, &flag);
    assert!(!engine.is_null());
    // "\xff\xfe" is not valid UTF-8; trailing NUL terminates the C string
    let bad = b"\xff\xfe\x00";
    unsafe {
        assert_eq!(
            qimgv_engine_open_dir(engine, bad.as_ptr() as *const c_char),
            QIMGV_ERR_BAD_UTF8
        );
        qimgv_engine_free(engine);
    }
}

#[test]
fn open_dir_event_queries_and_truncation() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("b.jpg"), b"x").unwrap();

    let flag = AtomicBool::new(false);
    let engine = new_engine(Some("jpg\npng"), true, &flag);
    assert!(!engine.is_null());

    let path_c = CString::new(dir.path().to_str().unwrap()).unwrap();
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr()) }, QIMGV_OK);

    assert!(wait_flag(&flag, Duration::from_secs(5)), "notify not fired");
    let (kind, code, ev_dir) =
        take_event(engine, Duration::from_secs(5)).expect("no DirListed event");
    assert_eq!(kind, QIMGV_EVENT_DIR_LISTED);
    assert_eq!(code, QIMGV_OK);
    assert!(ev_dir.starts_with(dir.path().to_str().unwrap()), "event dir: {ev_dir}");

    unsafe {
        assert_eq!(qimgv_engine_file_count(engine), 2);
        assert_eq!(qimgv_engine_dir_count(engine), 0);

        let mut meta = QimgvEntryMeta { handle: 0, size: 0, mtime_ms: 0, is_dir: 0 };
        let mut name = [0 as c_char; 64];
        let mut path = [0 as c_char; 512];
        assert_eq!(
            qimgv_engine_file_entry_at(engine, 0, &mut meta, name.as_mut_ptr(), name.len(), path.as_mut_ptr(), path.len()),
            QIMGV_OK
        );
        assert_eq!(CStr::from_ptr(name.as_ptr()).to_str().unwrap(), "a.jpg");
        let h0 = meta.handle;

        assert_eq!(
            qimgv_engine_file_entry_at(engine, 1, &mut meta, name.as_mut_ptr(), name.len(), path.as_mut_ptr(), path.len()),
            QIMGV_OK
        );
        let h1 = meta.handle;

        let mut out = 0u64;
        assert_eq!(qimgv_engine_next_file(engine, h0, &mut out), QIMGV_OK);
        assert_eq!(out, h1, "next_file must follow natural sort order");
        assert_eq!(qimgv_engine_prev_file(engine, h0, &mut out), QIMGV_ERR_EMPTY);

        // name buffer too small for "a.jpg\0" (6 bytes needed, 2 given)
        assert_eq!(
            qimgv_engine_file_entry_at(engine, 0, &mut meta, name.as_mut_ptr(), 2, path.as_mut_ptr(), path.len()),
            QIMGV_ERR_TRUNCATED
        );

        // the single DirListed event was already drained
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK };
        let mut buf = [0 as c_char; 64];
        assert_eq!(qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len()), 0);

        qimgv_engine_free(engine);
    }
}

#[test]
fn extension_filter_via_ffi() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("keep.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("drop.txt"), b"x").unwrap();
    std::fs::write(dir.path().join("UPPER.JPG"), b"x").unwrap(); // case-insensitive

    let flag = AtomicBool::new(false);
    let engine = new_engine(Some("jpg"), true, &flag);
    assert!(!engine.is_null());

    let path_c = CString::new(dir.path().to_str().unwrap()).unwrap();
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr()) }, QIMGV_OK);
    take_event(engine, Duration::from_secs(5)).expect("no DirListed event");

    unsafe {
        assert_eq!(qimgv_engine_file_count(engine), 2, "txt filtered out, JPG matched case-insensitively");
        qimgv_engine_free(engine);
    }
}
