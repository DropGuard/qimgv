//! Header probe tests against hand-crafted minimal format bytes. The parsers
//! we use (imagesize, kamadak-exif) don't validate chunk CRCs, so structural
//! correctness is enough — no real image fixtures needed.

use std::io::Write;

use qimgv_engine::meta::{probe, ImageFormat};

fn tmp_with(name: &str, bytes: &[u8]) -> (tempfile::TempDir, std::path::PathBuf) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join(name);
    let mut f = std::fs::File::create(&path).unwrap();
    f.write_all(bytes).unwrap();
    (dir, path)
}

fn png_chunk(tag: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut out = (data.len() as u32).to_be_bytes().to_vec();
    out.extend_from_slice(tag);
    out.extend_from_slice(data);
    out // no CRC — parsers don't validate
}

fn png_bytes(w: u32, h: u32, apng: bool) -> Vec<u8> {
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]); // 8-bit RGB

    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    out.extend_from_slice(&png_chunk(b"IHDR", &ihdr));
    if apng {
        out.extend_from_slice(&png_chunk(b"acTL", &[0, 0, 0, 1, 0, 0, 0, 0]));
    }
    out.extend_from_slice(&png_chunk(b"IDAT", &[0]));
    out.extend_from_slice(&png_chunk(b"IEND", &[]));
    out
}

#[test]
fn png_dimensions_and_apng_flag() {
    let (_d, p) = tmp_with("still.png", &png_bytes(64, 32, false));
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Png);
    assert_eq!((info.width, info.height), (64, 32));
    assert!(!info.animated);

    let (_d, p) = tmp_with("anim.png", &png_bytes(64, 32, true));
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Png);
    assert!(info.animated, "acTL chunk must mark APNG");
}

#[test]
fn gif_is_always_animated() {
    // GIF89a + logical screen descriptor: w=20 h=10 LE
    let bytes = b"GIF89a\x14\x00\x0a\x00\x00\x00\x00";
    let (_d, p) = tmp_with("a.gif", bytes);
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Gif);
    assert!(info.animated);
    assert_eq!((info.width, info.height), (20, 10));
}

fn webp_bytes(animated: bool) -> Vec<u8> {
    let mut payload = vec![if animated { 0x02 } else { 0x00 }, 0, 0, 0]; // flags + reserved
    payload.extend_from_slice(&[39, 0, 0, 0]); // canvas width - 1 (40)
    payload.extend_from_slice(&[19, 0, 0, 0]); // canvas height - 1 (20)
    let mut out = b"RIFF\x00\x00\x00\x00WEBP".to_vec();
    out.extend_from_slice(b"VP8X");
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(&payload);
    out
}

#[test]
fn webp_animation_bit() {
    let (_d, p) = tmp_with("still.webp", &webp_bytes(false));
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::WebP);
    assert!(!info.animated);

    let (_d, p) = tmp_with("anim.webp", &webp_bytes(true));
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::WebP);
    assert!(info.animated, "VP8X animation flag must be detected");
}

#[test]
fn avif_brand_decides_animation() {
    let mut still = b"\x00\x00\x00\x18ftypavif\x00\x00\x00\x00".to_vec();
    still.extend_from_slice(&[0u8; 8]);
    let (_d, p) = tmp_with("a.avif", &still);
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Avif);
    assert!(!info.animated);

    let mut anim = b"\x00\x00\x00\x18ftypavis\x00\x00\x00\x00".to_vec();
    anim.extend_from_slice(&[0u8; 8]);
    let (_d, p) = tmp_with("b.avif", &anim);
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Avif);
    assert!(info.animated);
}

/// FFD8 + APP1(Exif, orientation=6) + SOF0(32x16) + EOI.
fn jpeg_bytes(orientation: u16) -> Vec<u8> {
    let mut tiff = b"II\x2a\x00\x08\x00\x00\x00".to_vec(); // LE TIFF, IFD @8
    tiff.extend_from_slice(&1u16.to_le_bytes()); // one entry
    tiff.extend_from_slice(&0x0112u16.to_le_bytes()); // Orientation tag
    tiff.extend_from_slice(&3u16.to_le_bytes()); // SHORT
    tiff.extend_from_slice(&1u32.to_le_bytes()); // count
    tiff.extend_from_slice(&(orientation as u32).to_le_bytes());
    tiff.extend_from_slice(&0u32.to_le_bytes()); // next IFD

    let mut app1 = b"Exif\x00\x00".to_vec();
    app1.extend_from_slice(&tiff);

    let mut out = b"\xff\xd8".to_vec();
    out.extend_from_slice(b"\xff\xe1");
    out.extend_from_slice(&((app1.len() + 2) as u16).to_be_bytes());
    out.extend_from_slice(&app1);
    out.extend_from_slice(b"\xff\xc0\x00\x0b\x08"); // SOF0, len 11, 8-bit
    out.extend_from_slice(&16u16.to_be_bytes()); // height
    out.extend_from_slice(&32u16.to_be_bytes()); // width
    out.extend_from_slice(&[1, 0x11, 0]); // 1 component
    out.extend_from_slice(b"\xff\xd9");
    out
}

#[test]
fn jpeg_orientation_and_dimensions() {
    let (_d, p) = tmp_with("rot.jpg", &jpeg_bytes(6));
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Jpeg);
    assert_eq!(info.orientation, 6, "EXIF orientation must come from one open");
    assert_eq!((info.width, info.height), (32, 16));

    let (_d, p) = tmp_with("plain.jpg", &jpeg_bytes(1));
    let info = probe(&p).unwrap();
    assert_eq!(info.orientation, 1);
}

#[test]
fn bmp_magic_and_dims() {
    // BM(2) + fileSize(4) + reserved(4) + dataOffset(4) + BITMAPINFOHEADER:
    // biSize(4)=40, width(4)=10, height(4)=20
    let mut b = b"BM".to_vec();
    b.extend_from_slice(&[0u8; 12]);
    b.extend_from_slice(&40u32.to_le_bytes());
    b.extend_from_slice(&10u32.to_le_bytes());
    b.extend_from_slice(&20u32.to_le_bytes());
    b.extend_from_slice(&[1, 0, 24, 0]);
    let (_d, p) = tmp_with("a.bmp", &b);
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Bmp);
    assert_eq!((info.width, info.height), (10, 20));
}

#[test]
fn unknown_bytes_are_ok_not_error() {
    let (_d, p) = tmp_with("blob.bin", b"hello world, definitely not an image");
    let info = probe(&p).unwrap();
    assert_eq!(info.format, ImageFormat::Unknown);
    assert_eq!((info.width, info.height), (0, 0));
    assert_eq!(info.orientation, 1);
}

#[test]
fn missing_file_is_io_error() {
    let p = std::path::Path::new("/nonexistent/qimgv-meta-test.bin");
    assert!(matches!(probe(p), Err(qimgv_engine::EngineError::Io { .. })));
}
