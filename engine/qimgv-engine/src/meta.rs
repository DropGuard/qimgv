//! Single-pass header probe: format + dimensions + animation flag + EXIF
//! orientation from ONE file open. Replaces qimgv's DocumentInfo behavior,
//! which reopens the file 3-5 times per image (mime sniff, per-format
//! animation detection, EXIF orientation each with its own handle).
//!
//! `probe` is a pure function — thread-safe by construction, no engine state.
//! Parse failures are NOT errors: they yield `ImageFormat::Unknown` / zero
//! dimensions so callers can fall back to the full decode path.

use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::Path;

use crate::error::EngineError;

/// Mirrors the format family qimgv's DocumentInfo detects. Video formats are
/// out of scope (mpv owns their metadata).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum ImageFormat {
    Unknown = 0,
    Jpeg = 1,
    Png = 2,
    Gif = 3,
    WebP = 4,
    Avif = 5,
    Jxl = 6,
    Bmp = 7,
    Tiff = 8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MetaInfo {
    pub format: ImageFormat,
    pub animated: bool,
    /// EXIF orientation 1..=8; 1 means normal/unknown.
    pub orientation: u32,
    /// 0 = unknown (header truncated or format not size-parseable).
    pub width: u32,
    pub height: u32,
}

impl Default for MetaInfo {
    fn default() -> Self {
        Self {
            format: ImageFormat::Unknown,
            animated: false,
            orientation: 1,
            width: 0,
            height: 0,
        }
    }
}

/// How much of the file head to read. Enough for JPEG APP1/EXIF + SOF in
/// practice, PNG IHDR + acTL, WebP/AVIF boxes.
const HEAD_LEN: usize = 64 * 1024;

pub fn probe(path: &Path) -> Result<MetaInfo, EngineError> {
    let mut file = File::open(path)
        .map_err(|source| EngineError::Io { path: path.to_owned(), source })?;

    let mut head = [0u8; HEAD_LEN];
    let mut filled = 0;
    loop {
        let n = file
            .read(&mut head[filled..])
            .map_err(|source| EngineError::Io { path: path.to_owned(), source })?;
        if n == 0 || filled + n == HEAD_LEN {
            filled += n;
            break;
        }
        filled += n;
    }
    let head = &head[..filled];

    let mut info = MetaInfo {
        format: detect_format(head),
        ..Default::default()
    };
    if info.format == ImageFormat::Unknown {
        return Ok(info);
    }

    info.animated = detect_animated(info.format, head);
    info.orientation = detect_orientation(info.format, &file).unwrap_or(1);

    match imagesize::blob_size(head) {
        Ok(sz) => {
            info.width = sz.width as u32;
            info.height = sz.height as u32;
        }
        Err(_) => {
            // header partially beyond our snapshot — retry with the real file
            let mut br = BufReader::new(&file);
            let _ = br.seek(SeekFrom::Start(0));
            if let Ok(sz) = imagesize::reader_size(&mut br) {
                info.width = sz.width as u32;
                info.height = sz.height as u32;
            }
        }
    }
    Ok(info)
}

fn starts_with(head: &[u8], magic: &[u8]) -> bool {
    head.len() >= magic.len() && &head[..magic.len()] == magic
}

fn detect_format(head: &[u8]) -> ImageFormat {
    if starts_with(head, b"\xff\xd8\xff") {
        ImageFormat::Jpeg
    } else if starts_with(head, b"\x89PNG\r\n\x1a\n") {
        ImageFormat::Png
    } else if starts_with(head, b"GIF8") {
        ImageFormat::Gif
    } else if head.len() >= 12 && &head[0..4] == b"RIFF" && &head[8..12] == b"WEBP" {
        ImageFormat::WebP
    } else if head.len() >= 12
        && &head[4..8] == b"ftyp"
        && (&head[8..12] == b"avif" || &head[8..12] == b"avis")
    {
        ImageFormat::Avif
    } else if starts_with(head, b"BM") {
        ImageFormat::Bmp
    } else if starts_with(head, b"II*\x00") || starts_with(head, b"MM\x00*") {
        ImageFormat::Tiff
    } else if starts_with(head, b"\xff\x0a")
        || (head.len() >= 12 && &head[0..12] == b"\x00\x00\x00\x0cJXL \r\n\x87\n")
    {
        ImageFormat::Jxl
    } else {
        ImageFormat::Unknown
    }
}

fn detect_animated(format: ImageFormat, head: &[u8]) -> bool {
    match format {
        // qimgv treats every gif as animated (documentinfo.cpp)
        ImageFormat::Gif => true,
        ImageFormat::Png => head.windows(4).any(|w| w == b"acTL"),
        // VP8X payload flags at head[20]; bit 1 = animation (same offsets as
        // documentinfo.cpp detectAnimatedWebP)
        ImageFormat::WebP => head.len() >= 21 && &head[12..16] == b"VP8X" && head[20] & 0x02 != 0,
        // "avis" brand = animated AVIF (documentinfo.cpp detectAnimatedAvif)
        ImageFormat::Avif => head.len() >= 12 && &head[8..12] == b"avis",
        // TODO: animated JXL (needs ISOBMFF walk; rare — full decode decides)
        _ => false,
    }
}

fn detect_orientation(format: ImageFormat, file: &File) -> Option<u32> {
    match format {
        ImageFormat::Jpeg | ImageFormat::Tiff => {
            let mut br = BufReader::new(file);
            let _ = br.seek(SeekFrom::Start(0));
            let exif = exif::Reader::new().read_from_container(&mut br).ok()?;
            let field = exif.get_field(exif::Tag::Orientation, exif::In::PRIMARY)?;
            field.value.get_uint(0)
        }
        // PNG eXIf etc. — left to the full decode path for now
        _ => None,
    }
}
