//! Thumbnail scheduler tests: batch replacement (viewport cancellation),
//! in-flight accounting, dedup and clear.

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use qimgv_engine::{Command, Engine, EngineConfig, Event, NameFilter, SortMode, ThumbReq};

fn config() -> EngineConfig {
    EngineConfig {
        scan_threads: 1,
        show_hidden: Arc::new(AtomicBool::new(false)),
        filter: NameFilter::from_extensions(["jpg"]),
        initial_sort: SortMode::Name,
    }
}

fn req(path: &str, size: u32) -> ThumbReq {
    ThumbReq { path: PathBuf::from(format!("/{path}")), size, crop: false }
}

/// Drains all pending ThumbNext events.
fn drain(engine: &Engine) -> Vec<ThumbReq> {
    let mut out = Vec::new();
    while let Some(Event::ThumbNext { req }) = engine.try_event() {
        out.push(req);
    }
    out
}

/// Accumulates dispatches until `pred` holds on the accumulated set.
fn wait_until(
    engine: &Engine,
    acc: &mut Vec<ThumbReq>,
    pred: impl Fn(&[ThumbReq]) -> bool,
    secs: u64,
) {
    let deadline = Instant::now() + Duration::from_secs(secs);
    loop {
        acc.extend(drain(engine));
        if pred(acc) {
            return;
        }
        assert!(Instant::now() < deadline, "timed out waiting for thumbnail dispatch");
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn batch_replace_and_in_flight_accounting() {
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = notify_tx.send(());
    }));
    engine.send(Command::ThumbConfig { max_in_flight: 2 }).unwrap();

    // viewport batch of 4: the first request carries replace_batch
    engine.send(Command::ThumbRequest { req: req("a.jpg", 256), replace_batch: true }).unwrap();
    engine.send(Command::ThumbRequest { req: req("b.jpg", 256), replace_batch: false }).unwrap();
    engine.send(Command::ThumbRequest { req: req("c.jpg", 256), replace_batch: false }).unwrap();
    engine.send(Command::ThumbRequest { req: req("d.jpg", 256), replace_batch: false }).unwrap();

    let mut acc = Vec::new();
    wait_until(&engine, &mut acc, |d| d.len() == 2, 5); // max_in_flight
    let mut names: Vec<String> =
        acc.iter().map(|r| r.path.to_string_lossy().into_owned()).collect();
    names.sort();
    assert_eq!(names, ["/a.jpg", "/b.jpg"], "dispatch order follows batch order");

    // one decode finishes → exactly one more dispatch, in batch order
    engine.send(Command::ThumbDone { req: req("a.jpg", 256) }).unwrap();
    wait_until(&engine, &mut acc, |d| d.len() == 3, 5);
    assert_eq!(acc[2].path.to_string_lossy(), "/c.jpg");

    // viewport moves: pending batch replaced — d.jpg must never dispatch
    engine.send(Command::ThumbRequest { req: req("e.jpg", 256), replace_batch: true }).unwrap();
    engine.send(Command::ThumbDone { req: req("b.jpg", 256) }).unwrap();
    wait_until(&engine, &mut acc, |d| d.len() == 4, 5);
    assert_eq!(acc[3].path.to_string_lossy(), "/e.jpg");

    engine.send(Command::ThumbDone { req: req("c.jpg", 256) }).unwrap();
    engine.send(Command::ThumbDone { req: req("e.jpg", 256) }).unwrap();
    std::thread::sleep(Duration::from_millis(150));
    let tail = drain(&engine);
    assert!(
        !tail.iter().any(|r| r.path.to_string_lossy() == "/d.jpg"),
        "stale d.jpg must not dispatch after batch replace"
    );
}

#[test]
fn duplicate_requests_dedup() {
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = notify_tx.send(());
    }));
    engine.send(Command::ThumbConfig { max_in_flight: 8 }).unwrap();
    engine.send(Command::ThumbRequest { req: req("a.jpg", 256), replace_batch: true }).unwrap();
    engine.send(Command::ThumbRequest { req: req("a.jpg", 256), replace_batch: false }).unwrap();
    let mut acc = Vec::new();
    wait_until(&engine, &mut acc, |d| !d.is_empty(), 5);
    assert_eq!(acc.len(), 1, "same (path,size,crop) must dedup");
    engine.send(Command::ThumbDone { req: req("a.jpg", 256) }).unwrap();
    std::thread::sleep(Duration::from_millis(150));
    assert!(drain(&engine).is_empty(), "duplicate must not re-dispatch");
}

#[test]
fn clear_drops_pending() {
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = notify_tx.send(());
    }));
    engine.send(Command::ThumbConfig { max_in_flight: 1 }).unwrap();
    engine.send(Command::ThumbRequest { req: req("a.jpg", 256), replace_batch: true }).unwrap();
    engine.send(Command::ThumbRequest { req: req("b.jpg", 256), replace_batch: false }).unwrap();
    let mut acc = Vec::new();
    wait_until(&engine, &mut acc, |d| d.len() == 1, 5);
    assert_eq!(acc[0].path.to_string_lossy(), "/a.jpg");

    engine.send(Command::ThumbClear).unwrap();
    engine.send(Command::ThumbDone { req: req("a.jpg", 256) }).unwrap();
    std::thread::sleep(Duration::from_millis(150));
    assert!(drain(&engine).is_empty(), "cleared batch must not dispatch");
}
