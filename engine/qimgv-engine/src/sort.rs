use std::cmp::Ordering;

use crate::model::EntryData;

/// Mirrors qimgv's `SortingMode` (settings.h). The C++ bridge maps values
/// explicitly, so the numbering here is the FFI contract (see qimgv_engine.h).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum SortMode {
    Name = 0,
    NameDesc = 1,
    Time = 2,
    TimeDesc = 3,
    Size = 4,
    SizeDesc = 5,
}

impl TryFrom<i32> for SortMode {
    type Error = crate::EngineError;

    fn try_from(v: i32) -> Result<Self, Self::Error> {
        match v {
            0 => Ok(Self::Name),
            1 => Ok(Self::NameDesc),
            2 => Ok(Self::Time),
            3 => Ok(Self::TimeDesc),
            4 => Ok(Self::Size),
            5 => Ok(Self::SizeDesc),
            other => Err(crate::EngineError::BadSortMode(other)),
        }
    }
}

/// Natural, case-insensitive name comparison — stand-in for QCollator's
/// numeric mode ("img2" < "img10"). Full ICU collation is deliberately out of
/// scope (decided in stack discussion).
fn natural(a: &str, b: &str) -> Ordering {
    natord::compare_ignore_case(a, b).then_with(|| a.cmp(b))
}

impl SortMode {
    pub fn compare(self, a: &EntryData, b: &EntryData) -> Ordering {
        match self {
            Self::Name => natural(&a.name, &b.name),
            Self::NameDesc => natural(&b.name, &a.name),
            Self::Time => a.mtime.cmp(&b.mtime).then_with(|| natural(&a.name, &b.name)),
            Self::TimeDesc => b.mtime.cmp(&a.mtime).then_with(|| natural(&a.name, &b.name)),
            Self::Size => a.size.cmp(&b.size).then_with(|| natural(&a.name, &b.name)),
            Self::SizeDesc => b.size.cmp(&a.size).then_with(|| natural(&a.name, &b.name)),
        }
    }
}
