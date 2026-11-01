use std::path::PathBuf;

#[derive(Debug, thiserror::Error)]
pub enum EngineError {
    #[error("I/O error on {}: {source}", path.display())]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("not a directory: {}", .0.display())]
    NotADirectory(PathBuf),
    #[error("engine command channel closed")]
    EngineGone,
    #[error("unknown sort mode: {0}")]
    BadSortMode(i32),
}
