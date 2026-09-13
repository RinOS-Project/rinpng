RinOS in-tree PNG decoder (`libs/png`).

Implemented APIs:

- `rpng_get_info`
- `rpng_decode_rgba`

Features:

- Chunk/CRC validation (`IHDR`, `PLTE`, `tRNS`, `IDAT`, `IEND`)
- zlib inflate (`rinz`)
- PNG filters 0..4
- Color types 0/2/3/4/6
- Bit depths 1/2/4/8/16 (where valid for the color type)
- Adam7 interlace
