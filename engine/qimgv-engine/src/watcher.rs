//! In-directory inotify watching (Linux). The watcher thread owns the
//! inotify fd and implements the drain discipline agreed in the consistency
//! discussions: the kernel queue is a conveyor belt, not a warehouse — a
//! dedicated thread reads with a large buffer and hands parsed events to the
//! engine loop through an ordered channel. IN_Q_OVERFLOW is surfaced so the
//! engine can reconcile with a full rescan.

use std::fs;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::{SystemTime, UNIX_EPOCH};

use crossbeam_channel::{Receiver, Sender};
use inotify::{Inotify, WatchMask};

use crate::filter::NameFilter;
use crate::model::DirectoryState;

// --- raw parsing ------------------------------------------------------------

/// One parsed inotify event, before it touches the engine state.
///
/// `dir` is the directory the *watch that produced this event* was added for,
/// resolved by the watcher thread — not by the consumer. An event that carries
/// only a name forces the receiver to supply the directory from its own state,
/// and a stale event then gets a live directory bolted onto it: deleting
/// `/A/img1.jpg` would remove `/B/img1.jpg`, which is a file that still exists.
/// With the subject inside the message, that deletion simply does not resolve.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawWatchEvent {
    /// The kernel's watch descriptor. Only `Overflow` events have no watch
    /// (`wd == -1`); everything else was produced by exactly one directory.
    pub wd: i32,
    pub dir: PathBuf,
    pub name: Option<String>,
    pub is_dir: bool,
    pub kind: RawWatchKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RawWatchKind {
    Created,
    Modified,
    Deleted,
    MovedFrom,
    MovedTo,
    /// Kernel dropped events (queue full) — full rescan required.
    Overflow,
    /// The watched directory itself went away.
    WatchGone,
}

// Raw inotify masks (inotify(7)). The kernel's record header is parsed by the
// inotify crate; these constants classify its mask bits, kept as plain u32 so
// the mapping can be tested without the crate's bitflags in the public API.
mod mask {
    pub const MODIFY: u32 = 0x0000_0002;
    pub const MOVED_FROM: u32 = 0x0000_0040;
    pub const MOVED_TO: u32 = 0x0000_0080;
    pub const CREATE: u32 = 0x0000_0100;
    pub const DELETE: u32 = 0x0000_0200;
    pub const DELETE_SELF: u32 = 0x0000_0400;
    pub const MOVE_SELF: u32 = 0x0000_0800;
    pub const Q_OVERFLOW: u32 = 0x0000_4000;
    pub const IGNORED: u32 = 0x0000_8000;
    pub const ISDIR: u32 = 0x4000_0000;
}

/// Maps one event's mask bits to what the engine cares about. `None` means the
/// record is bookkeeping the listing does not react to (IN_IGNORED after a
/// removal, attrib-only changes, and so on).
pub fn classify(mask: u32) -> Option<RawWatchKind> {
    let kind = if mask & mask::Q_OVERFLOW != 0 {
        RawWatchKind::Overflow
    } else if mask & (mask::DELETE_SELF | mask::MOVE_SELF) != 0 {
        RawWatchKind::WatchGone
    } else if mask & mask::IGNORED != 0 {
        return None; // bookkeeping after rm_watch / auto-removal
    } else if mask & mask::CREATE != 0 {
        RawWatchKind::Created
    } else if mask & mask::MODIFY != 0 {
        RawWatchKind::Modified
    } else if mask & mask::DELETE != 0 {
        RawWatchKind::Deleted
    } else if mask & mask::MOVED_FROM != 0 {
        RawWatchKind::MovedFrom
    } else if mask & mask::MOVED_TO != 0 {
        RawWatchKind::MovedTo
    } else {
        return None;
    };
    Some(kind)
}

pub fn is_dir_mask(mask: u32) -> bool {
    mask & mask::ISDIR != 0
}

/// True when an event belongs to the currently installed watch. Overflow is
/// the exception: it names no watch (the kernel sends `wd == -1`) because it
/// describes the queue as a whole, and the queue's only subject is the live
/// directory — dropping it would silently stop the engine from ever
/// reconciling after a kernel-side loss.
pub fn event_is_live(wd: i32, kind: RawWatchKind, live_wd: Option<i32>) -> bool {
    if kind == RawWatchKind::Overflow {
        return true;
    }
    live_wd == Some(wd)
}

// --- engine-side application --------------------------------------------------

/// What the engine loop should do with a parsed event.
#[derive(Debug, Clone, PartialEq)]
pub enum Applied {
    Added,
    Modified,
    Removed,
}

/// Why an event touched nothing. A bare "ignored" is the one answer that cannot
/// be investigated: when a file is missing from the listing, "the watcher never
/// reported it", "the filter refused its name" and "it was already in the
/// listing" all look identical from outside, and only the first means a bug. So
/// every `Ignored` has to say which of them it is, and a trace line can name it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IgnoreReason {
    /// The event is about a folder other than the one this state describes.
    WrongSubject,
    /// A create or move with no name attached — inotify always supplies one, so
    /// reaching this means the parse produced something it should not have.
    NoName,
    /// Dotfile, and the scanner's hidden rule says it is not listable.
    Hidden,
    /// Not a directory and its name matches neither the whitelist nor an override.
    ExtensionRejected,
    /// The path was gone by the time it was stat'd.
    StatFailed,
    /// A create for a path the listing already holds.
    AlreadyListed,
    /// A delete or modify for a path this listing never contained.
    NotListed,
    /// A modify on a directory: its own mtime has no listing meaning.
    DirIsCanonical,
    /// A modify whose on-disk mtime equals the one already stored.
    MtimeUnchanged,
    /// The application step panicked; the engine loop's guard reports this and
    /// prints the payload.
    Panicked,
}

#[derive(Debug, Clone, PartialEq)]
pub enum WatchAction {
    Changed {
        kind: Applied,
        path: String,
        name: String,
        is_dir: bool,
        size: u64,
        mtime_ms: i64,
        /// The entry's position in the engine's own order at the time of the
        /// event. The engine decides where things sit; consumers splice.
        index: u32,
    },
    /// The watcher snapshot can't be trusted for this directory.
    Reconcile { dir: PathBuf },
    /// The watched directory is gone.
    DirGone { dir: PathBuf },
    Ignored(IgnoreReason),
}

/// Applies one parsed event to the state. The directory comes from the event:
/// a caller that supplied its own current directory would apply a stale event
/// from one folder against another folder's listing.
pub fn apply_event(
    state: &mut DirectoryState,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    show_hidden: bool,
    ev: &RawWatchEvent,
) -> WatchAction {
    use RawWatchKind as K;
    let dir = ev.dir.as_path();
    // The event's subject has to be the directory this state describes. A delete
    // for elsewhere already resolves to nothing here, but a *create* elsewhere
    // would otherwise insert a foreign path as a row of this listing — the two
    // halves of the same mistake, and both closed by one check at the point of
    // mutation rather than by callers remembering to filter.
    if state.dir() != Some(dir) {
        return WatchAction::Ignored(IgnoreReason::WrongSubject);
    }
    match ev.kind {
        K::Overflow => WatchAction::Reconcile { dir: dir.to_path_buf() },
        K::WatchGone => WatchAction::DirGone { dir: dir.to_path_buf() },
        K::Created | K::MovedTo => {
            let Some(name) = &ev.name else { return WatchAction::Ignored(IgnoreReason::NoName) };
            let path = dir.join(name);
            // The scanner's membership rules apply here too, in the same order:
            // a hidden entry is not admitted whatever else would have allowed it.
            if crate::scan::is_hidden(&path, name) && !show_hidden {
                return WatchAction::Ignored(IgnoreReason::Hidden);
            }
            // Directories bypass the extension whitelist. scan.rs puts every
            // directory in the listing without consulting it, and the watcher used
            // to run dir names through it too — so an external mkdir matched no
            // image extension and the folder never appeared until a full rescan.
            if !ev.is_dir && !filter.matches(name) && !overrides.contains(&path) {
                return WatchAction::Ignored(IgnoreReason::ExtensionRejected);
            }
            let meta = match fs::metadata(&path) {
                Ok(m) => m,
                // vanished already
                Err(_) => return WatchAction::Ignored(IgnoreReason::StatFailed),
            };
            let entry = crate::model::EntryData {
                name: name.clone(),
                path: path.to_string_lossy().into_owned(),
                is_dir: ev.is_dir,
                size: if ev.is_dir { 0 } else { meta.len() },
                // Directories are canonical (size 0, mtime epoch, name-ordered
                // in every mode): scan.rs creates them that way, so a watched
                // creation must not hand them real metadata that would let a
                // size/time sort reorder them against the scanner's rule.
                mtime: if ev.is_dir { UNIX_EPOCH } else { meta.modified().unwrap_or(UNIX_EPOCH) },
            };
            if let Some((_key, index)) = state.insert_entry(entry) {
                WatchAction::Changed {
                    kind: Applied::Added,
                    path: path.to_string_lossy().into_owned(),
                    name: name.clone(),
                    is_dir: ev.is_dir,
                    size: if ev.is_dir { 0 } else { meta.len() },
                    mtime_ms: if ev.is_dir { 0 } else { mtime_ms(meta.modified().unwrap_or(UNIX_EPOCH)) },
                    index,
                }
            } else {
                WatchAction::Ignored(IgnoreReason::AlreadyListed)
            }
        }
        K::Deleted | K::MovedFrom => {
            let path = dir.join(ev.name.as_deref().unwrap_or(""));
            let path = path.to_string_lossy().into_owned();
            if let Some((_key, index)) = state.remove_entry_by_path(&path) {
                WatchAction::Changed {
                    kind: Applied::Removed,
                    path,
                    name: String::new(),
                    is_dir: ev.is_dir,
                    size: 0,
                    mtime_ms: 0,
                    index,
                }
            } else {
                WatchAction::Ignored(IgnoreReason::NotListed)
            }
        }
        K::Modified => {
            // A directory's own mtime carries no listing meaning (see the
            // canonical-dir note above), and no consumer mirrors it: the
            // engine state stays exactly as the scanner left it.
            if ev.is_dir {
                return WatchAction::Ignored(IgnoreReason::DirIsCanonical);
            }
            let path = dir.join(ev.name.as_deref().unwrap_or(""));
            let path = path.to_string_lossy().into_owned();
            let Some((_, old_mtime)) = state.entry_meta(&path) else {
                return WatchAction::Ignored(IgnoreReason::NotListed);
            };
            let meta = match fs::metadata(&path) {
                Ok(m) => m,
                Err(_) => return WatchAction::Ignored(IgnoreReason::StatFailed),
            };
            let mtime = meta.modified().unwrap_or(UNIX_EPOCH);
            if mtime == old_mtime {
                return WatchAction::Ignored(IgnoreReason::MtimeUnchanged);
            }
            let size = meta.len();
            let Some(index) = state.update_entry_meta(&path, size, mtime) else {
                return WatchAction::Ignored(IgnoreReason::NotListed);
            };
            WatchAction::Changed {
                kind: Applied::Modified,
                path,
                name: String::new(),
                is_dir: ev.is_dir,
                size,
                mtime_ms: mtime_ms(mtime),
                index,
            }
        }
    }
}

pub fn mtime_ms(t: SystemTime) -> i64 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

// --- the watcher thread -------------------------------------------------------

/// Everything about a folder that has to happen in one order happens on one
/// thread: install the subscription, then enumerate the baseline. Two threads
/// cannot promise that ordering to each other, which is why a change used to be
/// able to fall into a gap where it was in neither the snapshot nor any event —
/// not unnoticed, but undetectable. `session`/`dispatch` ride along so the
/// result can be attributed to the request it answers.
pub(crate) enum WatchCmd {
    Watch { path: PathBuf, session: u64, dispatch: u64 },
    /// Re-enumerate a folder that is already watched (a declared loss: the
    /// kernel queue overflowed, the folder vanished, or a caller asked).
    Enumerate { path: PathBuf, session: u64, dispatch: u64 },
    /// Drop the current watch. Explicit, because the previous implementation of
    /// "stop watching" was to add a watch on an empty path and rely on it failing.
    Unwatch,
    Stop,
}

/// What the engine loop should do with a parsed event... (events are tagged
/// with the generation they belong to, so in-flight events for a previous
/// target are dropped after a retarget).
pub(crate) struct WatcherHandle {
    cmd_tx: Sender<WatchCmd>,
    wake: OwnedFd,
    join: Option<JoinHandle<()>>,
    running: Arc<AtomicBool>,
}

impl WatcherHandle {
    pub(crate) fn spawn(
        options: crate::scan::ScanOptions,
        res_tx: Sender<crate::engine::ScanDone>,
    ) -> (WatcherHandle, Receiver<RawWatchEvent>) {
        let (cmd_tx, cmd_rx) = crossbeam_channel::unbounded::<WatchCmd>();
        let (ev_tx, ev_rx) = crossbeam_channel::unbounded::<RawWatchEvent>();
        let running = Arc::new(AtomicBool::new(true));
        let running_flag = Arc::clone(&running);

        // wake fd lets command sends interrupt the poll from any thread
        let wake = new_eventfd();
        // the loop gets its own descriptor onto the same eventfd: each end closes
        // exactly once by ownership, so no fd number is ever released twice
        let loop_wake = wake.try_clone().expect("dup the watcher wake fd");

        let join = std::thread::Builder::new()
            .name("qimgv-watch".into())
            .spawn(move || watcher_loop(&cmd_rx, &ev_tx, &res_tx, &options, loop_wake, running_flag))
            .expect("spawn watcher thread");

        (
            WatcherHandle {
                cmd_tx,
                wake,
                join: Some(join),
                running,
            },
            ev_rx,
        )
    }

    pub(crate) fn send(&self, cmd: WatchCmd) {
        let _ = self.cmd_tx.send(cmd);
        let one: u64 = 1;
        unsafe {
            libc::write(self.wake.as_raw_fd(), &one as *const u64 as *const libc::c_void, 8);
        }
    }

    /// Safe to call twice, because the engine loop shuts the watcher down and
    /// then drops it: this only re-sends a command and re-joins an already-joined
    /// thread. Nothing releases a descriptor by hand anymore — `wake` is an
    /// OwnedFd and closes once when the handle drops.
    pub(crate) fn shutdown(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        let _ = self.cmd_tx.send(WatchCmd::Stop);
        let one: u64 = 1;
        unsafe {
            libc::write(self.wake.as_raw_fd(), &one as *const u64 as *const libc::c_void, 8);
        }
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

fn new_eventfd() -> OwnedFd {
    // CLOEXEC so the wake pipe cannot leak into a process the app spawns
    let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    assert!(fd >= 0, "eventfd for the watcher wake pipe failed");
    unsafe { OwnedFd::from_raw_fd(fd) }
}

impl Drop for WatcherHandle {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Take the baseline for a folder. Called immediately after the watch is
/// installed, by the same thread, so "changes from now on are visible" and
/// "this is what was there" cannot be reordered against each other. The result
/// goes back to the engine loop, which is the only writer of the listing state.
fn enumerate_and_report(
    path: &Path,
    session: u64,
    dispatch: u64,
    res_tx: &Sender<crate::engine::ScanDone>,
    options: &crate::scan::ScanOptions,
) {
    let start = std::time::Instant::now();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        crate::scan::scan_directory(path, options)
    }))
    .unwrap_or_else(|_| Err(crate::error::EngineError::Io {
        path: path.to_path_buf(),
        source: std::io::Error::other("scan worker panicked"),
    }));
    let _ = res_tx.send(crate::engine::ScanDone {
        session,
        dispatch,
        path: path.to_path_buf(),
        result,
        scan_ms: start.elapsed().as_millis(),
    });
}

fn watcher_loop(
    cmd_rx: &Receiver<WatchCmd>,
    ev_tx: &Sender<RawWatchEvent>,
    res_tx: &Sender<crate::engine::ScanDone>,
    options: &crate::scan::ScanOptions,
    wake: OwnedFd,
    running: Arc<AtomicBool>,
) {
    let mut inotify = match Inotify::init() {
        Ok(i) => i,
        Err(e) => {
            log::warn!("watcher: init failed: {e}");
            return;
        }
    };
    let mut watch: Option<inotify::WatchDescriptor> = None;
    // the directory `watch` was added for; every event this thread emits names it
    let mut watch_dir: PathBuf = PathBuf::new();
    let mut buffer = vec![0u8; 64 * 1024];

    while running.load(Ordering::Relaxed) {
        let mut fds = [
            libc::pollfd { fd: inotify.as_raw_fd(), events: libc::POLLIN, revents: 0 },
            libc::pollfd { fd: wake.as_raw_fd(), events: libc::POLLIN, revents: 0 },
        ];
        let ready = unsafe { libc::poll(fds.as_mut_ptr(), 2, 500) };
        if ready < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() != Some(libc::EINTR) {
                log::warn!("watcher: poll failed: {err}");
                return;
            }
            continue;
        }
        if ready == 0 {
            continue;
        }

        // The fd is drained before any command is applied. Both orderings are
        // safe once events carry their own directory, and this one keeps the
        // generation tag honest: a backlog from the outgoing watch is still
        // tagged with the outgoing generation instead of the incoming one.
        if fds[0].revents & libc::POLLIN != 0 {
            match inotify.read_events(&mut buffer) {
                Ok(events) => {
                    let live_wd = watch.as_ref().map(|w| w.get_watch_descriptor_id());
                    for ev in events {
                        let Some(kind) = classify(ev.mask.bits()) else {
                            crate::trace::line("drop", || format!("mask={:#x} bookkeeping", ev.mask.bits()));
                            continue;
                        };
                        let wd = ev.wd.get_watch_descriptor_id();
                        if !event_is_live(wd, kind, live_wd) {
                            // a record from a watch that is no longer installed
                            crate::trace::line("drop", || format!("kind={kind:?} wd={wd} live={live_wd:?}"));
                            continue;
                        }
                        if watch_dir.as_os_str().is_empty() {
                            // overflow reported with no live folder to attribute
                            crate::trace::line("drop", || format!("kind={kind:?} no directory to attribute"));
                            continue;
                        }
                        let name = ev.name.map(|c| c.to_string_lossy().into_owned());
                        if name.is_none() && !matches!(kind, RawWatchKind::Overflow | RawWatchKind::WatchGone) {
                            crate::trace::line("drop", || format!("kind={kind:?} no name"));
                            continue;
                        }
                        let raw = RawWatchEvent {
                            wd,
                            dir: watch_dir.clone(),
                            name,
                            is_dir: is_dir_mask(ev.mask.bits()),
                            kind,
                        };
                        crate::trace::line("read", || format!("{raw:?}"));
                        if ev_tx.send(raw).is_err() {
                            return;
                        }
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(e) => {
                    log::warn!("watcher: read failed: {e}");
                    return;
                }
            }
        }

        if fds[1].revents & libc::POLLIN != 0 {
            // commands: drain the wake counter, then the channel
            let mut buf = [0u8; 8];
            unsafe {
                while libc::read(wake.as_raw_fd(), buf.as_mut_ptr().cast(), 8) as isize == 8 {}
            }
            while let Ok(cmd) = cmd_rx.try_recv() {
                match cmd {
                    WatchCmd::Stop => return,
                    WatchCmd::Enumerate { path, session, dispatch } => {
                        enumerate_and_report(&path, session, dispatch, &res_tx, options);
                    }
                    WatchCmd::Unwatch => {
                        if let Some(w) = watch.take() {
                            let _ = inotify.watches().remove(w);
                        }
                        watch_dir = PathBuf::new();
                    }
                    WatchCmd::Watch { path, session, dispatch } => {
                        if let Some(w) = watch.take() {
                            let _ = inotify.watches().remove(w);
                        }
                        match inotify.watches().add(
                            &path,
                            WatchMask::CREATE
                                | WatchMask::MODIFY
                                | WatchMask::DELETE
                                | WatchMask::MOVED_FROM
                                | WatchMask::MOVED_TO
                                | WatchMask::DELETE_SELF
                                | WatchMask::MOVE_SELF,
                        ) {
                            Ok(wd) => {
                                watch = Some(wd);
                                watch_dir = path.clone();
                            }
                            Err(e) => {
                                log::warn!("watcher: add_watch {:?}: {e}", path);
                                watch = None;
                                watch_dir = PathBuf::new();
                            }
                        }
                        // The baseline is taken here, by this thread, after the
                        // subscription has been installed (or after installing it
                        // failed, in which case the listing still works and live
                        // updates do not — visible in the warning above). No other
                        // thread can enumerate this folder, so a change cannot land
                        // in a gap between the two acts: it is either in the snapshot
                        // or it arrives on the event queue afterwards.
                        enumerate_and_report(&path, session, dispatch, &res_tx, options);
                    }
                }
            }
        }
    }
}
