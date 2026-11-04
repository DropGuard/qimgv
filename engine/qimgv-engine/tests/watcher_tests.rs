//! Watcher tests: mask classification, watch attribution, state application,
//! and live watching through a real engine. The overflow path is tested
//! synthetically (the kernel only emits IN_Q_OVERFLOW under memory pressure we
//! can't trigger deterministically without root).

use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use qimgv_engine::watcher::{
    apply_event, classify, event_is_live, is_dir_mask, RawWatchEvent, RawWatchKind, WatchAction,
};
use qimgv_engine::{Command, Engine, EngineConfig, NameFilter, SortMode};

// --- mask classification ------------------------------------------------------

#[test]
fn classify_maps_masks_and_skips_bookkeeping() {
    assert_eq!(classify(0x100), Some(RawWatchKind::Created));
    assert_eq!(classify(0x2), Some(RawWatchKind::Modified));
    assert_eq!(classify(0x200), Some(RawWatchKind::Deleted));
    assert_eq!(classify(0x40), Some(RawWatchKind::MovedFrom));
    assert_eq!(classify(0x80), Some(RawWatchKind::MovedTo));
    assert_eq!(classify(0x4000), Some(RawWatchKind::Overflow));
    assert_eq!(classify(0x400), Some(RawWatchKind::WatchGone));
    assert_eq!(classify(0x800), Some(RawWatchKind::WatchGone));
    // IN_IGNORED is the kernel telling us a watch we already removed is gone
    assert_eq!(classify(0x8000), None);
    // ACCESS/OPEN/CLOSE and friends move no listing row
    assert_eq!(classify(0x1), None);
    assert_eq!(classify(0x8), None);
    // a directory flag must not change the kind it classifies to
    assert_eq!(classify(0x4000_0100), Some(RawWatchKind::Created));
    assert!(is_dir_mask(0x4000_0100));
    assert!(!is_dir_mask(0x100));
}

// --- which watch an event belongs to -----------------------------------------

#[test]
fn events_of_a_watch_that_is_no_longer_installed_are_dropped() {
    // The production shape of the bug this pins: a delete for /A is delivered
    // after the watch moved to /B. With the record's subject discarded, the
    // consumer joined the name onto its *current* directory and removed a file
    // that exists — /B/img1.jpg — so a listing lost a row nobody deleted.
    assert!(event_is_live(7, RawWatchKind::Deleted, Some(7)));
    assert!(!event_is_live(7, RawWatchKind::Deleted, Some(9)));
    assert!(!event_is_live(7, RawWatchKind::Created, None));
    assert!(!event_is_live(7, RawWatchKind::WatchGone, Some(9)));
    // IN_Q_OVERFLOW carries wd == -1: it names no watch, it describes the
    // queue. Dropping it would stop the engine reconciling after a kernel-side
    // loss forever, which is worse than a stale event.
    assert!(event_is_live(-1, RawWatchKind::Overflow, Some(9)));
    assert!(event_is_live(-1, RawWatchKind::Overflow, None));
}

// --- state application --------------------------------------------------------

/// A raw event as the watcher thread would emit it for `dir`.
fn raw_event(dir: &std::path::Path, name: Option<&str>, is_dir: bool, kind: RawWatchKind) -> RawWatchEvent {
    RawWatchEvent {
        wd: 1,
        dir: dir.to_path_buf(),
        name: name.map(str::to_owned),
        is_dir,
        kind,
    }
}

fn state_with_file() -> (qimgv_engine::DirectoryState, tempfile::TempDir, std::path::PathBuf) {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    let mut st = qimgv_engine::DirectoryState::new(SortMode::Name);
    st.replace_all(
        dir.path().to_owned(),
        vec![qimgv_engine::EntryData {
            name: "a.jpg".into(),
            path: dir.path().join("a.jpg").to_string_lossy().into_owned(),
            is_dir: false,
            size: 1,
            mtime: std::time::UNIX_EPOCH,
        }],
        vec![],
    );
    let p = dir.path().join("a.jpg");
    (st, dir, p)
}

#[test]
fn apply_created_respects_filter_and_adds() {
    let (mut st, dir, _p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    // filtered out
    std::fs::write(dir.path().join("skip.txt"), b"x").unwrap();
    let ev = raw_event(dir.path(), Some("skip.txt"), false, RawWatchKind::Created);
    assert!(matches!(apply_event(&mut st, &filter, &mut overrides, &ev), WatchAction::Ignored));
    assert_eq!(st.file_count(), 1);

    // allowed → added
    std::fs::write(dir.path().join("b.jpg"), b"xx").unwrap();
    let ev = raw_event(dir.path(), Some("b.jpg"), false, RawWatchKind::Created);
    match apply_event(&mut st, &filter, &mut overrides, &ev) {
        WatchAction::Changed { kind: qimgv_engine::watcher::Applied::Added, path, .. } => {
            assert!(path.ends_with("b.jpg"));
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 2);
}

#[test]
fn apply_created_adds_directories_even_though_the_filter_rejects_their_names() {
    // The scanner and the watcher have to agree: scan.rs lists every directory
    // regardless of the extension whitelist, so a name like "newfolder" must still
    // reach the listing. Otherwise a folder created outside the app stays invisible.
    let (mut st, dir, _p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    std::fs::create_dir(dir.path().join("newfolder")).unwrap();
    let ev = raw_event(dir.path(), Some("newfolder"), true, RawWatchKind::Created);
    match apply_event(&mut st, &filter, &mut overrides, &ev) {
        WatchAction::Changed { kind: qimgv_engine::watcher::Applied::Added, path, is_dir, .. } => {
            assert!(path.ends_with("newfolder"));
            assert!(is_dir, "the mutation must still report it as a directory");
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.dir_count(), 1);
    assert_eq!(st.file_count(), 1, "the pre-existing file is untouched");
}

#[test]
fn apply_deleted_and_modified() {
    let (mut st, dir, p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    // modify with fresh mtime → Changed{Modified}
    std::fs::write(&p, b"new content").unwrap();
    let ev = raw_event(dir.path(), Some("a.jpg"), false, RawWatchKind::Modified);
    assert!(matches!(
        apply_event(&mut st, &filter, &mut overrides, &ev),
        WatchAction::Changed { kind: qimgv_engine::watcher::Applied::Modified, .. }
    ));

    // delete → Changed{Removed}
    std::fs::remove_file(&p).unwrap();
    let ev = raw_event(dir.path(), Some("a.jpg"), false, RawWatchKind::Deleted);
    match apply_event(&mut st, &filter, &mut overrides, &ev) {
        WatchAction::Changed { kind: qimgv_engine::watcher::Applied::Removed, path, .. } => {
            assert!(path.ends_with("a.jpg"));
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 0);
}

#[test]
fn apply_overflow_requests_reconcile() {
    let (mut st, dir, _p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();
    let ev = raw_event(dir.path(), None, false, RawWatchKind::Overflow);
    assert_eq!(
        apply_event(&mut st, &filter, &mut overrides, &ev),
        WatchAction::Reconcile { dir: dir.path().to_path_buf() }
    );
}

#[test]
fn a_delete_from_another_directory_cannot_empty_this_one() {
    // The listing's only writer is the engine, so a row disappears when the
    // event that removes it resolves to a path that is in the listing. This is
    // the shape of the production bug: /A's delete arriving after the watch
    // moved to /B. Because apply_event joins onto the directory *named by the
    // event*, /A/a.jpg is simply not in /B's listing — and a name that happens
    // to match is not enough.
    let (mut st, _dir, _p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();
    let other = tempfile::tempdir().unwrap();

    let ev = raw_event(other.path(), Some("a.jpg"), false, RawWatchKind::Deleted);
    assert_eq!(apply_event(&mut st, &filter, &mut overrides, &ev), WatchAction::Ignored);
    assert_eq!(st.file_count(), 1, "the row for /B/a.jpg survived a delete meant for /A");

    // and a create named elsewhere lands where it was created, not here
    std::fs::write(other.path().join("c.jpg"), b"x").unwrap();
    let ev = raw_event(other.path(), Some("c.jpg"), false, RawWatchKind::Created);
    match apply_event(&mut st, &filter, &mut overrides, &ev) {
        WatchAction::Changed { path, .. } => panic!("a foreign create must not add a row: {path}"),
        WatchAction::Ignored => {}
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 1);
}

// --- live watching through a real engine ---------------------------------------

fn config() -> EngineConfig {
    EngineConfig {
        scan_threads: 2,
        show_hidden: Arc::new(AtomicBool::new(false)),
        filter: NameFilter::from_extensions(["jpg"]),
        initial_sort: SortMode::Name,
    }
}

fn wait_for(engine: &Engine, pred: impl Fn(&qimgv_engine::DirectoryState) -> bool, secs: u64) {
    let deadline = std::time::Instant::now() + Duration::from_secs(secs);
    loop {
        if pred(&engine.state.read().unwrap()) {
            return;
        }
        assert!(std::time::Instant::now() < deadline, "timed out waiting on engine state");
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn live_watch_applies_creates_and_deletes() {
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = notify_tx.send(());
    }));

    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    engine.send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    wait_for(&engine, |st| st.file_count() == 1, 10);

    // create → watch → state gains the entry without a rescan
    std::fs::write(dir.path().join("b.jpg"), b"x").unwrap();
    wait_for(&engine, |st| st.file_count() == 2, 10);

    // delete → gone
    std::fs::remove_file(dir.path().join("b.jpg")).unwrap();
    wait_for(&engine, |st| st.file_count() == 1, 10);
}

#[test]
fn live_watch_burst_keeps_up() {
    // the drain discipline: 2000 creates in a burst must all land in the
    // state (the kernel queue must not lose events at this scale)
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = notify_tx.send(());
    }));

    let dir = tempfile::tempdir().unwrap();
    engine.send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    wait_for(&engine, |st| st.dir() == Some(dir.path()), 10);

    for i in 0..2000 {
        std::fs::write(dir.path().join(format!("f{i:05}.jpg")), b"x").unwrap();
    }
    wait_for(&engine, |st| st.file_count() == 2000, 60);
}

#[test]
fn include_path_bypasses_filter_and_survives_rescan() {
    let (notify_tx, _notify_rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = notify_tx.send(());
    }));

    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("mime-ok.xyz"), b"x").unwrap();
    engine.send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() }).unwrap();
    wait_for(&engine, |st| st.file_count() == 1, 10);

    engine.send(Command::IncludePath(dir.path().join("mime-ok.xyz"))).unwrap();
    wait_for(&engine, |st| st.file_count() == 2, 10);

    // rescan must keep the override (filter still doesn't match)
    engine.send(Command::SetSort(SortMode::NameDesc)).unwrap();
    wait_for(&engine, |st| st.sort() == SortMode::NameDesc, 10);
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(engine.state.read().unwrap().file_count(), 2, "override lost on resort");
}
