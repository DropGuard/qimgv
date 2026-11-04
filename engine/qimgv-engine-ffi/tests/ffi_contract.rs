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
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
        let mut buf = [0 as c_char; 512];
        let mut m = QimgvMutation { kind: 0, is_dir: 0, size: 0, mtime_ms: 0, index: 0 };
        let r = unsafe { qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len(), &mut m, std::ptr::null_mut()) };
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
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
        let mut buf = [0 as c_char; 64];
        let mut m = QimgvMutation { kind: 0, is_dir: 0, size: 0, mtime_ms: 0, index: 0 };
        assert_eq!(qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len(), &mut m, std::ptr::null_mut()), 0);
        qimgv_engine_free(engine);
    }
}

#[test]
fn argument_validation_and_last_error() {
    let flag = AtomicBool::new(false);
    let engine = new_engine(None, false, &flag);
    assert!(!engine.is_null());
    unsafe {
        assert_eq!(qimgv_engine_open_dir(engine, std::ptr::null(), 0, std::ptr::null_mut()), QIMGV_ERR_INVALID);
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
            qimgv_engine_open_dir(engine, bad.as_ptr() as *const c_char, 0, std::ptr::null_mut()),
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
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()) }, QIMGV_OK);

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
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
        let mut buf = [0 as c_char; 64];
        let mut m = QimgvMutation { kind: 0, is_dir: 0, size: 0, mtime_ms: 0, index: 0 };
        assert_eq!(qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len(), &mut m, std::ptr::null_mut()), 0);

        qimgv_engine_free(engine);
    }
}

#[test]
fn probe_via_ffi() {
    // minimal CRC-free PNG (parsers don't validate chunk CRCs)
    fn png_chunk(tag: &[u8], data: &[u8]) -> Vec<u8> {
        let mut out = (data.len() as u32).to_be_bytes().to_vec();
        out.extend_from_slice(tag);
        out.extend_from_slice(data);
        out
    }
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut ihdr = 64u32.to_be_bytes().to_vec();
    ihdr.extend_from_slice(&32u32.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    png.extend_from_slice(&png_chunk(b"IHDR", &ihdr));
    png.extend_from_slice(&png_chunk(b"IDAT", &[0]));
    png.extend_from_slice(&png_chunk(b"IEND", &[]));

    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("t.png");
    std::fs::write(&path, &png).unwrap();
    let path_c = CString::new(path.to_str().unwrap()).unwrap();

    let mut out = QimgvProbeResult { format: 0, animated: 0, orientation: 0, width: 0, height: 0 };
    unsafe {
        assert_eq!(qimgv_engine_probe(path_c.as_ptr(), &mut out), QIMGV_OK);
        assert_eq!(out.format, QIMGV_FMT_PNG);
        assert_eq!((out.width, out.height), (64, 32));
        assert_eq!(out.animated, 0);
        assert!(out.orientation >= 1 && out.orientation <= 8);

        assert_eq!(qimgv_engine_probe(path_c.as_ptr(), std::ptr::null_mut()), QIMGV_ERR_INVALID);
        assert_eq!(
            qimgv_engine_probe(c"\xff\xfe".as_ptr(), &mut out),
            QIMGV_ERR_BAD_UTF8
        );
        assert_eq!(
            qimgv_engine_probe(c"/nonexistent/qimgv-probe".as_ptr(), &mut out),
            QIMGV_ERR_IO
        );
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
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()) }, QIMGV_OK);
    take_event(engine, Duration::from_secs(5)).expect("no DirListed event");

    unsafe {
        assert_eq!(qimgv_engine_file_count(engine), 2, "txt filtered out, JPG matched case-insensitively");
        qimgv_engine_free(engine);
    }
}

#[test]
fn null_extension_list_accepts_everything_via_ffi() {
    // the NULL arm of qimgv_engine_new, documented as "accept everything". The
    // bridge passes NULL whenever Settings has no formats configured, and an
    // empty whitelist used to be read as "accept nothing" — every folder empty.
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("b.xyz"), b"x").unwrap();

    let flag = AtomicBool::new(false);
    let engine = new_engine(None, false, &flag);
    assert!(!engine.is_null());
    let path_c = CString::new(dir.path().to_str().unwrap()).unwrap();
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()) }, QIMGV_OK);
    take_event(engine, Duration::from_secs(5)).expect("no DirListed event");

    unsafe {
        assert_eq!(qimgv_engine_file_count(engine), 2, "NULL whitelist must not hide files");
        qimgv_engine_free(engine);
    }
}

// --- thumbnail scheduling across the C ABI ---------------------------------
// The dispatch/cancel *algorithm* is owned by
// engine/qimgv-engine/tests/thumb_tests.rs. These tests own only what the
// boundary can get wrong: how a path/size/crop triple is marshalled in through
// `thumb_request`/`thumb_done` and back out through `take_event`. A triple
// that does not round-trip identically leaks an in-flight slot forever, and
// the pipeline stalls silently at max_in_flight.

/// Takes one THUMB_NEXT event, returning (path, size, crop).
fn take_thumb(engine: *mut QimgvEngine, timeout: Duration) -> Option<(String, u32, bool)> {
    let deadline = Instant::now() + timeout;
    loop {
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
        let mut buf = [0 as c_char; 4096];
        let mut m = QimgvMutation { kind: 0, is_dir: 0, size: 0, mtime_ms: 0, index: 0 };
        let mut t = QimgvThumbReq { size: 0, crop: 0 };
        let r = unsafe {
            qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len(), &mut m, &mut t)
        };
        if r > 0 {
            assert_eq!(ev.kind, QIMGV_EVENT_THUMB_NEXT, "expected a thumbnail dispatch");
            let path = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned();
            return Some((path, t.size, t.crop != 0));
        }
        if r < 0 {
            let err = unsafe { CStr::from_ptr(qimgv_last_error()) }.to_string_lossy().into_owned();
            panic!("take_event failed while waiting for THUMB_NEXT: {err}");
        }
        if Instant::now() >= deadline {
            return None;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}

fn thumb_request(engine: *mut QimgvEngine, path: &str, size: u32, crop: bool, replace: bool) -> c_int {
    let c = CString::new(path).unwrap();
    unsafe {
        qimgv_engine_thumb_request(engine, c.as_ptr(), size, crop as c_int, replace as c_int)
    }
}

fn thumb_done(engine: *mut QimgvEngine, path: &str, size: u32, crop: bool) -> c_int {
    let c = CString::new(path).unwrap();
    unsafe { qimgv_engine_thumb_done(engine, c.as_ptr(), size, crop as c_int) }
}

fn thumb_engine(max_in_flight: u32) -> (AtomicBool, *mut QimgvEngine) {
    let flag = AtomicBool::new(false);
    let engine = new_engine(None, false, &flag);
    assert!(!engine.is_null());
    assert_eq!(unsafe { qimgv_engine_thumb_config(engine, max_in_flight) }, QIMGV_OK);
    (flag, engine)
}

#[test]
fn thumb_dispatch_marshals_path_size_and_crop() {
    let (_flag, engine) = thumb_engine(4);
    // crop is a c_int on the wire but a bool inside: 1 must come back as true
    assert_eq!(thumb_request(engine, "/imgs/a.jpg", 128, true, true), QIMGV_OK);
    let got = take_thumb(engine, Duration::from_secs(5)).expect("no THUMB_NEXT dispatched");
    assert_eq!(got, ("/imgs/a.jpg".to_string(), 128, true));
    unsafe { qimgv_engine_free(engine) };
}

#[test]
fn thumb_done_requires_the_same_triple_that_was_requested() {
    let (_flag, engine) = thumb_engine(1);
    assert_eq!(thumb_request(engine, "/a.jpg", 256, true, true), QIMGV_OK);
    assert!(take_thumb(engine, Duration::from_secs(5)).is_some(), "a.jpg should dispatch");

    assert_eq!(thumb_request(engine, "/b.jpg", 256, false, false), QIMGV_OK);
    // a done that differs in ANY of the three fields must not free the slot
    assert_eq!(thumb_done(engine, "/a.jpg", 512, true), QIMGV_OK);
    assert_eq!(thumb_done(engine, "/a.jpg", 256, false), QIMGV_OK);
    assert_eq!(thumb_done(engine, "/z.jpg", 256, true), QIMGV_OK);
    assert!(
        take_thumb(engine, Duration::from_millis(300)).is_none(),
        "a mismatched done must not open a slot"
    );

    // the exact triple drains, and only then does b.jpg dispatch
    assert_eq!(thumb_done(engine, "/a.jpg", 256, true), QIMGV_OK);
    let got = take_thumb(engine, Duration::from_secs(5)).expect("b.jpg stuck after a matching done");
    assert_eq!(got.0, "/b.jpg");
    unsafe { qimgv_engine_free(engine) };
}

#[test]
fn thumb_config_zero_still_dispatches_one() {
    // the engine clamps max_in_flight to >= 1; a caller passing 0 over the wire
    // must not be able to wedge the pipeline
    let (_flag, engine) = thumb_engine(0);
    assert_eq!(thumb_request(engine, "/a.jpg", 256, false, true), QIMGV_OK);
    assert!(take_thumb(engine, Duration::from_secs(5)).is_some(), "max_in_flight=0 must clamp to 1");
    unsafe { qimgv_engine_free(engine) };
}

#[test]
fn thumb_clear_drops_pending_but_keeps_in_flight_slots() {
    let (_flag, engine) = thumb_engine(1);
    assert_eq!(thumb_request(engine, "/a.jpg", 256, false, true), QIMGV_OK);
    assert!(take_thumb(engine, Duration::from_secs(5)).is_some());
    assert_eq!(thumb_request(engine, "/pending.jpg", 256, false, false), QIMGV_OK);

    assert_eq!(unsafe { qimgv_engine_thumb_clear(engine) }, QIMGV_OK);
    // /a.jpg is still in flight, so clearing must not hand out its slot
    assert_eq!(thumb_request(engine, "/b.jpg", 256, false, true), QIMGV_OK);
    assert!(take_thumb(engine, Duration::from_millis(300)).is_none(), "slot is still held by /a.jpg");
    assert!(
        thumb_done(engine, "/pending.jpg", 256, false) == QIMGV_OK,
        "done for a cleared request must be a no-op, not an error"
    );

    assert_eq!(thumb_done(engine, "/a.jpg", 256, false), QIMGV_OK);
    let got = take_thumb(engine, Duration::from_secs(5)).expect("b.jpg stuck after clear");
    assert_eq!(got.0, "/b.jpg");
    unsafe { qimgv_engine_free(engine) };
}

#[test]
fn thumb_request_rejects_null_and_empty_paths() {
    let (_flag, engine) = thumb_engine(2);
    unsafe {
        assert_eq!(
            qimgv_engine_thumb_request(engine, std::ptr::null(), 256, 0, 1),
            QIMGV_ERR_INVALID
        );
        assert_eq!(
            qimgv_engine_thumb_request(engine, c"".as_ptr(), 256, 0, 1),
            QIMGV_ERR_INVALID
        );
        assert_eq!(thumb_done(engine, "", 256, false), QIMGV_ERR_INVALID);
        assert!(!qimgv_last_error().is_null(), "an error must leave a diagnostic");
    }
    unsafe { qimgv_engine_free(engine) };
}

#[test]
fn thumb_event_is_delivered_with_a_null_thumb_out_param() {
    // mutation/thumb out params are documented optional: passing NULL must
    // consume the event normally, not fail or write through the null
    let (_flag, engine) = thumb_engine(4);
    assert_eq!(thumb_request(engine, "/a.jpg", 256, true, true), QIMGV_OK);
    let got = take_event(engine, Duration::from_secs(5)).expect("no event via null thumb param");
    assert_eq!(got.0, QIMGV_EVENT_THUMB_NEXT);
    assert_eq!(got.1, QIMGV_OK);
    assert_eq!(got.2, "/a.jpg", "path must still be marshalled");
    unsafe { qimgv_engine_free(engine) };
}

#[test]
fn thumb_round_trip_works_while_a_directory_open_is_in_flight() {
    // qimgv dispatches thumbnails for a folder while its listing is still being
    // scanned by the worker pool: the two flows share one event channel and must
    // neither deadlock nor lose each other. The scan is a thread and the dispatch
    // comes from the pump, so the two events arrive in either order.
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();

    let (_flag, engine) = thumb_engine(2);
    let path_c = CString::new(dir.path().to_str().unwrap()).unwrap();
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()) }, QIMGV_OK);
    assert_eq!(thumb_request(engine, "/a.jpg", 256, false, true), QIMGV_OK);

    let mut got_thumb = false;
    let mut got_listing = false;
    let deadline = Instant::now() + Duration::from_secs(5);
    while !(got_thumb && got_listing) {
        let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
        let mut buf = [0 as c_char; 4096];
        let mut m = QimgvMutation { kind: 0, is_dir: 0, size: 0, mtime_ms: 0, index: 0 };
        let mut t = QimgvThumbReq { size: 0, crop: 0 };
        let r = unsafe {
            qimgv_engine_take_event(engine, &mut ev, buf.as_mut_ptr(), buf.len(), &mut m, &mut t)
        };
        if r > 0 {
            let text = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned();
            match ev.kind {
                QIMGV_EVENT_THUMB_NEXT => {
                    assert_eq!(text, "/a.jpg");
                    assert_eq!(thumb_done(engine, "/a.jpg", 256, false), QIMGV_OK);
                    got_thumb = true;
                }
                QIMGV_EVENT_DIR_LISTED => {
                    assert!(text.starts_with(dir.path().to_str().unwrap()), "listed: {text}");
                    got_listing = true;
                }
                other => panic!("unexpected event kind {other} while interleaving"),
            }
        } else {
            assert!(r >= 0, "take_event failed: {}", unsafe {
                CStr::from_ptr(qimgv_last_error()) }.to_string_lossy());
            assert!(Instant::now() < deadline, "thumb={got_thumb} listing={got_listing}");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    // the open completed normally underneath all of that
    assert_eq!(unsafe { qimgv_engine_file_count(engine) }, 1);
    unsafe { qimgv_engine_free(engine) };
}

// --- sort codes --------------------------------------------------------------

fn file_names(engine: *mut QimgvEngine, count: usize) -> Vec<String> {
    (0..count)
        .map(|i| {
            let mut meta = QimgvEntryMeta { handle: 0, size: 0, mtime_ms: 0, is_dir: 0 };
            let mut name = [0 as c_char; 256];
            let mut path = [0 as c_char; 1024];
            let r = unsafe {
                qimgv_engine_file_entry_at(engine, i as c_int, &mut meta, name.as_mut_ptr(),
                                           name.len(), path.as_mut_ptr(), path.len())
            };
            assert_eq!(r, QIMGV_OK, "no entry at index {i}");
            unsafe { CStr::from_ptr(name.as_ptr()) }.to_string_lossy().into_owned()
        })
        .collect()
}

/// `set_sort` re-orders asynchronously on the engine thread: poll until the
/// listing reads as expected.
fn wait_for_names(engine: *mut QimgvEngine, expected: &[&str]) {
    let want: Vec<String> = expected.iter().map(|s| s.to_string()).collect();
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let got = file_names(engine, expected.len());
        if got == want {
            return;
        }
        assert!(Instant::now() < deadline, "resort timed out: got {got:?}, want {want:?}");
        std::thread::sleep(Duration::from_millis(10));
    }
}

#[test]
fn sort_codes_map_to_the_documented_orders() {
    // qimgv's SortingMode (settings.h), the wire codes (qimgv_engine.h) and
    // SortMode (sort.rs) are three hand-maintained orderings of the same six
    // modes. The codes are pinned literally here by behavior — the C++ side
    // asserts the enum->macro mapping and the macro values themselves, so a
    // renumbering on either end lands in a test. Sorting by the wrong column is
    // the kind of bug that never crashes and looks completely plausible, so name,
    // size and mtime are chosen to be mutually contradictory.
    let dir = tempfile::tempdir().unwrap();
    let t0 = std::time::SystemTime::UNIX_EPOCH + Duration::from_secs(1_700_000_000);
    // (name, size, mtime) chosen so that all six orders are different
    // permutations: name a<b<c, size a<c<b, time b<a<c.
    let layout = [("a.jpg", 10usize, t0 + Duration::from_secs(3600)),
                  ("b.jpg", 30, t0), // oldest
                  ("c.jpg", 20, t0 + Duration::from_secs(7200))]; // newest
    for &(name, size, mtime) in &layout {
        let p = dir.path().join(name);
        std::fs::write(&p, vec![b'x'; size]).unwrap();
        std::fs::File::open(&p).unwrap().set_modified(mtime).unwrap();
    }

    let flag = AtomicBool::new(false);
    let engine = new_engine(Some("jpg"), false, &flag);
    assert!(!engine.is_null());
    let path_c = CString::new(dir.path().to_str().unwrap()).unwrap();
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()) }, QIMGV_OK);
    take_event(engine, Duration::from_secs(5)).expect("no DirListed event");
    assert_eq!(file_names(engine, 3), vec!["a.jpg", "b.jpg", "c.jpg"], "sort rides with OpenDir");

    for (code, expected) in [
        (0, ["a.jpg", "b.jpg", "c.jpg"]), // QIMGV_SORT_NAME
        (1, ["c.jpg", "b.jpg", "a.jpg"]), // QIMGV_SORT_NAME_DESC
        (2, ["b.jpg", "a.jpg", "c.jpg"]), // QIMGV_SORT_TIME: oldest first
        (3, ["c.jpg", "a.jpg", "b.jpg"]), // QIMGV_SORT_TIME_DESC
        (4, ["a.jpg", "c.jpg", "b.jpg"]), // QIMGV_SORT_SIZE: smallest first
        (5, ["b.jpg", "c.jpg", "a.jpg"]), // QIMGV_SORT_SIZE_DESC
    ] {
        assert_eq!(unsafe { qimgv_engine_set_sort(engine, code) }, QIMGV_OK);
        wait_for_names(engine, &expected);
    }

    // an unknown code must be refused, not silently fall back to a sort order
    assert_eq!(unsafe { qimgv_engine_set_sort(engine, 6) }, QIMGV_ERR_BAD_SORT);
    unsafe { qimgv_engine_free(engine) };
}

// --- teardown ---------------------------------------------------------------

static NOTIFY_STARTED: AtomicBool = AtomicBool::new(false);
static NOTIFY_FINISHED: AtomicBool = AtomicBool::new(false);

/// Stands in for EngineBridge::notifyCb, which writes through a pointer to a
/// member of the C++ object that owns the engine. Such a callback must not be
/// running once qimgv_engine_free returns: ~EngineBridge frees the engine and
/// then closes its eventfd.
extern "C" fn slow_notify(_: *mut c_void) {
    NOTIFY_STARTED.store(true, Ordering::SeqCst);
    std::thread::sleep(Duration::from_millis(200));
    NOTIFY_FINISHED.store(true, Ordering::SeqCst);
}

#[test]
fn free_waits_for_a_notify_callback_that_is_already_running() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    let path_c = CString::new(dir.path().to_str().unwrap()).unwrap();

    let engine = unsafe { qimgv_engine_new(Some(slow_notify), std::ptr::null_mut(), 1, c"jpg".as_ptr()) };
    assert!(!engine.is_null());
    assert_eq!(unsafe { qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()) }, QIMGV_OK);

    // free while the callback is provably mid-flight
    let deadline = Instant::now() + Duration::from_secs(5);
    while !NOTIFY_STARTED.load(Ordering::SeqCst) {
        assert!(Instant::now() < deadline, "the notify callback never ran");
        std::thread::sleep(Duration::from_millis(5));
    }
    unsafe { qimgv_engine_free(engine) };

    assert!(NOTIFY_FINISHED.load(Ordering::SeqCst),
        "qimgv_engine_free returned while the caller's callback was still running");
}

#[test]
fn a_truncated_take_event_still_owes_the_same_event() {
    // take_event pops before it marshals. Without a place to put what it cannot
    // write, a caller holding a too-small buffer DESTROYS the event — and a
    // DirListed that never arrives is indistinguishable from an open that never
    // answers. This file owns C-ABI buffer semantics, so the guarantee is pinned
    // here rather than only in the C++ consumer's retry loop.
    let flag = AtomicBool::new(false);
    let engine = new_engine(Some("jpg"), true, &flag);

    // a subject longer than the tiny buffer below, in components the kernel
    // accepts (255 bytes each)
    let dir = tempfile::tempdir().unwrap();
    let mut deep = dir.path().to_path_buf();
    for _ in 0..3 {
        deep = deep.join("d".repeat(200));
    }
    std::fs::create_dir_all(&deep).unwrap();
    let path_c = CString::new(deep.to_str().unwrap()).unwrap();
    unsafe {
        assert_eq!(qimgv_engine_open_dir(engine, path_c.as_ptr(), 0, std::ptr::null_mut()), QIMGV_OK);
    }
    assert!(wait_flag(&flag, Duration::from_secs(5)), "no notify for the open");

    let mut ev = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
    let mut tiny = [0 as c_char; 8];
    let mut m = QimgvMutation { kind: 0, is_dir: 0, size: 0, mtime_ms: 0, index: 0 };
    let r = unsafe {
        qimgv_engine_take_event(engine, &mut ev, tiny.as_mut_ptr(), tiny.len(), &mut m, std::ptr::null_mut())
    };
    assert_eq!(r, QIMGV_ERR_TRUNCATED, "a short buffer must report truncation, got {r}");

    // and the event is still owed: the same call with room to spare must return it
    let mut ev2 = QimgvEvent { kind: QIMGV_EVENT_NONE, code: QIMGV_OK, epoch: 0, seq: 0 };
    let mut big = vec![0 as c_char; 4096];
    let deadline = Instant::now() + Duration::from_secs(5);
    let kind = loop {
        let rr = unsafe {
            qimgv_engine_take_event(engine, &mut ev2, big.as_mut_ptr(), big.len(), &mut m, std::ptr::null_mut())
        };
        assert!(rr != QIMGV_ERR_TRUNCATED, "the retry with a large buffer still truncated");
        if rr > 0 {
            break ev2.kind;
        }
        assert!(Instant::now() < deadline, "the truncated event was lost, not deferred");
        std::thread::sleep(Duration::from_millis(5));
    };
    assert_eq!(kind, QIMGV_EVENT_DIR_LISTED);
    let written = unsafe { CStr::from_ptr(big.as_ptr()) }.to_string_lossy().into_owned();
    assert_eq!(written, deep.to_str().unwrap(), "the deferred event must still name its directory");

    unsafe { qimgv_engine_free(engine) };
}
