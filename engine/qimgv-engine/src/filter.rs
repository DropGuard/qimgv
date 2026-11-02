use std::collections::HashSet;
use std::sync::Arc;

/// Case-insensitive extension whitelist, mirroring qimgv's generated
/// `.*\.(jpg|png|...)$` filter (settings.cpp supportedFormatsRegex, used with
/// QRegularExpression::CaseInsensitiveOption in directorymanager.cpp).
///
/// The extension list is supplied by the C++ side because it derives from the
/// deployed QImageReader plugins, which differ per machine.
#[derive(Clone, Debug)]
pub struct NameFilter {
    // None = no restriction. An empty whitelist means the caller has nothing to
    // restrict with, which qimgv_engine.h documents as "accept everything" —
    // reading it as "accept nothing" hides every file in the folder instead.
    extensions: Option<Arc<HashSet<String>>>,
}

impl NameFilter {
    pub fn from_extensions<'a>(extensions: impl IntoIterator<Item = &'a str>) -> Self {
        let set: HashSet<String> = extensions
            .into_iter()
            .map(|e| e.trim_start_matches('.').to_ascii_lowercase())
            .filter(|e| !e.is_empty())
            .collect();
        Self {
            extensions: if set.is_empty() { None } else { Some(Arc::new(set)) },
        }
    }

    pub fn matches(&self, name: &str) -> bool {
        let Some(extensions) = &self.extensions else {
            return true;
        };
        match name.rsplit_once('.') {
            Some((_, ext)) if !ext.is_empty() => {
                extensions.contains(&ext.to_ascii_lowercase())
            }
            _ => false,
        }
    }
}
