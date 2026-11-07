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
//! run. [`pair`] is the record stream's decision layer — the validation drops
//! and the cookie pairing state machine — equally pure, so the orderings the
//! kernel rarely produces (interleaved cookies, pairs split by a read
//! boundary) are testable against it directly. [`fd`] is the machinery an
//! interpreter cannot run: the inotify fd, the eventfd wake pipe, poll, and
//! the watcher thread itself, which now only feeds records to `pair` and
//! sends what it yields.

mod apply;
mod fd;
mod pair;

pub use apply::{
    apply_event, classify, event_is_live, is_dir_mask, mtime_ms, Applied, IgnoreReason,
    RawWatchEvent, RawWatchKind, WatchAction,
};
pub use pair::{DropReason, Out, Pairer, Record};

pub(crate) use fd::{WatchCmd, WatcherHandle};
