//! The watcher thread and its file-descriptor machinery: the inotify fd, the
//! eventfd wake pipe, poll, and the one thread that owns them. Nothing here
//! runs under an interpreter (real syscalls only) — the semantics that tests
//! and Miri can reach live in `super::apply`.

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;

use crossbeam_channel::{Receiver, Sender};
use inotify::{Inotify, WatchMask};

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

/// A handle to the watcher thread: sends commands, wakes the poll, joins on
/// shutdown. (Events are tagged with the generation they belong to, so
/// in-flight events for a previous target are dropped after a retarget.)
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
    ) -> (WatcherHandle, Receiver<super::RawWatchEvent>) {
        let (cmd_tx, cmd_rx) = crossbeam_channel::unbounded::<WatchCmd>();
        let (ev_tx, ev_rx) = crossbeam_channel::unbounded::<super::RawWatchEvent>();
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
    ev_tx: &Sender<super::RawWatchEvent>,
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
                        let Some(kind) = super::classify(ev.mask.bits()) else {
                            crate::trace::line("drop", || format!("mask={:#x} bookkeeping", ev.mask.bits()));
                            continue;
                        };
                        let wd = ev.wd.get_watch_descriptor_id();
                        if !super::event_is_live(wd, kind, live_wd) {
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
                        if name.is_none()
                            && !matches!(kind, super::RawWatchKind::Overflow | super::RawWatchKind::WatchGone)
                        {
                            crate::trace::line("drop", || format!("kind={kind:?} no name"));
                            continue;
                        }
                        let raw = super::RawWatchEvent {
                            wd,
                            dir: watch_dir.clone(),
                            name,
                            is_dir: super::is_dir_mask(ev.mask.bits()),
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
