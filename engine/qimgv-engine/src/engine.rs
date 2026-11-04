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
use crate::scan::{ScanOptions, ScanOutput};
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
    /// Re-read the folder of the current request. Raised by the engine itself when
    /// the kernel queue overflowed or the watched folder vanished, and by a scan
    /// that was interrupted mid-flight; addressable from a test for the same reason
    /// it is needed in production: both are "the answer I have is not stable".
    Rescan(PathBuf),
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
        /// This engine's own count of the mutations it has emitted, starting at
        /// 1. Crosses the ABI as `qimgv_event.seq`.
        seq: u64,
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

/// One enumeration the watcher thread owes back to the loop: which request it
/// answers (`session`, `dispatch`) and which folder was read.
/// One directory-listing request, and the only place the loop decides whether a
/// result is still wanted. It replaces three loose counters that each answered a
/// piece of that question — `session` (whose answer the FFI filters on), a dispatch
/// number (newest scan writes the listing), and `watch_gen` (does this event belong
/// to the folder on screen, now answered by the event's own directory) — and adds
/// the two the counters could not express at all: whether a scan is in flight, and
/// whether the folder changed underneath it. A scan that was interrupted is not an
/// answer: it is taken again, because otherwise a file created or deleted inside
/// the window between asking for a watch and installing it would be in neither the
/// snapshot nor any event, and the listing and the mirror would settle on different
/// sets while their counts agreed.
struct ListingRequest {
    dir: PathBuf,
    session: u64,
    dispatch: u64,
}

impl ListingRequest {
    /// Take the listing again, as the same session: a rescan answers the request
    /// that already exists, it does not start one.
    fn rescan(&mut self, next: &mut u64) -> ScanJob {
        *next += 1;
        self.dispatch = *next;
        ScanJob { session: self.session, dispatch: self.dispatch, path: self.dir.clone() }
    }
}

pub(crate) struct ScanJob {
    session: u64,
    dispatch: u64,
    path: PathBuf,
}
pub(crate) struct ScanDone {
    pub(crate) session: u64,
    pub(crate) dispatch: u64,
    pub(crate) path: PathBuf,
    pub(crate) result: Result<ScanOutput, EngineError>,
    pub(crate) scan_ms: u128,
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
        // the enumeration results channel; its producer is the watcher thread
        let (res_tx, res_rx) = unbounded::<ScanDone>();
        let state = Arc::new(RwLock::new(DirectoryState::new(cfg.initial_sort)));
        let epoch = Arc::new(AtomicU64::new(0));
        let epoch_shared = Arc::clone(&epoch);
        let show_hidden = cfg.show_hidden;
        let filter = cfg.filter;
        let overrides: Arc<Mutex<HashSet<PathBuf>>> = Arc::new(Mutex::new(HashSet::new()));


        #[cfg(target_os = "linux")]
        let (watcher, watch_ev_rx) = {
            let options = ScanOptions {
                show_hidden: Arc::clone(&show_hidden),
                filter: filter.clone(),
                overrides: Arc::clone(&overrides),
            };
            let (handle, rx) = WatcherHandle::spawn(options, res_tx.clone());
            (Some(handle), rx)
        };
        #[cfg(not(target_os = "linux"))]
        let watch_ev_rx: Receiver<()> = crossbeam_channel::never();

        let loop_join = {
            let state = Arc::clone(&state);
            let show_hidden = Arc::clone(&show_hidden);
            let epoch = Arc::clone(&epoch);
            let overrides = Arc::clone(&overrides);
            let filter = filter.clone();
            let cmd_tx_for_loop = cmd_tx.clone();
            let epoch_at = {
                let e = Arc::clone(&epoch);
                move || e.load(Ordering::Relaxed)
            };
            thread::spawn(move || {
                let mut listing: Option<ListingRequest> = None;
                // only this thread dispatches scans, so the number needs no atomic
                let mut next_dispatch: u64 = 0;
                // Ordinal of the mutations this engine has emitted. Assigned at
                // birth, before the event can be dropped or truncated by anyone:
                // a number printed by the engine and a number printed by the C++
                // mirror identify the same event, which is what makes the two
                // halves of a trace comparable when one of them is missing a line.
                let mut next_mutation: u64 = 0;
                let mut thumbs = ThumbScheduler { max_in_flight: 2, ..Default::default() };
                loop {
                    crossbeam_channel::select! {
                        recv(cmd_rx) -> cmd => {
                            match cmd {
                                Err(_) | Ok(Command::Shutdown) => break,
                                Ok(Command::OpenDir { path, sort, epoch: e }) => {
                                    if let Some(mode) = sort {
                                        state_write(&state).resort(mode);
                                    }
                                    let mut req = ListingRequest {
                                        dir: path.clone(),
                                        session: e,
                                        dispatch: 0,
                                    };
                                    let job = req.rescan(&mut next_dispatch);
                                    listing = Some(req);
                                    #[cfg(target_os = "linux")]
                                    if let Some(w) = &watcher {
                                        w.send(watcher::WatchCmd::Watch {
                                            path: job.path,
                                            session: job.session,
                                            dispatch: job.dispatch,
                                        });
                                    }
                                }
                                Ok(Command::Rescan(dir)) => {
                                    // only the folder the current request is about
                                    // can be re-read; anything else is a result for
                                    // a folder nobody asked about any more
                                    let job = match &mut listing {
                                        Some(req) if req.dir == dir => Some(req.rescan(&mut next_dispatch)),
                                        _ => None,
                                    };
                                    #[cfg(target_os = "linux")]
                                    if let Some(job) = job {
                                        if let Some(w) = &watcher {
                                            w.send(watcher::WatchCmd::Enumerate {
                                                path: job.path,
                                                session: job.session,
                                                dispatch: job.dispatch,
                                            });
                                        }
                                    }
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
                                    listing = None;
                                    #[cfg(target_os = "linux")]
                                    if let Some(w) = &watcher {
                                        w.send(watcher::WatchCmd::Unwatch);
                                    }
                                }
                            }
                        }
                        recv(res_rx) -> done => {
                            let Ok(done) = done else { break };
                            // Three questions, in order, and the record answers all
                            // of them: is this the dispatch that still owns the
                            // answer; did the folder change while the scan ran; and
                            // only then — what does the answer say. A superseded
                            // dispatch is dropped silently because its replacement is
                            // already on its way; an interrupted one is not an answer
                            // at all, so nothing is reported until a scan lands with
                            // the folder quiet.
                            let current = listing.as_ref().map(|r| r.dispatch == done.dispatch).unwrap_or(false);
                            if !current {
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
                                    let _ = ev_tx.send(Event::DirListed { dir, epoch: done.session });
                                    fire_notify(&notify);
                                }
                                Err(e) => {
                                    let _ = ev_tx.send(Event::OpenFailed {
                                        dir: done.path,
                                        error: Arc::new(e),
                                        epoch: done.session,
                                    });
                                    fire_notify(&notify);
                                }
                            }
                        }
                        recv(watch_ev_rx) -> tagged => {
                            #[cfg(target_os = "linux")]
                            {
                                let Ok(raw) = tagged else { break };
                                // Only events about the folder the current request
                                // is answering for may touch the listing. There is no
                                // "arrived while a scan was in flight" case to mark:
                                // the same thread installs the watch and enumerates,
                                // so a change during the enumeration is either in the
                                // baseline or reaches this channel after it.
                                if !matches!(&listing, Some(req) if req.dir == raw.dir) {
                                    crate::trace::line("drop", || {
                                        let wanted = listing.as_ref().map(|r| r.dir.clone())
                                            .unwrap_or_default();
                                        format!("subject {:?} is not the requested {:?}", raw.dir, wanted)
                                    });
                                    continue;
                                }
                                let req_session = listing.as_ref().map(|r| r.session).unwrap_or(0);
                                let action = {
                                    let mut st = state_write(&state);
                                    let mut ov = overrides.lock().unwrap_or_else(|e| e.into_inner());
                                    catch_unwind(AssertUnwindSafe(|| {
                                        watcher::apply_event(&mut st, &filter, &mut ov,
                                                             show_hidden.load(Ordering::Relaxed), &raw)
                                    }))
                                    .unwrap_or_else(|payload| {
                                        // The guard exists so one bad event cannot
                                        // end the loop, but a guard that swallows
                                        // its payload turns a crash into a missing
                                        // file — indistinguishable from "nothing
                                        // happened". Say what actually unwound.
                                        let msg = payload
                                            .downcast_ref::<&str>().map(|s| (*s).to_string())
                                            .or_else(|| payload.downcast_ref::<String>().cloned())
                                            .unwrap_or_else(|| "<non-string payload>".to_string());
                                        eprintln!("[engine] watch event application panicked: {msg}");
                                        watcher::WatchAction::Ignored(watcher::IgnoreReason::Panicked)
                                    })
                                };
                                crate::trace::line("apply", || format!("{raw:?} -> {action:?}"));
                                match action {
                                    watcher::WatchAction::Changed { kind, path, name, is_dir, size, mtime_ms, index } => {
                                        let kind = match kind {
                                            watcher::Applied::Added => WatchChangeKind::Added,
                                            watcher::Applied::Modified => WatchChangeKind::Modified,
                                            watcher::Applied::Removed => WatchChangeKind::Removed,
                                        };
                                        next_mutation += 1;
                                        crate::trace::line("emit", || format!(
                                            "seq={next_mutation} {kind:?} {path} index={index} session={req_session}"));
                                        let _ = ev_tx.send(Event::Mutated { seq: next_mutation, kind, path, name, is_dir, size, mtime_ms, index, epoch: req_session });
                                        fire_notify(&notify);
                                    }
                                    watcher::WatchAction::Reconcile { dir } => {
                                        // queue overflow: the kernel dropped events,
                                        // converge with a full rescan
                                        crate::trace::line("reconcile", || format!("{dir:?}"));
                                        let _ = cmd_tx_for_loop.send(Command::Rescan(dir));
                                    }
                                    watcher::WatchAction::DirGone { dir } => {
                                        crate::trace::line("reconcile", || format!("gone {dir:?}"));
                                        let _ = cmd_tx_for_loop.send(Command::Rescan(dir));
                                    }
                                    // named by the apply line above
                                    watcher::WatchAction::Ignored(_) => {}
                                }
                            }
                            #[cfg(not(target_os = "linux"))]
                            {
                                let _ = tagged;
                            }
                        }
                    }
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
    /// Allocates the id of a NEW request. It is the only place a session id is
    /// born: the loop's own rescan (queue overflow, the watched folder deleted)
    /// re-answers the session that is already current and must not allocate here,
    /// because the stale-scan guard below asks "am I the newest" — so an internal
    /// bump silently cancelled every open that was in flight, without emitting a
    /// thing for the caller to notice.
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
