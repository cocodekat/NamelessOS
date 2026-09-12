import sys
from pathlib import Path

BLOCK = 512

if len(sys.argv) != 3:
    print("Usage: python tar2f.py <storage.img> <output>")
    sys.exit(1)

image = Path(sys.argv[1])
output = Path(sys.argv[2])

data = image.read_bytes()
pos = 0

output.mkdir(parents=True, exist_ok=True)

while pos + BLOCK <= len(data):
    header = data[pos:pos + BLOCK]

    # Two consecutive empty blocks = end of archive
    if header == b'\0' * BLOCK:
        break

    # TAR filename: bytes 0..99
    name = header[0:100].split(b'\0', 1)[0].decode("utf-8", errors="replace")

    if not name:
        break

    # TAR size field: bytes 124..135
    size_raw = header[124:136].split(b'\0', 1)[0].strip()

    try:
        size = int(size_raw or b"0", 8)
    except ValueError:
        print(f"Invalid size for {name!r}: {size_raw!r}")
        break

    # TAR type flag: byte 156
    typeflag = header[156:157]

    print(f"{name} ({size} bytes)")

    file_path = output / name.lstrip("/")

    if typeflag in (b'5',) or name.endswith('/'):
        file_path.mkdir(parents=True, exist_ok=True)
    else:
        file_path.parent.mkdir(parents=True, exist_ok=True)

        file_data_start = pos + BLOCK
        file_data_end = file_data_start + size

        with open(file_path, "wb") as f:
            f.write(data[file_data_start:file_data_end])

    # TAR data is padded to 512-byte blocks
    data_blocks = (size + BLOCK - 1) // BLOCK
    pos += BLOCK + data_blocks * BLOCK

print(f"\nExtracted filesystem to: {output}")
