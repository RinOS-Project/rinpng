RinPNG is RinOS's bounded, in-tree PNG decoder. Its supported subset is
explicit; the API does not claim complete PNG/APNG compatibility.

## Supported API and profile

The public C API in `rpng.h` consists of `rpng_get_info` and
`rpng_decode_rgba`. It accepts static PNG using color types 0, 2, 3, 4, and 6,
with the bit depths permitted for each type; it handles Adam7, filters 0..4,
palette data, `tRNS`, and CRC validation. The output is stored in 32-bit
caller-owned pixels. `rin_image_decode_png` is the preferred RinOS entry point
when a caller needs the common ARGB8888 contract and configurable limits.

APNG animation is not implemented. The decoder does not expose ancillary PNG
metadata as a color-management contract. A format identifier or valid IHDR
alone does not mean that an unsupported PNG feature is decoded.

## Ownership, limits, and errors

Input and output buffers are caller-owned. `rpng_decode_rgba` has no output
capacity parameter, so the caller must allocate at least `width * height`
32-bit pixels using checked arithmetic before calling it. The decoder owns
temporary palette, IDAT, and inflated buffers only for the duration of the
call; independent calls use independent parser state.

Codec-local limits are 64 MiB encoded input, 4096 pixels per dimension, and
256 MiB inflated raw data. The common RinImage defaults further cap canonical
output at 64 MiB. Return values distinguish parameter, format, unsupported,
allocation, CRC, decompression, and limit failures through `RPNG_ERR_*`.

Treat input as untrusted and apply caller-specific limits before decoding.
The decoder has no cancellation/deadline API; its raw-data limit is not a CPU
budget. The `rpng.h` C interface is source-level and does not publish a
separately versioned binary ABI. RinOS integrates the codec through RinImage;
this repository has no standalone build or test target.
