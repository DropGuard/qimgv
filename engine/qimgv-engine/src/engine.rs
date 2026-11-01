use std::collections::HashSet;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock, RwLockWriteGuard};
use std::thread;

use crossbeam_channel::{unbounded, Receiver, Sender};

use crate::error::EngineError;
use crate::filter::NameFilter;
use crate::model::DirectoryState;
use crate::scan::{scan_directory, ScanOptions, ScanOutput};
use crate::sort::SortMode;
#[cfg(target_os = "linux")]
use crate::watcher::{self, WatcherHandle};

/// Called (from an engine thread) after an event was queued. The FFI layer
/// supplies an eventfd writer; tests supply a channel send. Foreign code may
/// panic — it is caught and ignored.
pub type Notify = Arc<dyn Fn() + Send + Sync>;

#[derive(Debug, Clone)]
pub enum Command {
    /// Opens a directory. `sort` is applied atomically with the scan (one
    /// DirListed event) — passing it separately would emit a stale resort
    /// event for the *previous* directory's state. `epoch` identifies the
    /// session (allocated by the caller via `next_epoch`).
    OpenDir { path: PathBuf, sort: Option<SortMode>, epoch: u64 },
    SetSort(SortMode),
    /// Adds a path to the listing bypassing the extension filter (app-side
    /// mime fallback); persists across rescans until the file disappears.
    IncludePath(PathBuf),
    /// Stops watching the current directory (listing reset on the C++ side).
    StopWatch,
    Shutdown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WatchChangeKind {
    Added,
    Modified,
    Removed,
}

#[derive(Debug)]
pub enum Event {
    /// Directory listing replaced (also emitted after SetSort so the UI
    /// repopulates; handles stay stable across re-sorts).
    DirListed { dir: PathBuf, epoch: u64 },
    OpenFailed { dir: PathBuf, error: Arc<EngineError>, epoch: u64 },
    /// Incremental change applied by the engine watcher.
    Mutated {
        kind: WatchChangeKind,
        path: String,
        name: String,
        is_dir: bool,
        size: u64,
        mtime_ms: i64,
        epoch: u64,
    },
}

#[derive(Clone)]
pub struct EngineConfig {
    pub scan_threads: usize,
    pub show_hidden: Arc<AtomicBool>,
    pub filter: NameFilter,
    pub initial_sort: SortMode,
}

struct ScanJob {
    epoch: u64,
    path: PathBuf,
}
struct ScanDone {
    epoch: u64,
    path: PathBuf,
    result: Result<ScanOutput, EngineError>,
}

pub struct Engine {
    cmd_tx: Sender<Command>,
    events: Receiver<Event>,
    show_hidden: Arc<AtomicBool>,
    epoch: Arc<AtomicU64>,
    pub state: Arc<RwLock<DirectoryState>>,
}

fn state_write(lock: &RwLock<DirectoryState>) -> RwLockWriteGuard<'_, DirectoryState> {
    match lock.write() {
        Ok(g) => g,
        Err(poisoned) => poisoned.into_inner(),
    }
}

fn fire_notify(notify: &Notify) {
    let _ = catch_unwind(AssertUnwindSafe(|| notify()));
}

impl Engine {
    /// Starts the engine loop thread, the scan worker pool and (on Linux) the
    /// inotify watcher thread. Dropping the returned handle shuts everything
    /// down (command channel closes → loop breaks → job channel closes →
    /// workers exit; watcher is joined explicitly).
    pub fn spawn(cfg: EngineConfig, notify: Notify) -> Engine {
        let (cmd_tx, cmd_rx) = unbounded();
        let (ev_tx, ev_rx) = unbounded::<Event>();
        let (job_tx, job_rx) = unbounded::<ScanJob>();
        let (res_tx, res_rx) = unbounded::<ScanDone>();
        let state = Arc::new(RwLock::new(DirectoryState::new(cfg.initial_sort)));
        let epoch = Arc::new(AtomicU64::new(0));
        let epoch_shared = Arc::clone(&epoch);
        let show_hidden = cfg.show_hidden;
        let filter = cfg.filter;
        let overrides: Arc<Mutex<HashSet<PathBuf>>> = Arc::new(Mutex::new(HashSet::new()));

        let mut workers = Vec::new();
        for _ in 0..cfg.scan_threads.max(1) {
            let job_rx = job_rx.clone();
            let res_tx = res_tx.clone();
            let filter = filter.clone();
            let show_hidden = Arc::clone(&show_hidden);
            let overrides = Arc::clone(&overrides);
            workers.push(thread::spawn(move || {
                while let Ok(job) = job_rx.recv() {
                    let options = ScanOptions {
                        show_hidden: Arc::clone(&show_hidden),
                        filter: filter.clone(),
                        overrides: Arc::clone(&overrides),
                    };
                    let result = catch_unwind(AssertUnwindSafe(|| {
                        scan_directory(&job.path, &options)
                    }))
                    .unwrap_or_else(|_| {
                        Err(EngineError::Io {
                            path: job.path.clone(),
                            source: std::io::Error::other("scan worker panicked"),
                        })
                    });
                    if res_tx
                        .send(ScanDone { epoch: job.epoch, path: job.path, result })
                        .is_err()
                    {
                        break;
                    }
                }
            }));
        }

        #[cfg(target_os = "linux")]
        let (watcher, watch_ev_rx) = {
            let (handle, rx) = WatcherHandle::spawn();
            (Some(handle), rx)
        };
        #[cfg(not(target_os = "linux"))]
        let watch_ev_rx: Receiver<(u64, ())> = crossbeam_channel::never();

        {
            let state = Arc::clone(&state);
            let epoch = Arc::clone(&epoch);
            let overrides = Arc::clone(&overrides);
            let filter = filter.clone();
            let epoch_at = {
                let e = Arc::clone(&epoch);
                move || e.load(Ordering::Relaxed)
            };
            thread::spawn(move || {
                let mut watch_gen: u64 = 0;
                let mut watched_dir = PathBuf::new();
                loop {
                    crossbeam_channel::select! {
                        recv(cmd_rx) -> cmd => {
                            match cmd {
                                Err(_) | Ok(Command::Shutdown) => break,
                                Ok(Command::OpenDir { path, sort, epoch: e }) => {
                                    watch_gen += 1;
                                    if let Some(mode) = sort {
                                        state_write(&state).resort(mode);
                                    }
                                    watched_dir = path.clone();
                                    #[cfg(target_os = "linux")]
                                    if let Some(w) = &watcher {
                                        w.send(watcher::WatchCmd::Watch {
                                            path: path.clone(),
                                            watch_gen,
                                        });
                                    }
                                    let _ = job_tx.send(ScanJob { epoch: e, path });
                                }
                                Ok(Command::SetSort(mode)) => {
                                    let dir = {
                                        let mut st = state_write(&state);
                                        st.resort(mode);
                                        st.dir().map(|d| d.to_owned())
                                    };
                                    if let Some(dir) = dir {
                                        let _ = ev_tx.send(Event::DirListed { dir, epoch: epoch_at() });
                                        fire_notify(&notify);
                                    }
                                }
                                Ok(Command::IncludePath(p)) => {
                                    let dir = {
                                        let meta = std::fs::metadata(&p);
                                        let mut ov = overrides.lock().unwrap_or_else(|e| e.into_inner());
                                        ov.insert(p.clone());
                                        let mut st = state_write(&state);
                                        if let Ok(m) = meta {
                                            st.insert_entry(crate::model::EntryData {
                                                name: p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
                                                path: p.to_string_lossy().into_owned(),
                                                is_dir: m.is_dir(),
                                                size: if m.is_dir() { 0 } else { m.len() },
                                                mtime: m.modified().unwrap_or(std::time::UNIX_EPOCH),
                                            });
                                        }
                                        st.dir().map(|d| d.to_owned())
                                    };
                                    if let Some(dir) = dir {
                                        let _ = ev_tx.send(Event::DirListed { dir, epoch: epoch_at() });
                                        fire_notify(&notify);
                                    }
                                }
                                Ok(Command::StopWatch) => {
                                    watch_gen += 1;
                                    watched_dir.clear();
                                    #[cfg(target_os = "linux")]
                                    if let Some(w) = &watcher {
                                        w.send(watcher::WatchCmd::Watch {
                                            path: PathBuf::new(),
                                            watch_gen,
                                        });
                                    }
                                }
                            }
                        }
                        recv(res_rx) -> done => {
                            let Ok(done) = done else { break };
                            // Stale scan: a newer OpenDir superseded it. Drop
                            // silently — this is the slice-1 cancellation path.
                            if done.epoch != epoch.load(Ordering::Relaxed) {
                                continue;
                            }
                            match done.result {
                                Ok(out) => {
                                    let dir = done.path.clone();
                                    {
                                        let mut st = state_write(&state);
                                        st.replace_all(dir.clone(), out.files, out.dirs);
                                    }
                                    let _ = ev_tx.send(Event::DirListed { dir, epoch: done.epoch });
                                    fire_notify(&notify);
                                }
                                Err(e) => {
                                    let _ = ev_tx.send(Event::OpenFailed {
                                        dir: done.path,
                                        error: Arc::new(e),
                                        epoch: done.epoch,
                                    });
                                    fire_notify(&notify);
                                }
                            }
                        }
                        recv(watch_ev_rx) -> tagged => {
                            #[cfg(target_os = "linux")]
                            {
                                let Ok((ev_gen, raw)) = tagged else { break };
                                if ev_gen != watch_gen || watched_dir.as_os_str().is_empty() {
                                    continue; // in-flight event for a previous target
                                }
                                let action = {
                                    let mut st = state_write(&state);
                                    let mut ov = overrides.lock().unwrap_or_else(|e| e.into_inner());
                                    catch_unwind(AssertUnwindSafe(|| {
                                        watcher::apply_event(&mut st, &watched_dir, &filter, &mut ov, &raw)
                                    }))
                                    .unwrap_or(watcher::WatchAction::Ignored)
                                };
                                match action {
                                    watcher::WatchAction::Changed { kind, path, name, is_dir, size, mtime_ms } => {
                                        let kind = match kind {
                                            watcher::Applied::Added => WatchChangeKind::Added,
                                            watcher::Applied::Modified => WatchChangeKind::Modified,
                                            watcher::Applied::Removed => WatchChangeKind::Removed,
                                        };
                                        let _ = ev_tx.send(Event::Mutated { kind, path, name, is_dir, size, mtime_ms, epoch: epoch_at() });
                                        fire_notify(&notify);
                                    }
                                    watcher::WatchAction::Reconcile => {
                                        // queue overflow: the kernel dropped events,
                                        // converge with a full rescan
                                        let e = epoch.fetch_add(1, Ordering::Relaxed) + 1;
                                        let _ = job_tx.send(ScanJob { epoch: e, path: watched_dir.clone() });
                                    }
                                    watcher::WatchAction::DirGone => {
                                        let e = epoch.fetch_add(1, Ordering::Relaxed) + 1;
                                        let _ = job_tx.send(ScanJob { epoch: e, path: watched_dir.clone() });
                                    }
                                    watcher::WatchAction::Ignored => {}
                                }
                            }
                            #[cfg(not(target_os = "linux"))]
                            {
                                let _ = tagged;
                            }
                        }
                    }
                }
                drop(job_tx);
                for h in workers {
                    let _ = h.join();
                }
                #[cfg(target_os = "linux")]
                if let Some(mut w) = watcher {
                    w.shutdown();
                }
            });
        }

        Engine {
            cmd_tx,
            events: ev_rx,
            show_hidden,
            epoch: epoch_shared,
            state,
        }
    }

    /// Current session epoch; bumped by every OpenDir/rescan.
    pub fn epoch(&self) -> u64 {
        self.epoch.load(Ordering::Relaxed)
    }

    /// Allocates the session epoch for an OpenDir the caller is about to send.
    pub fn next_epoch(&self) -> u64 {
        self.epoch.fetch_add(1, Ordering::Relaxed) + 1
    }

    pub fn send(&self, cmd: Command) -> Result<(), EngineError> {
        self.cmd_tx.send(cmd).map_err(|_| EngineError::EngineGone)
    }

    pub fn events(&self) -> &Receiver<Event> {
        &self.events
    }

    pub fn try_event(&self) -> Option<Event> {
        self.events.try_recv().ok()
    }

    pub fn set_show_hidden(&self, show: bool) {
        self.show_hidden.store(show, Ordering::Relaxed);
    }

    pub fn supports_watching() -> bool {
        cfg!(target_os = "linux")
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        // wakes the loop thread; it shuts workers and the watcher down
        let _ = self.cmd_tx.send(Command::Shutdown);
    }
}
