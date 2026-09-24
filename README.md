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

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinPNG implements a bounded static PNG decoder used through RinImage and its C interface. |
| Supported API | The public C interface is `rpng.h`. Supported images are static PNG files using the documented decoder profile. |
| Unsupported API | APNG animation and PNG features outside the documented decoder profile are unsupported; use RinImage for the normal application integration. |
| ownership | The caller owns input bytes and the destination pixel buffer and keeps them valid for the call. The decoder does not retain them. |
| thread-safety | Independent calls using separate buffers may run concurrently. Do not share writable output buffers across calls. |
| limits | Input is limited to 64 MiB, each dimension to 4096 pixels, and inflated raw image data to 256 MiB. Limit violations fail decoding. |
| errors | Malformed, unsupported, truncated, or over-limit data returns failure. Output is valid only after successful completion. |
| ABI stability | `rpng.h` is the public C ABI. No cross-version ABI stability guarantee is published; consumers should rebuild when updating RinPNG. |
| security | Treat PNG bytes as untrusted. The decoder has input, dimension, and inflated-size caps; callers should still check status and avoid unbounded follow-on processing. |
| build | No standalone build/test entry point is documented. RinImage is the supported integration point in the RinOS build. |
| test | No standalone test command is documented for this submodule. Validate through RinImage and the consuming RinOS targets. |
