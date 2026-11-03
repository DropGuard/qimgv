use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use qimgv_engine::{Command, Engine, EngineConfig, Event, NameFilter, SortMode};

fn config(extensions: &[&str]) -> EngineConfig {
    EngineConfig {
        scan_threads: 2,
        show_hidden: Arc::new(AtomicBool::new(false)),
        filter: NameFilter::from_extensions(extensions.to_vec()),
        initial_sort: SortMode::Name,
    }
}

/// Waits until `pred` holds on the engine state, with a deadline.
fn wait_for(engine: &Engine, pred: impl Fn(&qimgv_engine::DirectoryState) -> bool) {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if pred(&engine.state.read().unwrap()) {
            return;
        }
        assert!(Instant::now() < deadline, "timed out waiting on engine state");
        std::thread::sleep(Duration::from_millis(5));
    }
}

fn make_dir_with(ext: &str, count: usize) -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    for i in 0..count {
        std::fs::write(dir.path().join(format!("img{i:03}.{ext}")), b"x").unwrap();
    }
    dir
}

#[test]
fn open_dir_lists_filtered_files() {
    let (notify_tx, notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(
        config(&["jpg"]),
        Arc::new(move || {
            let _ = notify_tx.send(());
        }),
    );

    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("b.png"), b"x").unwrap(); // filtered out
    std::fs::write(dir.path().join(".hidden.jpg"), b"x").unwrap(); // hidden
    std::fs::create_dir(dir.path().join("subdir")).unwrap();

    engine.send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    notify_rx.recv_timeout(Duration::from_secs(5)).expect("no notify");

    let ev = engine.try_event().expect("no event");
    assert!(matches!(ev, Event::DirListed { .. }));
    let st = engine.state.read().unwrap();
    assert_eq!(st.dir(), Some(dir.path()));
    assert_eq!(st.file_count(), 1);
    assert_eq!(st.dir_count(), 1);
    assert_eq!(st.file_at(0).unwrap().data.name, "a.jpg");
}

#[test]
fn empty_extension_list_accepts_everything() {
    // qimgv_engine.h documents a NULL / empty whitelist as "accept everything".
    // Reading it as "accept nothing" makes every folder look empty instead.
    let engine = Engine::spawn(config(&[]), Arc::new(|| {}));
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.xyz"), b"x").unwrap();
    std::fs::write(dir.path().join("b.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("noextension"), b"x").unwrap();

    engine.send(Command::OpenDir {
        path: dir.path().to_owned(),
        sort: None,
        epoch: engine.next_epoch(),
    })
    .unwrap();
    wait_for(&engine, |st| st.file_count() == 3);
    assert_eq!(engine.state.read().unwrap().file_count(), 3);
}

#[test]
fn open_failed_event_on_missing_dir() {
    let (notify_tx, notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(
        config(&["jpg"]),
        Arc::new(move || {
            let _ = notify_tx.send(());
        }),
    );

    let missing = std::env::temp_dir().join("qimgv-engine-test-does-not-exist-42");
    engine.send(Command::OpenDir { path: missing.clone(), sort: None, epoch: engine.next_epoch() }).unwrap();
    notify_rx.recv_timeout(Duration::from_secs(5)).expect("no notify");

    match engine.try_event().expect("no event") {
        Event::OpenFailed { dir, .. } => assert_eq!(dir, missing),
        other => panic!("unexpected event: {other:?}"),
    }
}

#[test]
fn rapid_open_dir_keeps_only_latest() {
    // Two OpenDirs fired back-to-back: whichever scan finishes second must
    // win; the stale result must never land in the state.
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(
        config(&["jpg"]),
        Arc::new(move || {
            let _ = notify_tx.send(());
        }),
    );

    let dir_a = make_dir_with("jpg", 1);
    let dir_b = make_dir_with("jpg", 3);

    engine.send(Command::OpenDir { path: dir_a.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    engine.send(Command::OpenDir { path: dir_b.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();

    wait_for(&engine, |st| st.dir() == Some(dir_b.path()) && st.file_count() == 3);
    // drain whatever events accumulated; none may reference dir_a's listing
    while let Some(ev) = engine.try_event() {
        if let Event::DirListed { dir, .. } = ev {
            assert_eq!(dir, dir_b.path());
        }
    }
}

#[test]
fn set_sort_reorders_and_notifies() {
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(
        config(&["jpg"]),
        Arc::new(move || {
            let _ = notify_tx.send(());
        }),
    );

    let dir = make_dir_with("jpg", 4);
    engine.send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    wait_for(&engine, |st| st.file_count() == 4);

    engine.send(Command::SetSort(SortMode::NameDesc)).unwrap();
    wait_for(&engine, |st| st.sort() == SortMode::NameDesc);

    let names: Vec<String> =
        (0..4).map(|i| st_names(&engine, i)).collect();
    let mut sorted = names.clone();
    sorted.sort();
    sorted.reverse();
    assert_eq!(names, sorted);
}

fn st_names(engine: &Engine, i: usize) -> String {
    engine.state.read().unwrap().file_at(i).unwrap().data.name.clone()
}

#[test]
fn mutated_event_carries_the_engine_position() {
    // The mirror splices at the engine's position; the wire must actually
    // carry it, and it must agree with where the model put the entry. State
    // polling, not notify timing, drives the waits — the same discipline the
    // live-watch tests use.
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(
        config(&["jpg"]),
        Arc::new(move || {
            let _ = notify_tx.send(());
        }),
    );

    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    engine.send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    wait_for(&engine, |st| st.file_count() == 1);

    std::fs::write(dir.path().join("b.jpg"), b"xx").unwrap();
    wait_for(&engine, |st| st.file_count() == 2);

    // drain the queued events down to the Added mutation: the create may also
    // echo a modify for the same write, and the DirListed from the open is
    // still queued ahead of both
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    loop {
        match engine.try_event() {
            Some(Event::Mutated { kind: qimgv_engine::WatchChangeKind::Added, index, is_dir, .. }) => {
                assert_eq!(index, 1, "b.jpg sits after a.jpg in natural order");
                assert!(!is_dir);
                break;
            }
            Some(_) => continue,
            None => {
                assert!(std::time::Instant::now() < deadline, "no Added mutation event within 5s");
                std::thread::sleep(Duration::from_millis(5));
            }
        }
    }
    let st = engine.state.read().unwrap();
    assert_eq!(st.file_at(1).unwrap().data.name, "b.jpg");
}
