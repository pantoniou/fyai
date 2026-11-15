# Images of the embedded documents

`data/commands.yaml` and `data/catalog.yaml` are large. A parse of them cost
about 22% and 28% of the instructions of a start, so the build parses them one
time and embeds the result.

## Build

`tools/fyai_mkimage` parses a document with the flags of fyai
(`src/core/fyai_yaml_flags.h`) and deduplication, and makes the image of the result
with `fy_generic_image_create()` of libfyaml. The image holds the arena with
the shared values kept shared, and its pointers are offsets from address zero.
It does not hold the index of the deduplication or any state of the process, so
the bytes are the same at each build.
`tests/cases/image_reproducible.sh` checks this.

The tool writes one C array for each document, named by its third argument,
and the length `NAME_LEN`. The array is writable and aligned to
`FY_GENERIC_IMAGE_ALIGN`, so it is in the data of the binary and not in the
constant data. Before it writes the array, the tool loads a copy of the image
and compares it with the parse. A build with an image that does not load, or
that differs, fails. The tool runs on the build host: a cross build embeds
empty images and parses the text at run time.

## Load

`fyai_image_load()` calls `fy_generic_image_load()`, which checks the header and
moves each pointer of the arena in place to the address that it has. The first
load changes the arena, and a second one finds every pointer moved and changes
nothing. Nothing is copied, and the kernel copies a page of the arena only when
the load writes it.

The header holds the format version of the image, `FY_GENERIC_IMAGE_VERSION`,
the sizes and the tag bits that the encoding of a generic depends on, and a
hash of the header. An image of another version, or a damaged header, is
refused and is not touched. The caller then parses the embedded text.
`FYAI_NO_IMAGE` in the environment forces this path.

The build needs a libfyaml that has the functions of the images.

## Cost

| document | parse | image |
|---|---|---|
| commands | 2.9 ms | 200 KB |
| catalogue | 5.0 ms | 340 KB |

A start of `fyai branch list` takes 3.4 ms less with the images, 49.6 ms against
53.1 ms.
