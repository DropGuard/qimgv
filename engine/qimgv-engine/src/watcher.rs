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
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawWatchEvent {
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

// Raw inotify masks (inotify(7)); kept independent of the crate's bitflags so
// parse_events can be tested against hand-built buffers.
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

/// Parses one read() buffer worth of `struct inotify_event` records.
pub fn parse_events(buffer: &[u8]) -> Vec<RawWatchEvent> {
    const HDR: usize = std::mem::size_of::<(i32, u32, u32, u32)>();
    let mut out = Vec::new();
    let mut offset = 0usize;
    while offset + HDR <= buffer.len() {
        let mask = u32::from_ne_bytes(buffer[offset + 4..offset + 8].try_into().unwrap());
        let len = u32::from_ne_bytes(buffer[offset + 12..offset + 16].try_into().unwrap()) as usize;
        let name_bytes = &buffer[offset + HDR..(offset + HDR + len).min(buffer.len())];
        offset += HDR + len;

        let kind = if mask & mask::Q_OVERFLOW != 0 {
            RawWatchKind::Overflow
        } else if mask & (mask::DELETE_SELF | mask::MOVE_SELF) != 0 {
            RawWatchKind::WatchGone
        } else if mask & mask::IGNORED != 0 {
            continue; // bookkeeping after rm_watch / auto-removal
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
            continue;
        };

        let name = if matches!(kind, RawWatchKind::Overflow | RawWatchKind::WatchGone) {
            None
        } else {
            let end = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_bytes.len());
            if end == 0 {
                continue;
            }
            Some(String::from_utf8_lossy(&name_bytes[..end]).into_owned())
        };
        out.push(RawWatchEvent {
            name,
            is_dir: mask & mask::ISDIR != 0,
            kind,
        });
    }
    out
}

// --- engine-side application --------------------------------------------------

/// What the engine loop should do with a parsed event.
#[derive(Debug, Clone, PartialEq)]
pub enum Applied {
    Added,
    Modified,
    Removed,
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
    Reconcile,
    /// The watched directory is gone.
    DirGone,
    Ignored,
}

/// Applies one parsed event to the state.
pub fn apply_event(
    state: &mut DirectoryState,
    dir: &Path,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    ev: &RawWatchEvent,
) -> WatchAction {
    use RawWatchKind as K;
    match ev.kind {
        K::Overflow => WatchAction::Reconcile,
        K::WatchGone => WatchAction::DirGone,
        K::Created | K::MovedTo => {
            let Some(name) = &ev.name else { return WatchAction::Ignored };
            let path = dir.join(name);
            // Directories bypass the extension whitelist. scan.rs puts every
            // directory in the listing without consulting it, and the watcher used
            // to run dir names through it too — so an external mkdir matched no
            // image extension and the folder never appeared until a full rescan.
            if !ev.is_dir && !filter.matches(name) && !overrides.contains(&path) {
                return WatchAction::Ignored;
            }
            let meta = match fs::metadata(&path) {
                Ok(m) => m,
                Err(_) => return WatchAction::Ignored, // vanished already
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
                WatchAction::Ignored
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
                WatchAction::Ignored
            }
        }
        K::Modified => {
            // A directory's own mtime carries no listing meaning (see the
            // canonical-dir note above), and no consumer mirrors it: the
            // engine state stays exactly as the scanner left it.
            if ev.is_dir {
                return WatchAction::Ignored;
            }
            let path = dir.join(ev.name.as_deref().unwrap_or(""));
            let path = path.to_string_lossy().into_owned();
            let Some((_, old_mtime)) = state.entry_meta(&path) else {
                return WatchAction::Ignored;
            };
            let meta = match fs::metadata(&path) {
                Ok(m) => m,
                Err(_) => return WatchAction::Ignored,
            };
            let mtime = meta.modified().unwrap_or(UNIX_EPOCH);
            if mtime == old_mtime {
                return WatchAction::Ignored;
            }
            let size = meta.len();
            let Some(index) = state.update_entry_meta(&path, size, mtime) else {
                return WatchAction::Ignored;
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

pub(crate) enum WatchCmd {
    /// (Re)target the watch; `watch_gen` tags all subsequent events.
    Watch { path: PathBuf, watch_gen: u64 },
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
    pub(crate) fn spawn() -> (WatcherHandle, Receiver<(u64, RawWatchEvent)>) {
        let (cmd_tx, cmd_rx) = crossbeam_channel::unbounded::<WatchCmd>();
        let (ev_tx, ev_rx) = crossbeam_channel::unbounded::<(u64, RawWatchEvent)>();
        let running = Arc::new(AtomicBool::new(true));
        let running_flag = Arc::clone(&running);

        // wake fd lets command sends interrupt the poll from any thread
        let wake = new_eventfd();
        // the loop gets its own descriptor onto the same eventfd: each end closes
        // exactly once by ownership, so no fd number is ever released twice
        let loop_wake = wake.try_clone().expect("dup the watcher wake fd");

        let join = std::thread::Builder::new()
            .name("qimgv-watch".into())
            .spawn(move || watcher_loop(&cmd_rx, &ev_tx, loop_wake, running_flag))
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

fn watcher_loop(
    cmd_rx: &Receiver<WatchCmd>,
    ev_tx: &Sender<(u64, RawWatchEvent)>,
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
    let mut watch_gen: u64 = 0;
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

        if fds[1].revents & libc::POLLIN != 0 {
            // commands: drain the wake counter, then the channel
            let mut buf = [0u8; 8];
            unsafe {
                while libc::read(wake.as_raw_fd(), buf.as_mut_ptr().cast(), 8) as isize == 8 {}
            }
            while let Ok(cmd) = cmd_rx.try_recv() {
                match cmd {
                    WatchCmd::Stop => return,
                    WatchCmd::Watch { path, watch_gen: g } => {
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
                                watch_gen = g;
                            }
                            Err(e) => {
                                log::warn!("watcher: add_watch {:?}: {e}", path);
                                watch_gen = g; // stay in sync; no events will come
                            }
                        }
                    }
                }
            }
        }

        if fds[0].revents & libc::POLLIN != 0 {
            match inotify.read_events(&mut buffer) {
                Ok(_events) => {
                    for ev in parse_events(&buffer) {
                        if ev_tx.send((watch_gen, ev)).is_err() {
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
    }
}
