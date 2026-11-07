//! Watcher tests: mask classification, watch attribution, state application,
//! and live watching through a real engine. The overflow path is tested
//! synthetically (the kernel only emits IN_Q_OVERFLOW under memory pressure we
//! can't trigger deterministically without root).

use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use qimgv_engine::watcher::{
    apply_event, classify, event_is_live, is_dir_mask, Applied, IgnoreReason, RawWatchEvent,
    RawWatchKind, WatchAction,
};
use qimgv_engine::{Command, Engine, EngineConfig, Event, NameFilter, SortMode, WatchChangeKind};

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
    assert!(event_is_live(7, &RawWatchKind::Deleted, Some(7)));
    assert!(!event_is_live(7, &RawWatchKind::Deleted, Some(9)));
    assert!(!event_is_live(7, &RawWatchKind::Created, None));
    assert!(!event_is_live(7, &RawWatchKind::WatchGone, Some(9)));
    // IN_Q_OVERFLOW carries wd == -1: it names no watch, it describes the
    // queue. Dropping it would stop the engine reconciling after a kernel-side
    // loss forever, which is worse than a stale event.
    assert!(event_is_live(-1, &RawWatchKind::Overflow, Some(9)));
    assert!(event_is_live(-1, &RawWatchKind::Overflow, None));
    // a paired move is decided by the MOVED_TO half it rode in on, so the same
    // rule has to hold for it — this is the kind the fd layer emits.
    assert!(event_is_live(7, &RawWatchKind::Renamed { from: "a.jpg".into() }, Some(7)));
    assert!(!event_is_live(7, &RawWatchKind::Renamed { from: "a.jpg".into() }, Some(9)));
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
    assert_eq!(apply_event(&mut st, &filter, &mut overrides, false, &ev),
             WatchAction::Ignored(IgnoreReason::ExtensionRejected));
    assert_eq!(st.file_count(), 1);

    // allowed → added
    std::fs::write(dir.path().join("b.jpg"), b"xx").unwrap();
    let ev = raw_event(dir.path(), Some("b.jpg"), false, RawWatchKind::Created);
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
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
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
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
        apply_event(&mut st, &filter, &mut overrides, false, &ev),
        WatchAction::Changed { kind: qimgv_engine::watcher::Applied::Modified, .. }
    ));

    // delete → Changed{Removed}
    std::fs::remove_file(&p).unwrap();
    let ev = raw_event(dir.path(), Some("a.jpg"), false, RawWatchKind::Deleted);
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
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
        apply_event(&mut st, &filter, &mut overrides, false, &ev),
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
    assert_eq!(apply_event(&mut st, &filter, &mut overrides, false, &ev),
             WatchAction::Ignored(IgnoreReason::WrongSubject));
    assert_eq!(st.file_count(), 1, "the row for /B/a.jpg survived a delete meant for /A");

    // and a create named elsewhere lands where it was created, not here
    std::fs::write(other.path().join("c.jpg"), b"x").unwrap();
    let ev = raw_event(other.path(), Some("c.jpg"), false, RawWatchKind::Created);
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { path, .. } => panic!("a foreign create must not add a row: {path}"),
        WatchAction::Ignored(IgnoreReason::WrongSubject) => {}
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 1);
}

#[test]
fn the_hidden_rule_is_the_scanner_s_rule() {
    // Both halves, because the scanner skips hidden entries BEFORE it reaches the
    // file/dir branch: a dot-named folder was not a member either. Before this,
    // the watcher admitted what the scanner had refused, and the next rescan took
    // the row back out — a listing that changed with nobody having changed it.
    let (mut st, dir, _p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    std::fs::write(dir.path().join(".secret.jpg"), b"x").unwrap();
    let ev = raw_event(dir.path(), Some(".secret.jpg"), false, RawWatchKind::Created);
    assert_eq!(apply_event(&mut st, &filter, &mut overrides, false, &ev),
             WatchAction::Ignored(IgnoreReason::Hidden));
    assert_eq!(st.file_count(), 1, "a hidden file must not join the listing");

    std::fs::create_dir(dir.path().join(".git")).unwrap();
    let ev = raw_event(dir.path(), Some(".git"), true, RawWatchKind::Created);
    assert_eq!(apply_event(&mut st, &filter, &mut overrides, false, &ev),
             WatchAction::Ignored(IgnoreReason::Hidden));
    assert_eq!(st.dir_count(), 0, "nor may a hidden directory");

    // and the flag really is consulted rather than the check being hardcoded
    let ev = raw_event(dir.path(), Some(".secret.jpg"), false, RawWatchKind::Created);
    assert!(matches!(
        apply_event(&mut st, &filter, &mut overrides, true, &ev),
        WatchAction::Changed { .. }
    ));
    assert_eq!(st.file_count(), 2, "show_hidden admits it, as the scanner would");
}

// --- a rename, as one event ---------------------------------------------------

/// A listing of `names` (one byte each) over `dir`, in `sort`. The mtimes are
/// the epoch so a restat always sees the real file as newer — which is what
/// makes a Changed answer the expected one rather than a coin flip.
fn state_of(dir: &std::path::Path, names: &[&str], dirs: &[&str], sort: SortMode) -> qimgv_engine::DirectoryState {
    let file_entries = names.iter().map(|n| qimgv_engine::EntryData {
        name: (*n).into(),
        path: dir.join(n).to_string_lossy().into_owned(),
        is_dir: false,
        size: 1,
        mtime: std::time::UNIX_EPOCH,
    });
    let dir_entries = dirs.iter().map(|n| qimgv_engine::EntryData {
        name: (*n).into(),
        path: dir.join(n).to_string_lossy().into_owned(),
        is_dir: true,
        size: 0,
        mtime: std::time::UNIX_EPOCH,
    });
    let mut st = qimgv_engine::DirectoryState::new(sort);
    st.replace_all(dir.to_path_buf(), file_entries.collect(), dir_entries.collect());
    st
}

fn s(p: &std::path::Path) -> String {
    p.to_string_lossy().into_owned()
}

#[test]
fn a_paired_rename_is_one_event_about_one_entry() {
    // The whole point of pairing the cookie: `mv a.jpg z.jpg` inside a watched
    // folder used to reach the consumer as Removed(a) + Added(z) — two rows
    // coming and going, so everything keyed on the entry (the selected row, the
    // queued thumbnail, the picture on screen) had to be rebuilt from the new
    // path and the old one forgotten. One event says what actually happened:
    // one entry, now at this path.
    let (mut st, dir, p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();
    std::fs::rename(&p, dir.path().join("z.jpg")).unwrap();

    let ev = raw_event(dir.path(), Some("z.jpg"), false, RawWatchKind::Renamed { from: "a.jpg".into() });
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { kind: Applied::Renamed { from }, path, name, size, mtime_ms, index, is_dir } => {
            assert_eq!(from, s(&p), "the event names the path the entry had");
            assert_eq!(path, s(&dir.path().join("z.jpg")));
            assert_eq!(name, "z.jpg");
            assert_eq!(size, 1, "the destination's own stat, not the listing's memory");
            assert_ne!(mtime_ms, 0, "a file's rename carries its real mtime, so the thumbnail cache can invalidate");
            assert!(!is_dir);
            assert_eq!(index, 0, "one row, where the engine's order puts it");
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 1, "a rename must not leave the old row behind");
    assert!(st.contains_path(&s(&dir.path().join("z.jpg"))));
    assert!(!st.contains_path(&s(&p)));
}

#[test]
fn a_rename_moves_the_row_the_name_sort_says_to() {
    // A metadata update can keep its position under a name sort because the name
    // did not change; a rename changes the name, so the row has to be re-seated
    // in every mode and the index handed out has to be the new one.
    let dir = tempfile::tempdir().unwrap();
    for n in ["a.jpg", "b.jpg", "c.jpg"] {
        std::fs::write(dir.path().join(n), b"x").unwrap();
    }
    let mut st = state_of(dir.path(), &["a.jpg", "b.jpg", "c.jpg"], &[], SortMode::Name);
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    std::fs::rename(dir.path().join("c.jpg"), dir.path().join("0.jpg")).unwrap();
    let ev = raw_event(dir.path(), Some("0.jpg"), false, RawWatchKind::Renamed { from: "c.jpg".into() });
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { index, .. } => assert_eq!(index, 0, "c.jpg's row is now first"),
        other => panic!("unexpected: {other:?}"),
    }
    let got: Vec<String> = (0..st.file_count())
        .map(|i| st.file_at(i).unwrap().data.name.clone())
        .collect();
    assert_eq!(got, vec!["0.jpg", "a.jpg", "b.jpg"], "the order follows the names it holds");
}

#[test]
fn a_rename_onto_a_listed_name_overwrites_instead_of_leaving_two_rows() {
    // `mv a.jpg b.jpg` where both were listed: b.jpg's contents are gone, a.jpg's
    // inode is now at that path. The listing must end with one row at b.jpg and
    // a's metadata — and the event has to describe that result, because a
    // consumer that saw Added(b) after Removed(b) would have two rows to place
    // and only one file.
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"aaaa").unwrap();
    std::fs::write(dir.path().join("b.jpg"), b"b").unwrap();
    let mut st = state_of(dir.path(), &["a.jpg", "b.jpg"], &[], SortMode::Name);
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    std::fs::rename(dir.path().join("a.jpg"), dir.path().join("b.jpg")).unwrap();
    let ev = raw_event(dir.path(), Some("b.jpg"), false, RawWatchKind::Renamed { from: "a.jpg".into() });
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { kind: Applied::Renamed { from }, path, size, index, .. } => {
            assert_eq!(from, s(&dir.path().join("a.jpg")));
            assert_eq!(path, s(&dir.path().join("b.jpg")));
            assert_eq!(size, 4, "the row describes the file that is there now");
            assert_eq!(index, 0);
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 1);
    assert_eq!(st.file_at(0).unwrap().data.size, 4);
}

#[test]
fn an_unpaired_move_onto_a_listed_name_is_not_a_no_op() {
    // The same overwrite arriving from outside the watch (mv /elsewhere/a.jpg
    // here) has no MOVED_TO partner to pair with, so it is a bare MOVED_TO — and
    // answering it `AlreadyListed` used to leave a row whose size and mtime
    // described a file that no longer exists, with the thumbnail cache — which
    // validates by mtime, and now sees the mtime the moved-in file carries —
    // still serving the old picture.
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("b.jpg"), b"bbbb").unwrap();
    let mut st = state_of(dir.path(), &["b.jpg"], &[], SortMode::Name);
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();

    let ev = raw_event(dir.path(), Some("b.jpg"), false, RawWatchKind::MovedTo);
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { kind: Applied::Modified, path, size, .. } => {
            assert_eq!(path, s(&dir.path().join("b.jpg")));
            assert_eq!(size, 4);
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 1, "an overwrite replaces a row, it does not add one");
}

#[test]
fn a_rename_to_a_name_that_is_not_a_member_is_the_removal_it_was() {
    // The degradation has to be exact: a.jpg -> notes.txt is not listable, so
    // the only fact the listing has is that the row is gone — which is what an
    // unpaired MOVED_FROM reported before pairing existed.
    let (mut st, dir, p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();
    std::fs::rename(&p, dir.path().join("notes.txt")).unwrap();

    let ev = raw_event(dir.path(), Some("notes.txt"), false, RawWatchKind::Renamed { from: "a.jpg".into() });
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { kind: Applied::Removed, path, .. } => assert_eq!(path, s(&p)),
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 0);

    // and a hidden destination is not a member either
    let (mut st, dir, p) = state_with_file();
    std::fs::rename(&p, dir.path().join(".a.jpg")).unwrap();
    let ev = raw_event(dir.path(), Some(".a.jpg"), false, RawWatchKind::Renamed { from: "a.jpg".into() });
    assert!(matches!(
        apply_event(&mut st, &filter, &mut overrides, false, &ev),
        WatchAction::Changed { kind: Applied::Removed, .. }
    ));
    assert_eq!(st.file_count(), 0);
}

#[test]
fn a_rename_in_from_outside_the_watch_is_an_arrival() {
    // The source was never ours to rename, so there is no row to move: the
    // destination is an entry that appeared, exactly as an unpaired MOVED_TO said.
    let (mut st, dir, _p) = state_with_file();
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();
    std::fs::write(dir.path().join("in.jpg"), b"x").unwrap();

    let ev = raw_event(dir.path(), Some("in.jpg"), false,
                       RawWatchKind::Renamed { from: "/somewhere/else/in.jpg".into() });
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { kind: Applied::Added, path, .. } => assert_eq!(path, s(&dir.path().join("in.jpg"))),
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.file_count(), 2);
}

#[test]
fn a_renamed_directory_stays_canonical() {
    // Directories are canonical members (size 0, mtime epoch) whatever the disk
    // says, and a rename must not be the event that hands one real metadata —
    // that would let a size or time sort reorder it against the scanner's rule.
    let dir = tempfile::tempdir().unwrap();
    std::fs::create_dir(dir.path().join("sub")).unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    let mut st = state_of(dir.path(), &["a.jpg"], &["sub"], SortMode::Name);
    let filter = NameFilter::from_extensions(["jpg"]);
    let mut overrides: std::collections::HashSet<std::path::PathBuf> = Default::default();
    std::fs::rename(dir.path().join("sub"), dir.path().join("sub2")).unwrap();

    let ev = raw_event(dir.path(), Some("sub2"), true, RawWatchKind::Renamed { from: "sub".into() });
    match apply_event(&mut st, &filter, &mut overrides, false, &ev) {
        WatchAction::Changed { kind: Applied::Renamed { from }, path, is_dir, size, mtime_ms, .. } => {
            assert!(is_dir, "the event must still say it is a folder");
            assert_eq!(from, s(&dir.path().join("sub")));
            assert_eq!(path, s(&dir.path().join("sub2")));
            assert_eq!(size, 0);
            assert_eq!(mtime_ms, 0);
        }
        other => panic!("unexpected: {other:?}"),
    }
    assert_eq!(st.dir_count(), 1);
    assert_eq!(st.file_count(), 1, "the file is untouched");
}

// --- live watching through a real engine ---------------------------------------

#[cfg_attr(miri, allow(dead_code))]
fn config() -> EngineConfig {
    EngineConfig {
        scan_threads: 2,
        show_hidden: Arc::new(AtomicBool::new(false)),
        filter: NameFilter::from_extensions(["jpg"]),
        initial_sort: SortMode::Name,
    }
}

#[cfg_attr(miri, allow(dead_code))]
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

/// Drains until an event carrying a session id shows up, and returns that id.
#[cfg_attr(miri, allow(dead_code))]
fn next_session_event(engine: &qimgv_engine::Engine) -> u64 {
    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    loop {
        match engine.try_event() {
            Some(Event::DirListed { epoch, .. }) | Some(Event::OpenFailed { epoch, .. }) => return epoch,
            Some(_) => {}
            None => assert!(std::time::Instant::now() < deadline, "no listing event arrived"),
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
#[cfg_attr(miri, ignore = "drives the real fd machinery: no interpreter")]
fn an_engine_initiated_rescan_answers_the_session_it_belongs_to() {
    // The stale-scan guard asks "is this job the newest session", so the only
    // thing allowed to start a session is a request. Deleting the watched folder
    // makes the engine re-scan it on its own initiative — and that rescan used to
    // ALLOCATE a session id. The instant it did, an open the caller had just
    // allocated looked older than the engine's own bookkeeping, so the loop threw
    // its result away and emitted nothing about the decision: the caller waits
    // forever for a listing nobody cancelled out loud.
    let (tx, _rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = tx.send(());
    }));

    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    let opened = engine.next_epoch();
    engine.send(Command::OpenDir {
        path: dir.path().to_owned(),
        sort: None,
        epoch: opened,
    })
    .unwrap();
    assert_eq!(next_session_event(&engine), opened);

    // the engine's own reason to re-scan: the watched folder is gone
    std::fs::remove_dir_all(dir.path()).unwrap();
    assert_eq!(
        next_session_event(&engine),
        opened,
        "an internal rescan started a new session and can cancel an open in flight"
    );
}

#[test]
#[cfg_attr(miri, ignore = "drives the real fd machinery: no interpreter")]
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
#[cfg_attr(miri, ignore = "drives the real fd machinery: no interpreter")]
fn the_watcher_pairs_a_live_rename_into_one_event() {
    // The unit tests say what a paired event means; this says the kernel really
    // gives the watcher thread something pairable. A rename inside a watched
    // folder is one MOVED_FROM and one MOVED_TO sharing a cookie, and the cookie
    // exists nowhere else in the pipeline — so if the pairing is wrong, the
    // consumer gets the old Removed+Added pair (silent, and the reason this was
    // worth changing) or nothing at all (the row vanishes). Both are asserted
    // against, and so is the count: one event, not two.
    let (tx, _rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = tx.send(());
    }));

    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("a.jpg"), b"x").unwrap();
    engine
        .send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() })
        .unwrap();
    wait_for(&engine, |st| st.file_count() == 1, 10);
    // the listing event this open produced is not what this test is about
    while engine.try_event().is_some() {}

    let old = dir.path().join("a.jpg");
    let new = dir.path().join("z.jpg");
    let (want_old, want_new) = (old.to_string_lossy().into_owned(), new.to_string_lossy().into_owned());
    std::fs::rename(&old, &new).unwrap();

    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    let mut renamed: Vec<(String, String)> = Vec::new();
    let mut other_mutations = 0;
    loop {
        if let Some(Event::Mutated { kind, path, .. }) = engine.try_event() {
            match kind {
                WatchChangeKind::Renamed { from } => renamed.push((from, path)),
                _ => other_mutations += 1,
            }
        }
        if !renamed.is_empty() || std::time::Instant::now() >= deadline {
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    // give a second half a chance to show up before judging the count
    std::thread::sleep(Duration::from_millis(200));
    while let Some(ev) = engine.try_event() {
        if matches!(ev, Event::Mutated { .. }) {
            other_mutations += 1;
        }
    }
    assert_eq!(renamed.len(), 1, "no rename event arrived (other mutations: {other_mutations})");
    assert_eq!(renamed[0].0, want_old, "the event names the path the entry had");
    assert_eq!(renamed[0].1, want_new);
    assert_eq!(
        other_mutations, 0,
        "a paired rename must not also arrive as the remove+add pair it replaced"
    );

    wait_for(&engine, |st| st.file_count() == 1 && st.contains_path(&want_new), 10);
}

#[test]
#[cfg_attr(miri, ignore = "drives the real fd machinery: no interpreter")]
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
#[cfg_attr(miri, ignore = "drives the real fd machinery: no interpreter")]
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

#[test]
#[cfg_attr(miri, ignore = "drives the real fd machinery: no interpreter")]
fn operations_on_a_settled_folder_all_land_in_the_listing() {
    // The C++ suite's remaining divergence happens with the folder already
    // settled: everything below occurs after the initial scan landed, so this
    // reproduces the shape without racing a watch installation. If an operation
    // is lost, it was lost by something named here — a filter, a membership test,
    // or the kernel not telling us at all — and the message says which end state
    // was reached.
    let (tx, _rx) = mpsc::channel::<()>();
    let engine = Engine::spawn(config(), Arc::new(move || {
        let _ = tx.send(());
    }));
    let dir = tempfile::tempdir().unwrap();
    for name in ["img1.jpg", "img2.jpg", "img10.jpg"] {
        std::fs::write(dir.path().join(name), b"x").unwrap();
    }
    std::fs::create_dir(dir.path().join("subdir")).unwrap();
    engine
        .send(Command::OpenDir { path: dir.path().to_owned(), sort: None, epoch: engine.next_epoch() })
        .unwrap();
    // settled first: three jpgs listed, skip-nothing, and the folder is quiet
    wait_for(&engine, |st| st.file_count() == 3, 10);
    std::thread::sleep(Duration::from_millis(50));

    std::fs::write(dir.path().join("IMG2.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("img 10.jpg"), b"x").unwrap();
    std::fs::write(dir.path().join("10.jpg"), b"x").unwrap();
    std::fs::remove_file(dir.path().join("img1.jpg")).unwrap();
    let f = dir.path().join("img2.jpg");
    let mut h = std::fs::OpenOptions::new().append(true).open(&f).unwrap();
    std::io::Write::write_all(&mut h, b"appended").unwrap();
    drop(h);

    let want: Vec<String> = {
        let mut v: Vec<String> = std::fs::read_dir(dir.path())
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.ends_with(".jpg"))
            .collect();
        v.sort();
        v
    };
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    let got = loop {
        // the read guard dies with this block, BEFORE the sleep: a guard held
        // across the wait starves the engine loop's writer for the whole
        // deadline (std's RwLock has no writer priority), and the state this
        // loop is waiting for can never land
        let v: Vec<String> = {
            let st = engine.state.read().unwrap();
            (0..st.file_count())
                .filter_map(|i| st.file_at(i).map(|r| r.data.name.clone()))
                .collect()
        };
        let mut sorted = v;
        sorted.sort();
        if sorted == want || std::time::Instant::now() >= deadline {
            break sorted;
        }
        std::thread::sleep(Duration::from_millis(5));
    };
    assert_eq!(
        got,
        want,
        "engine listing holds {:?} while the folder holds {:?}",
        got,
        want
    );
}
