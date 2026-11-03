//! Property tests: invariants that must hold for ANY listing, which hand-written
//! cases cannot establish. Every one of these has a reason to exist — the engine
//! has already shipped drift of the kind asserted here (a 64-bit mtime read as 32
//! bits by the C++ consumer, an all-equal comparator letting the caller's order
//! diverge from the model's, and an empty extension whitelist meaning the
//! opposite of what the header documents). proptest shrinks a failure to a
//! minimal counterexample, which is the part manual cases never give.
//!
//! All of it is pure in-memory state: no filesystem, no threads, no engine.

use std::cmp::Ordering;
use std::collections::HashSet;
use std::ops::Range;
use std::path::PathBuf;
use std::time::{Duration, UNIX_EPOCH};

use proptest::prelude::*;

use qimgv_engine::{DirectoryState, EntryData, NameFilter, SortMode, key_from_ffi, key_to_ffi};

#[derive(Debug, Clone)]
struct Spec {
    name: String,
    size: u64,
    secs: u32,
}

impl Spec {
    fn entry(&self, is_dir: bool) -> EntryData {
        self.entry_in("/root", is_dir)
    }

    fn entry_in(&self, root: &str, is_dir: bool) -> EntryData {
        EntryData {
            name: self.name.clone(),
            path: format!("{root}/{}", self.name),
            is_dir,
            size: self.size,
            mtime: UNIX_EPOCH + Duration::from_secs(self.secs as u64),
        }
    }
}

/// Unique names, guaranteed by the index prefix, so no property has to reason
/// about the slotmap's dedup behaviour by accident.
///
/// The seconds range is epoch-sized on purpose: mtime crosses the wire in
/// milliseconds, and a field that small is always the true value squeezed into
/// 32 bits. That is the exact bug this file exists to catch.
fn specs(range: Range<usize>) -> impl Strategy<Value = Vec<Spec>> {
    prop::collection::vec(("[a-z0-9.]{1,8}", any::<u64>(), 1_000_000_000u32..2_000_000_000), range).prop_map(|v| {
        v.into_iter()
            .enumerate()
            .map(|(i, (name, size, secs))| Spec { name: format!("{i:02}{name}"), size, secs })
            .collect()
    })
}

fn any_mode() -> impl Strategy<Value = SortMode> {
    (0..6i32).prop_map(|m| SortMode::try_from(m).unwrap())
}

proptest! {
    // The comparator must be a strict weak order. The C++ mirror of this rule was
    // handed a comparator that judged every directory equal, which let std::sort
    // produce an arbitrary order that then disagreed with what the view showed.
    // Antisymmetry and transitivity are the properties that make that impossible.
    #[test]
    fn comparator_is_a_strict_weak_order(a in specs(1..2), b in specs(1..2), c in specs(1..2), mode in any_mode()) {
        let (x, y, z) = (&a[0], &b[0], &c[0]);
        let cmp = |p: &Spec, q: &Spec| mode.compare(&p.entry(false), &q.entry(false));
        prop_assert_eq!(cmp(x, x), Ordering::Equal);
        prop_assert_eq!(cmp(x, y), cmp(y, x).reverse());
        if cmp(x, y) == Ordering::Less && cmp(y, z) == Ordering::Less {
            prop_assert_eq!(cmp(x, z), Ordering::Less);
        }
    }

    // Equal primary keys fall back to ascending name order in every mode except
    // NameDesc, whose point is descending names. SortMode::Name is the oracle for
    // that order: a tiebreak that follows the mode's own direction instead keeps
    // the listing self-consistent, so only comparison against Name exposes it —
    // and it is the divergence that made the folder view and navigation disagree.
    #[test]
    fn equal_primary_keys_tiebreak_by_name_order(a in specs(1..2), b in specs(1..2), mode in any_mode()) {
        let x = &a[0];
        let y = Spec { name: b[0].name.clone(), size: x.size, secs: x.secs };
        let (ea, eb) = (x.entry(false), y.entry(false));
        let by_name = SortMode::Name.compare(&ea, &eb);
        let want = if mode == SortMode::NameDesc { by_name.reverse() } else { by_name };
        prop_assert_eq!(mode.compare(&ea, &eb), want, "name tiebreak changed");
    }

    #[test]
    fn listing_is_sorted_and_indexed_consistently(fs in specs(0..10), ds in specs(0..6), mode in any_mode()) {
        let mut st = DirectoryState::new(SortMode::Name);
        st.replace_all(PathBuf::from("/root"),
                       fs.iter().map(|s| s.entry(false)).collect(),
                       ds.iter().map(|s| s.entry(true)).collect());
        st.resort(mode);

        let got: Vec<&String> = (0..st.file_count()).map(|i| &st.file_at(i).unwrap().data.name).collect();
        let mut want: Vec<&Spec> = fs.iter().collect();
        want.sort_by(|p, q| mode.compare(&p.entry(false), &q.entry(false)));
        prop_assert_eq!(got, want.iter().map(|s| &s.name).collect::<Vec<_>>());

        let mut seen = HashSet::new();
        for i in 0..st.file_count() {
            let r = st.file_at(i).unwrap();
            prop_assert_eq!(st.index_of_file(r.handle), Some(i as u32));
            prop_assert!(seen.insert(key_to_ffi(r.handle)), "duplicate handle in the listing");
        }
        prop_assert_eq!(seen.len(), st.file_count());

        // directories travel in their own order and their own index map, so both
        // need the same guarantee the files got above
        let got_dirs: Vec<&String> =
            (0..st.dir_count()).map(|i| &st.dir_at(i).unwrap().data.name).collect();
        let mut want_dirs: Vec<&Spec> = ds.iter().collect();
        want_dirs.sort_by(|p, q| mode.compare(&p.entry(true), &q.entry(true)));
        prop_assert_eq!(got_dirs, want_dirs.iter().map(|s| &s.name).collect::<Vec<_>>());
        for i in 0..st.dir_count() {
            let r = st.dir_at(i).unwrap();
            prop_assert_eq!(st.index_of_dir(r.handle), Some(i as u32));
        }
    }

    // next/prev must be a total traversal and exact inverses: the C++ navigation
    // walks these, so a cycle or a lost entry is user-visible.
    #[test]
    fn next_prev_walk_covers_every_entry_once(fs in specs(1..10)) {
        let mut st = DirectoryState::new(SortMode::Name);
        st.replace_all(PathBuf::from("/root"), fs.iter().map(|s| s.entry(false)).collect(), vec![]);

        let first = st.file_at(0).unwrap().handle;
        let mut walked = vec![first];
        while let Some(n) = st.next_file(*walked.last().unwrap()) {
            walked.push(n);
            prop_assert!(walked.len() <= fs.len(), "next_file never terminates");
        }
        prop_assert_eq!(walked.len(), fs.len());
        let positions: Vec<u32> = walked.iter().map(|h| st.index_of_file(*h).unwrap()).collect();
        prop_assert_eq!(positions, (0..fs.len() as u32).collect::<Vec<_>>());
        for pair in walked.windows(2) {
            prop_assert_eq!(st.prev_file(pair[1]), Some(pair[0]));
            prop_assert_eq!(st.next_file(pair[0]), Some(pair[1]));
        }
        prop_assert_eq!(st.prev_file(first), None);
    }

    #[test]
    fn resort_keeps_handles_and_replace_all_invalidates_them(fs in specs(2..8), extra in specs(1..2)) {
        let mut st = DirectoryState::new(SortMode::Name);
        st.replace_all(PathBuf::from("/root"), fs.iter().map(|s| s.entry(false)).collect(), vec![]);
        let old: Vec<_> = (0..st.file_count()).map(|i| st.file_at(i).unwrap().handle).collect();

        st.resort(SortMode::SizeDesc);
        for h in &old {
            prop_assert!(st.index_of_file(*h).is_some(), "handle lost across a resort");
        }

        // a wholly different path set, so an entry left behind by the first
        // listing cannot be mistaken for one the second listing re-inserted
        let mut merged: Vec<EntryData> = fs.iter().map(|s| s.entry_in("/other", false)).collect();
        merged.extend(extra.iter().map(|s| s.entry_in("/other", false)));
        st.replace_all(PathBuf::from("/other"), merged.clone(), vec![]);
        for h in &old {
            prop_assert_eq!(st.index_of_file(*h), None, "stale handle still resolves after replace_all");
        }
        // the position map gets rebuilt either way, so it proves nothing about the
        // entry table. A leftover entry from the previous directory keeps its path
        // in the table, and insert_entry refuses a new file whose path is already
        // there — which is how a directory switch could swallow a creation.
        for e in &fs {
            prop_assert!(st.entry_meta(&e.entry(false).path).is_none(),
                         "previous listing still occupies the entry table");
        }
        prop_assert_eq!(st.file_count(), merged.len());
    }

    // The arena is consumed by C++ (EngineBridge::parseListing), which cannot be
    // called from here, so this file grows its own reader from the documented
    // format instead. Two independent implementations agreeing on arbitrary data is
    // the point: one of them reading a 64-bit field as 32 bits is what this catches.
    #[test]
    fn arena_round_trips_and_only_depends_on_the_listing(fs in specs(0..8), ds in specs(0..4), slack in 0usize..5) {
        let mut st = DirectoryState::new(SortMode::Name);
        let files: Vec<EntryData> = fs.iter().map(|s| s.entry(false)).collect();
        let dirs: Vec<EntryData> = ds.iter().map(|s| s.entry(true)).collect();
        st.replace_all(PathBuf::from("/root"), files.clone(), dirs.clone());

        let needed = 8 + files.iter().chain(&dirs)
            .map(|e| 32 + e.name.len() + e.path.len()).sum::<usize>();
        let mut buf = vec![0u8; needed + slack];
        let (n, fc, dc) = st.write_listing(&mut buf);
        prop_assert_eq!((n, fc, dc), (needed, files.len(), dirs.len()));

        let (got_files, got_dirs) = read_arena(&buf[..needed]);
        prop_assert_eq!(got_files.len(), files.len());
        // the handle is what C++ navigates by, and the arena position is what it
        // reads back as an index — both have to agree with the live state, or the
        // far side of the FFI is holding a reference to something else
        for (i, g) in got_files.iter().enumerate() {
            let k = key_from_ffi(g.handle);
            prop_assert!(k.is_some(), "arena handle does not decode");
            prop_assert_eq!(st.index_of_file(k.unwrap()), Some(i as u32),
                            "arena position is not the model index");
            // each row is checked against the state's row at the same index, not
            // against the order it happened to be inserted in
            let row = st.file_at(i).unwrap();
            prop_assert_eq!(row.handle, k.unwrap(), "handle/row mismatch");
            // names travel as UTF-8 bytes; a char-count disagreement shows up here
            prop_assert_eq!(&g.name, &row.data.name);
            prop_assert_eq!(&g.path, &row.data.path);
            prop_assert_eq!(g.size, row.data.size);
            prop_assert_eq!(g.mtime_ms, row.data.mtime.duration_since(UNIX_EPOCH).unwrap().as_millis() as i64);
            prop_assert!(!g.is_dir);
        }
        prop_assert_eq!(got_dirs.len(), dirs.len());
        for (i, g) in got_dirs.iter().enumerate() {
            let k = key_from_ffi(g.handle);
            prop_assert!(k.is_some(), "arena handle does not decode");
            prop_assert_eq!(st.index_of_dir(k.unwrap()), Some(i as u32),
                            "arena position is not the model index");
            prop_assert_eq!(&g.name, &st.dir_at(i).unwrap().data.name);
            prop_assert!(g.is_dir);
        }

        if slack > 0 {
            let mut tight = vec![0u8; needed];
            st.write_listing(&mut tight);
            prop_assert_eq!(&buf[..needed], &tight[..], "extra capacity changed the encoding");
        }
        if needed > 0 {
            let mut short = vec![0xAAu8; needed - 1];
            let (n2, _, _) = st.write_listing(&mut short);
            prop_assert_eq!(n2, needed, "a too-small buffer must still report the true size");
            prop_assert!(short.iter().all(|&b| b == 0xAA), "a refused write must not touch the buffer");
        }
    }

    #[test]
    fn whitelist_is_case_insensitive_and_empty_means_no_restriction(
        base in "[a-zA-Z0-9]{1,6}",
        ext in "[a-zA-Z]{1,4}",
        // at least one entry: a whitelist with no usable tokens is the "no
        // restriction" case, asserted above, and shares that behaviour with
        // a list of nothing but dots
        listed in prop::collection::vec("[a-z]{1,4}", 1..4),
    ) {
        let name = format!("{base}.{ext}");
        // an empty whitelist is documented as "accept everything" by the C header
        prop_assert!(NameFilter::from_extensions([]).matches(&name));
        prop_assert!(NameFilter::from_extensions([]).matches("noextension"));

        let filter = NameFilter::from_extensions(listed.iter().map(|s| s.as_str()));
        let expected = listed.iter().any(|e| e.eq_ignore_ascii_case(&ext));
        prop_assert_eq!(filter.matches(&name), expected);

        let with_dots: Vec<String> = listed.iter().map(|s| format!(".{s}")).collect();
        let dotted = NameFilter::from_extensions(with_dots.iter().map(|s| s.as_str()));
        prop_assert_eq!(dotted.matches(&name), expected);

        // under a real whitelist a name with no extension is not a match
        prop_assert!(!filter.matches(&base));

        // a list whose every token is a stray dot has no usable extension, so
        // it collapses to the no-restriction case rather than hiding the folder
        let dots = NameFilter::from_extensions([".", ".."]);
        prop_assert!(dots.matches(&name));
        prop_assert!(dots.matches(&base));
    }
}

/// A second implementation of the documented wire format, written from the
/// comment in model.rs rather than from the serializer itself.
#[derive(Debug)]
struct Rec {
    handle: u64,
    name: String,
    path: String,
    size: u64,
    mtime_ms: i64,
    is_dir: bool,
}

fn read_arena(buf: &[u8]) -> (Vec<Rec>, Vec<Rec>) {
    fn u32le(b: &[u8], p: usize) -> u32 { u32::from_le_bytes(b[p..p + 4].try_into().unwrap()) }
    fn u64le(b: &[u8], p: usize) -> u64 { u64::from_le_bytes(b[p..p + 8].try_into().unwrap()) }

    let (fc, dc) = (u32le(buf, 0) as usize, u32le(buf, 4) as usize);
    let mut off = 8usize;
    let mut files = Vec::new();
    let mut dirs = Vec::new();
    for (count, is_dir) in [(fc, false), (dc, true)] {
        for _ in 0..count {
            let handle = u64le(buf, off);
            let size = u64le(buf, off + 8);
            let mtime_ms = i64::from_le_bytes(buf[off + 16..off + 24].try_into().unwrap());
            let nlen = u32le(buf, off + 24) as usize;
            let plen = u32le(buf, off + 28) as usize;
            let name = String::from_utf8(buf[off + 32..off + 32 + nlen].to_vec()).unwrap();
            let path = String::from_utf8(buf[off + 32 + nlen..off + 32 + nlen + plen].to_vec()).unwrap();
            off += 32 + nlen + plen;
            (if is_dir { &mut dirs } else { &mut files }).push(Rec { handle, name, path, size, mtime_ms, is_dir });
        }
    }
    (files, dirs)
}

// --- the self-consistency contract -------------------------------------------
//
// After ANY mutation sequence in ANY sort mode, the order vectors must be
// sorted by the comparator and the position maps must tell the truth about
// them. update_entry_meta used to skip the re-seating, so a size change under
// SIZE sort silently unsorted the model — and every export after it.

#[derive(Debug, Clone)]
enum Op {
    Insert(usize, bool),
    Remove(usize),
    Update(usize, u64, u32),
}

fn op_strategy() -> impl Strategy<Value = Op> {
    prop_oneof![
        3 => (0..6usize, any::<bool>()).prop_map(|(i, is_dir)| Op::Insert(i, is_dir)),
        2 => (0..6usize).prop_map(Op::Remove),
        2 => (0..6usize, any::<u64>(), 1_000_000_000u32..2_000_000_000)
            .prop_map(|(i, size, secs)| Op::Update(i, size % 4096, secs)),
    ]
}

fn assert_well_ordered(st: &DirectoryState) {
    let mode = st.sort();
    for i in 0..st.file_count() {
        let e = st.file_at(i).unwrap();
        assert_eq!(st.index_of_file(e.handle), Some(i as u32), "position map lies about {}", e.data.name);
        if i > 0 {
            let prev = st.file_at(i - 1).unwrap();
            assert_ne!(
                mode.compare(prev.data, e.data),
                Ordering::Greater,
                "file order unsorted at {i}: {} vs {}",
                prev.data.name,
                e.data.name
            );
        }
    }
    for i in 0..st.dir_count() {
        let e = st.dir_at(i).unwrap();
        assert_eq!(st.index_of_dir(e.handle), Some(i as u32));
        if i > 0 {
            let prev = st.dir_at(i - 1).unwrap();
            assert_ne!(mode.compare(prev.data, e.data), Ordering::Greater, "dir order unsorted at {i}");
        }
    }
}

proptest! {
    #[test]
    fn mutations_leave_the_model_well_ordered(
        specs_in in specs(1..6),
        ops in prop::collection::vec(op_strategy(), 0..40),
        mode in any_mode(),
    ) {
        let mut st = DirectoryState::new(mode);
        let mut alive: Vec<(Spec, bool)> = Vec::new();
        for op in ops {
            match op {
                Op::Insert(i, is_dir) => {
                    let spec = specs_in[i % specs_in.len()].clone();
                    // the model dedups on full path, files and dirs alike
                    if alive.iter().any(|(s, _)| s.name == spec.name) {
                        continue;
                    }
                    let inserted = st.insert_entry(spec.entry_in("/root", is_dir));
                    prop_assert!(inserted.is_some(), "insert of a fresh path must succeed");
                    alive.push((spec, is_dir));
                }
                Op::Remove(i) => {
                    if alive.is_empty() {
                        continue;
                    }
                    let (spec, _) = alive.remove(i % alive.len());
                    let removed = st.remove_entry_by_path(&format!("/root/{}", spec.name));
                    prop_assert!(removed.is_some(), "removing a listed path must succeed");
                }
                Op::Update(i, size, secs) => {
                    if alive.is_empty() {
                        continue;
                    }
                    let (spec, _) = &alive[i % alive.len()];
                    let path = format!("/root/{}", spec.name);
                    let mtime = UNIX_EPOCH + Duration::from_secs(secs as u64);
                    let updated = st.update_entry_meta(&path, size, mtime);
                    prop_assert!(updated.is_some(), "updating a listed path must succeed");
                }
            }
            assert_well_ordered(&st);
        }
    }
}
