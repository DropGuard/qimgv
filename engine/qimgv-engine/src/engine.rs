use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, RwLock, RwLockWriteGuard};
use std::thread;

use crossbeam_channel::{unbounded, Receiver, Sender};

use crate::error::EngineError;
use crate::model::DirectoryState;
use crate::scan::{scan_directory, ScanOptions, ScanOutput};
use crate::sort::SortMode;

/// Called (from an engine thread) after an event was queued. The FFI layer
/// supplies an eventfd writer; tests supply a channel send. Foreign code may
/// panic — it is caught and ignored.
pub type Notify = Arc<dyn Fn() + Send + Sync>;

#[derive(Debug, Clone)]
pub enum Command {
    OpenDir { path: PathBuf },
    SetSort(SortMode),
    Shutdown,
}

#[derive(Debug)]
pub enum Event {
    /// Directory listing replaced (also emitted after SetSort so the UI
    /// repopulates; handles stay stable across re-sorts).
    DirListed { dir: PathBuf },
    OpenFailed { dir: PathBuf, error: Arc<EngineError> },
}

#[derive(Clone)]
pub struct EngineConfig {
    pub scan_threads: usize,
    pub show_hidden: Arc<AtomicBool>,
    pub filter: crate::filter::NameFilter,
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
    /// Starts the engine loop thread plus the scan worker pool. Dropping the
    /// returned handle shuts everything down (command channel closes → loop
    /// breaks → job channel closes → workers exit).
    pub fn spawn(cfg: EngineConfig, notify: Notify) -> Engine {
        let (cmd_tx, cmd_rx) = unbounded();
        let (ev_tx, ev_rx) = unbounded::<Event>();
        let (job_tx, job_rx) = unbounded::<ScanJob>();
        let (res_tx, res_rx) = unbounded::<ScanDone>();
        let state = Arc::new(RwLock::new(DirectoryState::new(cfg.initial_sort)));
        let epoch = Arc::new(AtomicU64::new(0));
        let show_hidden = cfg.show_hidden;
        let filter = cfg.filter;

        let mut workers = Vec::new();
        for _ in 0..cfg.scan_threads.max(1) {
            let job_rx = job_rx.clone();
            let res_tx = res_tx.clone();
            let filter = filter.clone();
            let show_hidden = Arc::clone(&show_hidden);
            workers.push(thread::spawn(move || {
                while let Ok(job) = job_rx.recv() {
                    let options = ScanOptions {
                        show_hidden: Arc::clone(&show_hidden),
                        filter: filter.clone(),
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

        {
            let state = Arc::clone(&state);
            let epoch = Arc::clone(&epoch);
            thread::spawn(move || {
                loop {
                    crossbeam_channel::select! {
                        recv(cmd_rx) -> cmd => {
                            match cmd {
                                Err(_) | Ok(Command::Shutdown) => break,
                                Ok(Command::OpenDir { path }) => {
                                    let e = epoch.fetch_add(1, Ordering::Relaxed) + 1;
                                    let _ = job_tx.send(ScanJob { epoch: e, path });
                                }
                                Ok(Command::SetSort(mode)) => {
                                    let dir = {
                                        let mut st = state_write(&state);
                                        st.resort(mode);
                                        st.dir().map(|d| d.to_owned())
                                    };
                                    if let Some(dir) = dir {
                                        let _ = ev_tx.send(Event::DirListed { dir });
                                        fire_notify(&notify);
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
                                    let _ = ev_tx.send(Event::DirListed { dir });
                                    fire_notify(&notify);
                                }
                                Err(e) => {
                                    let _ = ev_tx.send(Event::OpenFailed {
                                        dir: done.path,
                                        error: Arc::new(e),
                                    });
                                    fire_notify(&notify);
                                }
                            }
                        }
                    }
                }
                drop(job_tx);
                for h in workers {
                    let _ = h.join();
                }
            });
        }

        Engine { cmd_tx, events: ev_rx, show_hidden, state }
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
}
