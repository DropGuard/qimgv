//! qimgv-engine: UI-agnostic directory model and scheduling core for qimgv.
//!
//! Pure Rust, no Qt. Everything Qt-facing (C ABI, eventfd notification,
//! string copying) lives in `qimgv-engine-ffi`.

pub mod engine;
pub mod error;
pub mod filter;
pub mod meta;
pub mod model;
pub mod scan;
pub mod sort;

pub use engine::{Command, Engine, EngineConfig, Event, Notify};
pub use error::EngineError;
pub use filter::NameFilter;
pub use meta::{ImageFormat, MetaInfo};
pub use model::{key_from_ffi, key_to_ffi, DirectoryState, EntryData, EntryKey, EntryRef};
pub use sort::SortMode;
