#!/usr/bin/env python3

import sys
import struct


def read_psf(filename):
    with open(filename, "rb") as f:
        data = f.read()

    # -------------------------
    # PSF1
    # -------------------------
    if data[:2] == b"\x36\x04":
        mode = data[2]
        charsize = data[3]

        # PSF1 normally has 256 glyphs, or 512 with mode bit 0 set.
        num_glyphs = 512 if (mode & 0x01) else 256

        glyphs = []
        offset = 4

        for i in range(num_glyphs):
            glyph = data[offset + i * charsize:
                          offset + (i + 1) * charsize]
            glyphs.append(glyph)

        return glyphs, 8, charsize

    # -------------------------
    # PSF2
    # -------------------------
    if data[:4] == b"\x72\xb5\x4a\x86":
        (
            magic,
            version,
            headersize,
            flags,
            length,
            charsize,
            height,
            width,
        ) = struct.unpack_from("<8I", data, 0)

        if width != 8:
            raise ValueError(
                f"Unsupported PSF2 width: {width}. "
                "This script expects an 8-pixel-wide font."
            )

        glyphs = []
        offset = headersize

        for i in range(length):
            glyph = data[offset + i * charsize:
                          offset + (i + 1) * charsize]
            glyphs.append(glyph)

        return glyphs, width, height

    raise ValueError("Not a valid PSF1 or PSF2 font")


def convert_glyph(glyph, width, height):
    """
    Convert a PSF glyph into rows of bytes.

    PSF uses:
        bit 7 = leftmost pixel

    which matches the renderer:

        (bits >> (7 - x)) & 1

    Returns exactly `height` bytes.
    """

    rows = []

    bytes_per_row = (width + 7) // 8

    for y in range(height):
        row = glyph[y * bytes_per_row:
                    (y + 1) * bytes_per_row]

        # We only support 8-pixel-wide fonts.
        rows.append(row[0])

    return rows


def format_byte(value):
    return f"0b{value:08b}"


def write_header(filename, glyphs, width, height):
    with open(filename, "w") as f:
        f.write("#ifndef FONT8X16_H\n")
        f.write("#define FONT8X16_H\n\n")
        f.write("#include <stdint.h>\n\n")

        f.write(f"#define FONT8X16_WIDTH  {width}\n")
        f.write(f"#define FONT8X16_HEIGHT {height}\n\n")

        f.write(
            "static const unsigned char font8x16[128][16] = {\n"
        )

        for c in range(128):
            if c < len(glyphs):
                rows = convert_glyph(
                    glyphs[c],
                    width,
                    height
                )
            else:
                rows = [0] * height

            # Only output exactly 16 rows.
            rows = (rows + [0] * 16)[:16]

            if 32 <= c <= 126:
                char_repr = chr(c)
                if char_repr == "'":
                    char_repr = "\\'"
                elif char_repr == "\\":
                    char_repr = "\\\\"

                comment = f" // '{char_repr}'"
            else:
                comment = f" // 0x{c:02X}"

            f.write(f"    [{c}] = {{")

            f.write(
                ",".join(format_byte(row) for row in rows)
            )

            f.write(f"}},{comment}\n")

        f.write("};\n\n")
        f.write("#endif\n")


def main():
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} input.psf output.h"
        )
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2]

    try:
        glyphs, width, height = read_psf(input_file)

        if width != 8:
            raise ValueError(
                f"Font width is {width}, but this script "
                "requires width 8."
            )

        if height != 16:
            raise ValueError(
                f"Font height is {height}, but this script "
                f"requires height 16."
            )

        write_header(
            output_file,
            glyphs,
            width,
            height
        )

        print(
            f"Converted {input_file} -> {output_file}"
        )
        print(
            f"Font: {width}x{height}, "
            f"{len(glyphs)} glyphs"
        )

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
