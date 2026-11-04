use std::collections::HashSet;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::time::Instant;
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
    /// Thumbnail scheduling: submit one request. `replace_batch` clears all
    /// pending (not yet dispatched) requests first — the caller sends it on
    /// the FIRST request of a fresh viewport batch, so an older batch dies
    /// structurally (requests that were never dispatched never decode).
    ThumbRequest { req: ThumbReq, replace_batch: bool },
    /// The C++ executor finished (or failed) one dispatched request.
    ThumbDone { req: ThumbReq },
    /// Drop all pending requests (directory switch).
    ThumbClear,
    /// How many decodes may run concurrently on the C++ side.
    ThumbConfig { max_in_flight: u32 },
    Shutdown,
}

/// A decode request; identity is the full tuple (path may repeat at
/// different sizes/crops).
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ThumbReq {
    pub path: PathBuf,
    pub size: u32,
    pub crop: bool,
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
    /// Incremental change applied by the engine watcher. `index` is the
    /// entry's position in the engine's own order — the single order decision,
    /// handed to the mirror so it never re-derives one.
    Mutated {
        kind: WatchChangeKind,
        path: String,
        name: String,
        is_dir: bool,
        size: u64,
        mtime_ms: i64,
        index: u32,
        epoch: u64,
    },
    /// Scheduler dispatches the next decode (C++ pool executes it).
    ThumbNext { req: ThumbReq },
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
    scan_ms: u128,
}

pub struct Engine {
    cmd_tx: Sender<Command>,
    events: Receiver<Event>,
    loop_join: Option<thread::JoinHandle<()>>,
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

/// Viewport-aware thumbnail scheduling policy. The C++ pool executes; this
/// decides WHAT decodes next and lets stale batches die structurally.
#[derive(Default)]
struct ThumbScheduler {
    pending: std::collections::VecDeque<ThumbReq>,
    in_flight: HashSet<ThumbReq>,
    max_in_flight: u32,
}

impl ThumbScheduler {
    fn request(&mut self, req: ThumbReq, replace_batch: bool) {
        if replace_batch {
            self.pending.clear();
        }
        if !self.in_flight.contains(&req) && !self.pending.contains(&req) {
            self.pending.push_back(req);
        }
    }

    fn done(&mut self, req: &ThumbReq) {
        self.in_flight.remove(req);
    }

    fn pump(&mut self, ev_tx: &Sender<Event>, notify: &Notify) {
        while self.in_flight.len() < self.max_in_flight as usize {
            let Some(req) = self.pending.pop_front() else { break };
            if !self.in_flight.insert(req.clone()) {
                continue;
            }
            let _ = ev_tx.send(Event::ThumbNext { req });
            fire_notify(notify);
        }
    }
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
                    let scan_start = Instant::now();
                    let result = catch_unwind(AssertUnwindSafe(|| {
                        scan_directory(&job.path, &options)
                    }))
                    .unwrap_or_else(|_| {
                        Err(EngineError::Io {
                            path: job.path.clone(),
                            source: std::io::Error::other("scan worker panicked"),
                        })
                    });
                    let scan_ms = scan_start.elapsed().as_millis();
                    if res_tx
                        .send(ScanDone { epoch: job.epoch, path: job.path, result, scan_ms })
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

        let loop_join = {
            let state = Arc::clone(&state);
            let show_hidden = Arc::clone(&show_hidden);
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
                let mut thumbs = ThumbScheduler { max_in_flight: 2, ..Default::default() };
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
                                            let _ = st.insert_entry(crate::model::EntryData {
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
                                Ok(Command::ThumbRequest { req, replace_batch }) => {
                                    thumbs.request(req, replace_batch);
                                    thumbs.pump(&ev_tx, &notify);
                                }
                                Ok(Command::ThumbDone { req }) => {
                                    thumbs.done(&req);
                                    thumbs.pump(&ev_tx, &notify);
                                }
                                Ok(Command::ThumbClear) => {
                                    thumbs.pending.clear();
                                }
                                Ok(Command::ThumbConfig { max_in_flight }) => {
                                    thumbs.max_in_flight = max_in_flight.max(1);
                                    thumbs.pump(&ev_tx, &notify);
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
                                    let entry_count = out.files.len();
                                    let apply_start = Instant::now();
                                    {
                                        let mut st = state_write(&state);
                                        st.replace_all(dir.clone(), out.files, out.dirs);
                                    }
                                    eprintln!(
                                        "[engine] scan {} ms + insert/sort {} ms ({} entries, off UI thread)",
                                        done.scan_ms,
                                        apply_start.elapsed().as_millis(),
                                        entry_count
                                    );
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
                                        watcher::apply_event(&mut st, &filter, &mut ov,
                                                             show_hidden.load(Ordering::Relaxed), &raw)
                                    }))
                                    .unwrap_or(watcher::WatchAction::Ignored)
                                };
                                match action {
                                    watcher::WatchAction::Changed { kind, path, name, is_dir, size, mtime_ms, index } => {
                                        let kind = match kind {
                                            watcher::Applied::Added => WatchChangeKind::Added,
                                            watcher::Applied::Modified => WatchChangeKind::Modified,
                                            watcher::Applied::Removed => WatchChangeKind::Removed,
                                        };
                                        let _ = ev_tx.send(Event::Mutated { kind, path, name, is_dir, size, mtime_ms, index, epoch: epoch_at() });
                                        fire_notify(&notify);
                                    }
                                    watcher::WatchAction::Reconcile { dir } => {
                                        // queue overflow: the kernel dropped events,
                                        // converge with a full rescan
                                        let e = epoch.fetch_add(1, Ordering::Relaxed) + 1;
                                        let _ = job_tx.send(ScanJob { epoch: e, path: dir });
                                    }
                                    watcher::WatchAction::DirGone { dir } => {
                                        let e = epoch.fetch_add(1, Ordering::Relaxed) + 1;
                                        let _ = job_tx.send(ScanJob { epoch: e, path: dir });
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
            })
        };

        Engine {
            cmd_tx,
            events: ev_rx,
            show_hidden,
            epoch: epoch_shared,
            state,
            loop_join: Some(loop_join),
        }
    }

    /// Serializes the whole current listing in ONE read-lock pass
    /// (see DirectoryState::write_listing for the wire format).
    /// Returns (needed_bytes, file_count, dir_count).
    pub fn export_listing(&self, buf: &mut [u8]) -> (usize, usize, usize) {
        let st = self.state.read().unwrap_or_else(|p| p.into_inner());
        st.write_listing(buf)
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
        // ...and wait for it. The C++ side calls the notify callback with a
        // pointer to its own eventfd member, and it frees this engine from
        // ~EngineBridge before closing that fd. Returning here while the loop
        // thread could still fire a callback would leave it writing through a
        // dangling pointer, and into a descriptor number that has been recycled.
        if let Some(j) = self.loop_join.take() {
            let _ = j.join();
        }
    }
}
