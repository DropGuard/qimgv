use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering as AtomicOrdering};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

use crate::error::EngineError;
use crate::filter::NameFilter;
use crate::model::EntryData;

#[derive(Clone)]
pub struct ScanOptions {
    pub show_hidden: Arc<AtomicBool>,
    pub filter: NameFilter,
    /// Paths force-included by the app (mime fallback) — scanned even when
    /// the extension filter doesn't match.
    pub overrides: Arc<std::sync::Mutex<std::collections::HashSet<PathBuf>>>,
}

#[derive(Debug, Default)]
pub struct ScanOutput {
    pub files: Vec<EntryData>,
    pub dirs: Vec<EntryData>,
}

/// One pass over a single directory. Per-entry failures are skipped with a
/// warning (matching qimgv's behavior); only a failure to open the directory
/// itself is fatal.
///
/// Note on parity with directorymanager.cpp: directory entries get no
/// size/mtime (same as FSEntry defaults); symlink targets are followed for
/// type detection (fs::directory_entry::is_directory() behavior).
pub fn scan_directory(path: &Path, opts: &ScanOptions) -> Result<ScanOutput, EngineError> {
    let show_hidden = opts.show_hidden.load(AtomicOrdering::Relaxed);
    let mut out = ScanOutput::default();

    let rd = fs::read_dir(path)
        .map_err(|source| EngineError::Io { path: path.to_owned(), source })?;

    for entry in rd {
        let entry = match entry {
            Ok(e) => e,
            Err(source) => {
                log::warn!("scan: unreadable entry in {}: {source}", path.display());
                continue;
            }
        };
        let name = match entry.file_name().into_string() {
            Ok(n) => n,
            Err(_) => {
                log::warn!("scan: skipping non-UTF-8 name in {}", path.display());
                continue;
            }
        };
        #[cfg(unix)]
        let hidden = name.starts_with('.');
        #[cfg(windows)]
        let hidden = entry
            .metadata()
            .map(|m| m.file_attributes() & 0x2 != 0)
            .unwrap_or(false);
        if hidden && !show_hidden {
            continue;
        }

        let mut file_type = match entry.file_type() {
            Ok(ft) => ft,
            Err(source) => {
                log::warn!("scan: {}: {source}", path.display());
                continue;
            }
        };
        if file_type.is_symlink() {
            // DirEntry::file_type()/metadata() do not follow links; qimgv
            // (via std::filesystem) does.
            match fs::metadata(entry.path()) {
                Ok(m) => file_type = m.file_type(),
                Err(source) => {
                    log::warn!("scan: stat {}: {source}", entry.path().display());
                    continue;
                }
            }
        }

        let full_path = entry.path();
        let full_str = full_path.to_string_lossy().into_owned();

        let allowed = opts.filter.matches(&name)
            || opts
                .overrides
                .lock()
                .map(|o| o.contains(&full_path))
                .unwrap_or(false);

        if file_type.is_dir() {
            out.dirs.push(EntryData {
                name,
                path: full_str,
                is_dir: true,
                size: 0,
                mtime: UNIX_EPOCH,
            });
        } else if allowed {
            let meta = match entry.metadata() {
                Ok(m) => m,
                Err(source) => {
                    log::warn!("scan: stat {full_str}: {source}");
                    continue;
                }
            };
            out.files.push(EntryData {
                name,
                path: full_str,
                is_dir: false,
                size: meta.len(),
                mtime: meta.modified().unwrap_or(UNIX_EPOCH),
            });
        }
    }
    Ok(out)
}
