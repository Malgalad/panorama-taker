use oxipng::{BitDepth, ColorType, Deflater, FilterStrategy, Options, RawImage};
use std::panic::{catch_unwind, AssertUnwindSafe};

pub struct Encoder {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
    encoded: Vec<u8>,
}

#[unsafe(no_mangle)]
pub extern "C" fn pano_oxi_abi() -> u32 {
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn pano_oxi_create(width: u32, height: u32) -> *mut Encoder {
    catch_unwind(|| {
        let len = (width as usize)
            .checked_mul(height as usize)?
            .checked_mul(3)?;
        if len == 0 {
            return None;
        }
        let mut pixels = Vec::new();
        pixels.try_reserve_exact(len).ok()?;
        pixels.resize(len, 0);
        Some(Box::into_raw(Box::new(Encoder {
            width,
            height,
            pixels,
            encoded: Vec::new(),
        })))
    })
    .ok()
    .flatten()
    .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// `encoder` must be a live handle from create, not concurrently accessed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pano_oxi_pixels(encoder: *mut Encoder) -> *mut u8 {
    unsafe { (*encoder).pixels.as_mut_ptr() }
}

/// # Safety
/// A live, fully populated handle is required. Pixels are consumed, even on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pano_oxi_encode(encoder: *mut Encoder) -> bool {
    catch_unwind(AssertUnwindSafe(|| {
        let encoder = unsafe { &mut *encoder };
        let mut raw = RawImage::new(
            encoder.width,
            encoder.height,
            ColorType::RGB {
                transparent_color: None,
            },
            BitDepth::Eight,
            std::mem::take(&mut encoder.pixels),
        )
        .ok()?;
        raw.add_png_chunk(*b"sRGB", vec![0]);
        raw.add_png_chunk(*b"gAMA", 45455_u32.to_be_bytes().to_vec());
        let options = Options {
            filters: [FilterStrategy::MinSum].into_iter().collect(),
            deflater: Deflater::Libdeflater { compression: 3 },
            fast_evaluation: false,
            bit_depth_reduction: false,
            color_type_reduction: false,
            palette_reduction: false,
            grayscale_reduction: false,
            ..Options::default()
        };
        encoder.encoded = raw.create_optimized_png(&options).ok()?;
        Some(())
    }))
    .ok()
    .flatten()
    .is_some()
}

/// # Safety
/// Both pointers must be valid; the returned bytes live until destroy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pano_oxi_output(encoder: *const Encoder, length: *mut usize) -> *const u8 {
    unsafe {
        *length = (*encoder).encoded.len();
        (*encoder).encoded.as_ptr()
    }
}

/// # Safety
/// The handle must be null or a live create result, destroyed exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pano_oxi_destroy(encoder: *mut Encoder) {
    if !encoder.is_null() {
        drop(unsafe { Box::from_raw(encoder) });
    }
}
