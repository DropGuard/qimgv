//! The record stream's decision layer: what a batch of raw inotify records
//! means for the event stream. Two decisions live here, and they interlock.
//! The validation drops — bookkeeping masks, records of a watch that is no
//! longer installed, nameless subjects — and the cookie pairing, which folds
//! the kernel's two halves of one move into the single `Renamed` record
//! `apply_event` can decide on.
//!
//! The interlock is the reason this is one module: only a validated record may
//! break the adjacency a held MOVED_FROM relies on. A bookkeeping or dead-watch
//! record is not part of this watch's stream, so holding past it costs nothing;
//! anything that IS part of the stream and is not the matching MOVED_TO retires
//! the hold as the plain removal it always would have been. Splitting these
//! rules across a syscall loop would put a state machine nobody can test
//! against adversarial orderings inside the one file Miri refuses — so the
//! machine lives here, pure, and [`super::fd`] only feeds it and sends what it
//! yields.

use std::path::{Path, PathBuf};

use super::{classify, event_is_live, is_dir_mask, RawWatchEvent, RawWatchKind};

/// One raw inotify record, reduced to the fields the stream decisions read.
/// The kernel's record header parsing stays in the fd layer (the inotify crate
/// owns it); everything after that is here.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Record {
    pub wd: i32,
    /// The raw mask bits: classification and the directory flag are decisions
    /// of this layer, so it takes the bits, not a kind.
    pub bits: u32,
    pub cookie: u32,
    pub name: Option<String>,
}

/// Why a record produced no event. Every drop is nameable — a bare "ignored"
/// cannot be investigated, the same rule `apply`'s `IgnoreReason` follows.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DropReason {
    /// The mask classifies to nothing the listing reacts to (IN_IGNORED,
    /// ACCESS/OPEN/CLOSE and friends).
    Bookkeeping { bits: u32 },
    /// The record names a watch that is no longer installed — a delete for /A
    /// delivered after the watch moved to /B.
    DeadWatch { wd: i32, kind: RawWatchKind },
    /// Overflow or a watch-gone with no live folder to attribute it to.
    NoDirectory { kind: RawWatchKind },
    /// A subject-carrying record with no name in it.
    NoName { kind: RawWatchKind },
}

/// What one record (or the end of a batch) hands to the event stream, in order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Out {
    /// Send this event. `paired_cookie` is set exactly when the event is a
    /// fold of two records — the trace names the move it closed.
    Report { ev: RawWatchEvent, paired_cookie: Option<u32> },
    /// A held MOVED_FROM retired unpaired: its pair never showed up beside it,
    /// so it is reported as the removal it always would have been. The event
    /// names the watch and directory the FROM was SEEN in, which a retarget
    /// may no longer be watching.
    Unpaired { ev: RawWatchEvent, cookie: u32 },
    Drop(DropReason),
}

/// A MOVED_FROM that has been seen and not yet reported. The kernel emits the
/// two halves of one move back to back under the parent inode lock, so the
/// pair is the very next record — which is what makes holding it safe: a
/// rename out of the watched directory has no MOVED_TO here at all, and
/// waiting for one would delay a removal indefinitely.
struct Held {
    cookie: u32,
    /// The directory the record was seen in: a retarget can happen between the
    /// two acts, and a removal must name where it was seen, not where the
    /// thread is watching now.
    dir: PathBuf,
    wd: i32,
    name: String,
    is_dir: bool,
}

/// The pairing state machine, one batch at a time. `intake` takes records in
/// stream order; `settle` closes a batch — a hold that reaches it never had
/// its MOVED_TO beside it, because a 64 KiB read can in principle split a pair
/// across its boundary.
#[derive(Default)]
pub struct Pairer {
    held: Option<Held>,
}

impl Pairer {
    pub fn new() -> Self {
        Self { held: None }
    }

    pub fn intake(&mut self, rec: Record, dir: Option<&Path>, live_wd: Option<i32>) -> Vec<Out> {
        let mut out = Vec::new();
        let Some(kind) = classify(rec.bits) else {
            out.push(Out::Drop(DropReason::Bookkeeping { bits: rec.bits }));
            return out;
        };
        if !event_is_live(rec.wd, &kind, live_wd) {
            out.push(Out::Drop(DropReason::DeadWatch { wd: rec.wd, kind }));
            return out;
        }
        let Some(dir) = dir else {
            out.push(Out::Drop(DropReason::NoDirectory { kind }));
            return out;
        };
        let mut name = rec.name;
        if name.is_none()
            && !matches!(kind, RawWatchKind::Overflow | RawWatchKind::WatchGone)
        {
            out.push(Out::Drop(DropReason::NoName { kind }));
            return out;
        }
        let is_dir = is_dir_mask(rec.bits);

        // The matching MOVED_TO: the fold. The event reports with the
        // DESTINATION record's identity — the same watch, the same directory,
        // the name now on disk — and the source rides inside the kind.
        if matches!(kind, RawWatchKind::MovedTo)
            && self.held.as_ref().is_some_and(|h| h.cookie == rec.cookie)
        {
            let held = self.held.take().expect("pairing just checked the hold");
            out.push(Out::Report {
                ev: RawWatchEvent {
                    wd: rec.wd,
                    dir: dir.to_path_buf(),
                    name,
                    is_dir,
                    kind: RawWatchKind::Renamed { from: held.name },
                },
                paired_cookie: Some(rec.cookie),
            });
            return out;
        }
        // Anything else that is part of this stream breaks the adjacency. A
        // MOVED_FROM holds instead of reporting — its pair may be the very
        // next record — and everything else reports as itself.
        if let Some(held) = self.held.take() {
            out.push(Out::Unpaired {
                ev: RawWatchEvent {
                    wd: held.wd,
                    dir: held.dir,
                    name: Some(held.name),
                    is_dir: held.is_dir,
                    kind: RawWatchKind::MovedFrom,
                },
                cookie: held.cookie,
            });
        }
        if matches!(kind, RawWatchKind::MovedFrom) {
            self.held = Some(Held {
                cookie: rec.cookie,
                dir: dir.to_path_buf(),
                wd: rec.wd,
                name: name.take().unwrap_or_default(),
                is_dir,
            });
            return out;
        }
        out.push(Out::Report {
            ev: RawWatchEvent {
                wd: rec.wd,
                dir: dir.to_path_buf(),
                name,
                is_dir,
                kind,
            },
            paired_cookie: None,
        });
        out
    }

    /// The batch ended. A hold reaching here never had its MOVED_TO beside it.
    pub fn settle(&mut self) -> Vec<Out> {
        match self.held.take() {
            None => Vec::new(),
            Some(held) => vec![Out::Unpaired {
                ev: RawWatchEvent {
                    wd: held.wd,
                    dir: held.dir,
                    name: Some(held.name),
                    is_dir: held.is_dir,
                    kind: RawWatchKind::MovedFrom,
                },
                cookie: held.cookie,
            }],
        }
    }
}
