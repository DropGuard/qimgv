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
    extensions: Arc<HashSet<String>>,
}

impl NameFilter {
    pub fn from_extensions<'a>(extensions: impl IntoIterator<Item = &'a str>) -> Self {
        Self {
            extensions: Arc::new(
                extensions
                    .into_iter()
                    .map(|e| e.trim_start_matches('.').to_ascii_lowercase())
                    .collect(),
            ),
        }
    }

    pub fn matches(&self, name: &str) -> bool {
        match name.rsplit_once('.') {
            Some((_, ext)) if !ext.is_empty() => {
                self.extensions.contains(&ext.to_ascii_lowercase())
            }
            _ => false,
        }
    }
}
