use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use slotmap::{Key, KeyData, SlotMap};

use crate::sort::SortMode;

slotmap::new_key_type! {
    /// Generational handle to an entry. Stays valid (and keeps identifying the
    /// same entry) across re-sorts; becomes invalid once the entry is removed
    /// or the directory is replaced.
    pub struct EntryKey;
}

#[derive(Debug, Clone)]
pub struct EntryData {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
    pub size: u64,
    pub mtime: SystemTime,
}

#[derive(Debug)]
pub struct EntryRef<'a> {
    pub handle: EntryKey,
    pub data: &'a EntryData,
}

/// The single source of truth for the current directory. Mutation happens on
/// the engine loop thread; the FFI layer reads it through the shared
/// `Arc<RwLock<_>>` on the UI thread.
#[derive(Debug)]
pub struct DirectoryState {
    dir: Option<PathBuf>,
    entries: SlotMap<EntryKey, EntryData>,
    file_order: Vec<EntryKey>,
    dir_order: Vec<EntryKey>,
    file_pos: HashMap<EntryKey, u32>,
    dir_pos: HashMap<EntryKey, u32>,
    sort: SortMode,
}

impl DirectoryState {
    pub fn new(sort: SortMode) -> Self {
        Self {
            dir: None,
            entries: SlotMap::with_key(),
            file_order: Vec::new(),
            dir_order: Vec::new(),
            file_pos: HashMap::new(),
            dir_pos: HashMap::new(),
            sort,
        }
    }

    pub fn dir(&self) -> Option<&Path> {
        self.dir.as_deref()
    }

    pub fn sort(&self) -> SortMode {
        self.sort
    }

    /// Replaces the whole listing. Old handles stop resolving (generation bump
    /// + position maps rebuilt); new handles are assigned here.
    pub fn replace_all(&mut self, dir: PathBuf, files: Vec<EntryData>, dirs: Vec<EntryData>) {
        self.dir = Some(dir);
        self.entries.clear();
        self.file_order = files
            .into_iter()
            .map(|e| self.entries.insert(e))
            .collect();
        self.dir_order = dirs.into_iter().map(|e| self.entries.insert(e)).collect();
        self.reorder();
    }

    pub fn resort(&mut self, sort: SortMode) {
        self.sort = sort;
        self.reorder();
    }

    fn reorder(&mut self) {
        let mode = self.sort;
        {
            let (file_order, dir_order, entries) =
                (&mut self.file_order, &mut self.dir_order, &self.entries);
            file_order.sort_by(|&a, &b| mode.compare(&entries[a], &entries[b]));
            dir_order.sort_by(|&a, &b| mode.compare(&entries[a], &entries[b]));
        }
        self.file_pos = self
            .file_order
            .iter()
            .enumerate()
            .map(|(i, &k)| (k, i as u32))
            .collect();
        self.dir_pos = self
            .dir_order
            .iter()
            .enumerate()
            .map(|(i, &k)| (k, i as u32))
            .collect();
    }

    pub fn file_count(&self) -> usize {
        self.file_order.len()
    }

    pub fn dir_count(&self) -> usize {
        self.dir_order.len()
    }

    pub fn file_at(&self, index: usize) -> Option<EntryRef<'_>> {
        let &key = self.file_order.get(index)?;
        Some(EntryRef {
            handle: key,
            data: self.entries.get(key)?,
        })
    }

    pub fn dir_at(&self, index: usize) -> Option<EntryRef<'_>> {
        let &key = self.dir_order.get(index)?;
        Some(EntryRef {
            handle: key,
            data: self.entries.get(key)?,
        })
    }

    pub fn index_of_file(&self, handle: EntryKey) -> Option<u32> {
        self.file_pos.get(&handle).copied()
    }

    pub fn index_of_dir(&self, handle: EntryKey) -> Option<u32> {
        self.dir_pos.get(&handle).copied()
    }

    pub fn next_file(&self, handle: EntryKey) -> Option<EntryKey> {
        let i = *self.file_pos.get(&handle)? as usize;
        self.file_order.get(i + 1).copied()
    }

    pub fn prev_file(&self, handle: EntryKey) -> Option<EntryKey> {
        let i = *self.file_pos.get(&handle)? as usize;
        if i == 0 { None } else { self.file_order.get(i - 1).copied() }
    }

    pub fn next_dir(&self, handle: EntryKey) -> Option<EntryKey> {
        let i = *self.dir_pos.get(&handle)? as usize;
        self.dir_order.get(i + 1).copied()
    }

    pub fn prev_dir(&self, handle: EntryKey) -> Option<EntryKey> {
        let i = *self.dir_pos.get(&handle)? as usize;
        if i == 0 { None } else { self.dir_order.get(i - 1).copied() }
    }

    // --- incremental mutations (engine watcher) ---------------------------------

    /// Inserts an entry keeping the current sort order. Returns the new handle,
    /// or None if the path is already listed.
    pub fn insert_entry(&mut self, data: EntryData) -> Option<EntryKey> {
        if self.entries.values().any(|e| e.path == data.path) {
            return None;
        }
        let key = self.entries.insert(data);
        let is_dir = self.entries.get(key).unwrap().is_dir;
        let pos = {
            let mode = self.sort;
            let entries = &self.entries;
            let order = if is_dir { &mut self.dir_order } else { &mut self.file_order };
            let idx = order
                .binary_search_by(|&k| mode.compare(&entries[k], &entries[key]))
                .unwrap_or_else(|i| i);
            order.insert(idx, key);
            idx as u32
        };
        if is_dir {
            self.dir_pos.insert(key, pos);
        } else {
            self.file_pos.insert(key, pos);
        }
        Some(key)
    }

    /// Removes by full path from whichever order it lives in.
    pub fn remove_entry_by_path(&mut self, path: &str) -> Option<EntryKey> {
        let key = self
            .entries
            .iter()
            .find(|(_, e)| e.path == path)
            .map(|(k, _)| k)?;
        let (order, pos) = if self.entries.get(key).unwrap().is_dir {
            (&mut self.dir_order, &mut self.dir_pos)
        } else {
            (&mut self.file_order, &mut self.file_pos)
        };
        if let Some(i) = pos.remove(&key) {
            order.remove(i as usize);
            for v in pos.values_mut() {
                if *v > i {
                    *v -= 1;
                }
            }
        }
        self.entries.remove(key);
        Some(key)
    }

    /// (size, mtime) of a listed entry, for watcher change detection.
    pub fn entry_meta(&self, path: &str) -> Option<(u64, SystemTime)> {
        self.entries
            .values()
            .find(|e| e.path == path)
            .map(|e| (e.size, e.mtime))
    }

    pub fn update_entry_meta(&mut self, path: &str, size: u64, mtime: SystemTime) -> bool {
        let key = self
            .entries
            .iter()
            .find(|(_, e)| e.path == path)
            .map(|(k, _)| k);
        let Some(key) = key else { return false };
        let e = self.entries.get_mut(key).unwrap();
        e.size = size;
        e.mtime = mtime;
        true
    }

    // --- bulk listing export ------------------------------------------------

    /// Serializes the whole listing into `buf` (little-endian, one read-lock pass):
    ///
    ///   u32 file_count, u32 dir_count
    ///   then file_count + dir_count records (files first):
    ///     u64 handle, u64 size, i64 mtime_ms, u32 name_len, u32 path_len
    ///     name bytes, path bytes (no NUL)
    ///
    /// Returns (needed_bytes, file_count, dir_count). When `buf` is smaller than
    /// needed, nothing is written and needed_bytes tells the caller how much to
    /// allocate.
    pub fn write_listing(&self, buf: &mut [u8]) -> (usize, usize, usize) {
        let mut needed = 8usize;
        let mut records: Vec<(&EntryKey, &EntryData)> =
            Vec::with_capacity(self.file_order.len() + self.dir_order.len());
        for k in &self.file_order {
            records.push((k, &self.entries[*k]));
        }
        for k in &self.dir_order {
            records.push((k, &self.entries[*k]));
        }
        for (_, e) in &records {
            needed += 32 + e.name.len() + e.path.len();
        }
        let file_count = self.file_order.len();
        let dir_count = self.dir_order.len();
        if buf.len() < needed {
            return (needed, file_count, dir_count);
        }

        let mut off = 0usize;
        let mut put = |bytes: &[u8]| {
            buf[off..off + bytes.len()].copy_from_slice(bytes);
            off += bytes.len();
        };
        put(&(file_count as u32).to_le_bytes());
        put(&(dir_count as u32).to_le_bytes());
        for (k, e) in &records {
            put(&key_to_ffi(**k).to_le_bytes());
            put(&e.size.to_le_bytes());
            put(&crate::watcher::mtime_ms(e.mtime).to_le_bytes());
            put(&(e.name.len() as u32).to_le_bytes());
            put(&(e.path.len() as u32).to_le_bytes());
            put(e.name.as_bytes());
            put(e.path.as_bytes());
        }
        (needed, file_count, dir_count)
    }
}

// --- FFI helpers: stable bit representation for opaque handles --------------

pub fn key_to_ffi(key: EntryKey) -> u64 {
    key.data().as_ffi()
}

/// Decodes a handle from the FFI boundary. Only rejects the canonical
/// all-zero null handle; garbage bit patterns decode fine and are then
/// rejected naturally by the position maps when looked up.
pub fn key_from_ffi(bits: u64) -> Option<EntryKey> {
    if bits == 0 {
        return None;
    }
    Some(EntryKey::from(KeyData::from_ffi(bits)))
}
