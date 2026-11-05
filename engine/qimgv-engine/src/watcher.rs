//! In-directory inotify watching (Linux). The watcher thread owns the
//! inotify fd and implements the drain discipline agreed in the consistency
//! discussions: the kernel queue is a conveyor belt, not a warehouse — a
//! dedicated thread reads with a large buffer and hands parsed events to the
//! engine loop through an ordered channel. IN_Q_OVERFLOW is surfaced so the
//! engine can reconcile with a full rescan.
//!
//! The module is split on a purity seam: [`apply`] is the event semantics —
//! parse, classify, and apply one event to the listing state — with no libc
//! anywhere in it, which makes it the surface the property tests and Miri can
//! run. [`fd`] is the machinery an interpreter cannot run: the inotify fd, the
//! eventfd wake pipe, poll, and the watcher thread itself.

mod apply;
mod fd;

pub use apply::{
    apply_event, classify, event_is_live, is_dir_mask, mtime_ms, Applied, IgnoreReason,
    RawWatchEvent, RawWatchKind, WatchAction,
};

pub(crate) use fd::{WatchCmd, WatcherHandle};
