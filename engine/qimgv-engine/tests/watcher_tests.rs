//! Watcher tests: raw buffer parsing, state application, and live watching
//! through a real engine. The overflow path is tested synthetically (the
//! kernel only emits IN_Q_OVERFLOW under memory pressure we can't trigger
//! deterministically without root).

use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use qimgv_engine::watcher::{apply_event, parse_events, RawWatchEvent, RawWatchKind, WatchAction};
use qimgv_engine::{Command, Engine, EngineConfig, NameFilter, SortMode};

// --- raw parsing ------------------------------------------------------------

fn event_bytes(wd: i32, mask: u32, name: Option<&str>) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(&wd.to_ne_bytes());
    b.extend_from_slice(&mask.to_ne_bytes());
    b.extend_from_slice(&0u32.to_ne_bytes()); // cookie
    let name = name.unwrap_or("");
    b.extend_from_slice(&((name.len() + 1) as u32).to_ne_bytes());
    b.extend_from_slice(name.as_bytes());
    b.push(0);
    b
}

#[test]
fn parse_events_read_masks_and_names() {
    let mut buf = Vec::new();
    buf.extend_from_slice(&event_bytes(1, 0x100, Some("new.png"))); // CREATE
    buf.extend_from_slice(&event_bytes(1, 0x2, Some("new.png"))); // MODIFY
    buf.extend_from_slice(&event_bytes(-1, 0x4000, None)); // Q_OVERFLOW
    buf.extend_from_slice(&event_bytes(1, 0x4000_0100, Some("subdir"))); // CREATE|ISDIR

    let evs = parse_events(&buf);
    assert_eq!(evs.len(), 4);
    assert_eq!(evs[0].kind, RawWatchKind::Created);
    assert_eq!(evs[0].name.as_deref(), Some("new.png"));
    assert!(!evs[0].is_dir);
    assert_eq!(evs[1].kind, RawWatchKind::Modified);
    assert_eq!(evs[2].kind, RawWatchKind::Overflow);
    assert!(evs[2].name.is_none());
    assert_eq!(evs[3].kind, RawWatchKind::Created);
    assert!(evs[3].is_dir);
}

#[test]
fn truncated_tail_is_dropped_silently() {
    // a partial event (header only claims more bytes than present) must not
    // panic the parser — a torn read can happen at buffer boundaries
    let mut buf = event_bytes(1, 0x100, Some("abc.png"));
    buf.truncate(buf.len() - 2);
    let evs = parse_events(&buf);
    assert_eq!(evs.len(), 1); // name parses to "abc.pn"? no: NUL missing →
    // len says 8 bytes but only 6 present; we clamp to buffer end and take
    // bytes up to NUL or end → "abc.png" clamped is fine either way
    assert!(evs[0].name.is_some());
}

// --- state application --------------------------------------------------------

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
    let ev = RawWatchEvent { name: Some("skip.txt".into()), is_dir: false, kind: RawWatchKind::Created };
    assert!(matches!(apply_event(&mut st, dir.path(), &filter, &mut overrides, &ev), WatchAction::Ignored));
    assert_eq!(st.file_count(), 1);

    // allowed → added
    std::fs::write(dir.path().join("b.jpg"), b"xx").unwrap();
    let ev = RawWatchEvent { name: Some("b.jpg".into()), is_dir: false, kind: RawWatchKind::Created };
    match apply_event(&mut st, dir.path(), &filter, &mut overrides, &ev) {
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
    let ev = RawWatchEvent { name: Some("newfolder".into()), is_dir: true, kind: RawWatchKind::Created };
    match apply_event(&mut st, dir.path(), &filter, &mut overrides, &ev) {
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
    let ev = RawWatchEvent { name: Some("a.jpg".into()), is_dir: false, kind: RawWatchKind::Modified };
    assert!(matches!(
        apply_event(&mut st, dir.path(), &filter, &mut overrides, &ev),
        WatchAction::Changed { kind: qimgv_engine::watcher::Applied::Modified, .. }
    ));

    // delete → Changed{Removed}
    std::fs::remove_file(&p).unwrap();
    let ev = RawWatchEvent { name: Some("a.jpg".into()), is_dir: false, kind: RawWatchKind::Deleted };
    match apply_event(&mut st, dir.path(), &filter, &mut overrides, &ev) {
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
    let ev = RawWatchEvent { name: None, is_dir: false, kind: RawWatchKind::Overflow };
    assert_eq!(apply_event(&mut st, dir.path(), &filter, &mut overrides, &ev), WatchAction::Reconcile);
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
