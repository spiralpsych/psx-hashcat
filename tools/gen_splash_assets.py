import argparse
import struct
from pathlib import Path

from PIL import Image

LOGO_SIZE = (240, 231)
LOGO_PADDED_WIDTH = 256
LOGO_TEXTURE_POS = (320, 0)
LOGO_CLUT_POS = (960, 480)

TITLE_SIZE = (300, 77)
TITLE_PADDED_WIDTH = 320
TITLE_PADDED_HEIGHT = 78
TITLE_TEXTURE_POS = (384, 0)
TITLE_CLUT_POS = (960, 481)

# A 32-entry CLUT is intentional even though 4bpp textures use 16 entries
# PSn00bSDK 0.24 uploads in 16-word DMA blocks; 32 halfwords are exactly one
# block, while a normal 16-entry CLUT would be only half a block
CLUT_ENTRIES = 32


def rgb555(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    value = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
    return value if value else 1


def is_transparent(pixel: tuple[int, int, int, int]) -> bool:
    return pixel[3] < 128


def pack_4bpp(indices: list[int], width: int, height: int) -> list[int]:
    if width % 4:
        raise ValueError("4bpp texture width must be divisible by four")
    if len(indices) != width * height:
        raise ValueError("pixel index count does not match the padded dimensions")

    words: list[int] = []
    for y in range(height):
        row = y * width
        for x in range(0, width, 4):
            a, b, c, d = indices[row + x : row + x + 4]
            words.append(a | (b << 4) | (c << 8) | (d << 12))
    return words


def nearest_palette_index(
    rgb: tuple[int, int, int], palette: list[tuple[int, int, int]]
) -> int:
    r, g, b = rgb
    best = 0
    best_error = 1 << 60
    for i, (pr, pg, pb) in enumerate(palette):
        dr, dg, db = r - pr, g - pg, b - pb
        error = dr * dr * 3 + dg * dg * 4 + db * db * 2
        if error < best_error:
            best_error = error
            best = i
    return best


def make_logo(path: Path) -> tuple[list[int], list[int], Image.Image]:
    source = Image.open(path).convert("RGBA")
    image = source.resize(LOGO_SIZE, Image.Resampling.LANCZOS)

    indices: list[int] = []
    preview = Image.new("RGBA", LOGO_SIZE, (0, 0, 0, 0))
    for y in range(LOGO_SIZE[1]):
        for x in range(LOGO_SIZE[0]):
            pixel = image.getpixel((x, y))
            if is_transparent(pixel):
                idx = 0
                color = (0, 0, 0, 0)
            else:
                r, g, b, _ = pixel
                lum = (r * 77 + g * 150 + b * 29) >> 8
                idx = max(1, min(15, (lum * 15 + 127) // 255))
                v = (idx * 255) // 15
                color = (v, v, v, 255)
            indices.append(idx)
            preview.putpixel((x, y), color)
        indices.extend([0] * (LOGO_PADDED_WIDTH - LOGO_SIZE[0]))

    palette16 = [0] + [rgb555(((i * 255) // 15,) * 3) for i in range(1, 16)]
    palette = palette16 + [0] * (CLUT_ENTRIES - len(palette16))
    return pack_4bpp(indices, LOGO_PADDED_WIDTH, LOGO_SIZE[1]), palette, preview


def make_title(path: Path) -> tuple[list[int], list[int], Image.Image]:
    source = Image.open(path).convert("RGBA")
    image = source.resize(TITLE_SIZE, Image.Resampling.LANCZOS)

    pixels = (
        image.get_flattened_data()
        if hasattr(image, "get_flattened_data")
        else image.getdata()
    )
    nontransparent = [
        pixel[:3] for pixel in pixels if not is_transparent(pixel)
    ]
    if not nontransparent:
        raise ValueError(f"{path} contains no visible pixels")

    sample = Image.new("RGB", (len(nontransparent), 1))
    sample.putdata(nontransparent)
    quantized = sample.quantize(
        colors=15,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    raw_palette = quantized.getpalette()[: 15 * 3]
    colors = [
        tuple(raw_palette[i : i + 3])
        for i in range(0, len(raw_palette), 3)
    ]
    colors = sorted(colors, key=lambda c: (sum(c), c[0], c[1], c[2]))

    indices: list[int] = []
    preview = Image.new("RGBA", TITLE_SIZE, (0, 0, 0, 0))
    for y in range(TITLE_SIZE[1]):
        for x in range(TITLE_SIZE[0]):
            pixel = image.getpixel((x, y))
            if is_transparent(pixel):
                idx = 0
                preview.putpixel((x, y), (0, 0, 0, 0))
            else:
                idx = nearest_palette_index(pixel[:3], colors) + 1
                pr, pg, pb = colors[idx - 1]
                preview.putpixel((x, y), (pr, pg, pb, 255))
            indices.append(idx)
        indices.extend([0] * (TITLE_PADDED_WIDTH - TITLE_SIZE[0]))

    for _ in range(TITLE_PADDED_HEIGHT - TITLE_SIZE[1]):
        indices.extend([0] * TITLE_PADDED_WIDTH)

    palette16 = [0] + [rgb555(color) for color in colors]
    palette = palette16 + [0] * (CLUT_ENTRIES - len(palette16))
    return (
        pack_4bpp(indices, TITLE_PADDED_WIDTH, TITLE_PADDED_HEIGHT),
        palette,
        preview,
    )


def write_tim4(
    path: Path,
    pixel_words: list[int],
    palette: list[int],
    texture_pos: tuple[int, int],
    texture_width_pixels: int,
    texture_height: int,
    clut_pos: tuple[int, int],
) -> None:
    if len(palette) != CLUT_ENTRIES:
        raise ValueError(f"CLUT must contain exactly {CLUT_ENTRIES} entries")
    if texture_width_pixels % 4:
        raise ValueError("4bpp TIM width must be divisible by four")

    width_words = texture_width_pixels // 4
    expected_pixels = width_words * texture_height
    if len(pixel_words) != expected_pixels:
        raise ValueError("TIM pixel data length does not match its rectangle")

    # LoadImage2() consumes two 16-bit VRAM pixels per 32-bit DMA word
    # Every generated transfer must be an exact multiple of 16 DMA words
    for label, halfword_count in (
        ("CLUT", len(palette)),
        ("texture", len(pixel_words)),
    ):
        if halfword_count % 2:
            raise ValueError(f"{label} transfer contains an odd halfword count")
        if (halfword_count // 2) % 16:
            raise ValueError(f"{label} transfer is not a whole 16-word DMA block")

    clut_data = struct.pack(f"<{len(palette)}H", *palette)
    pixel_data = struct.pack(f"<{len(pixel_words)}H", *pixel_words)

    clut_block_length = 12 + len(clut_data)
    pixel_block_length = 12 + len(pixel_data)

    output = bytearray()
    output += struct.pack("<II", 0x10, 0x08)  # TIM magic, 4bpp + CLUT
    output += struct.pack(
        "<IHHHH",
        clut_block_length,
        clut_pos[0],
        clut_pos[1],
        len(palette),
        1,
    )
    output += clut_data
    output += struct.pack(
        "<IHHHH",
        pixel_block_length,
        texture_pos[0],
        texture_pos[1],
        width_words,
        texture_height,
    )
    output += pixel_data

    path.write_bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    assets_dir = args.assets_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    logo_words, logo_palette, logo_preview = make_logo(assets_dir / "logo.png")
    title_words, title_palette, title_preview = make_title(assets_dir / "title.png")

    write_tim4(
        output_dir / "splash_logo.tim",
        logo_words,
        logo_palette,
        LOGO_TEXTURE_POS,
        LOGO_PADDED_WIDTH,
        LOGO_SIZE[1],
        LOGO_CLUT_POS,
    )
    write_tim4(
        output_dir / "splash_title.tim",
        title_words,
        title_palette,
        TITLE_TEXTURE_POS,
        TITLE_PADDED_WIDTH,
        TITLE_PADDED_HEIGHT,
        TITLE_CLUT_POS,
    )

    logo_preview.save(output_dir / "logo_preview.png")
    title_preview.save(output_dir / "title_preview.png")

    canvas = Image.new("RGBA", (320, 240), (0, 0, 0, 255))
    canvas.alpha_composite(
        logo_preview,
        ((320 - LOGO_SIZE[0]) // 2, (240 - LOGO_SIZE[1]) // 2),
    )
    canvas.alpha_composite(
        title_preview,
        ((320 - TITLE_SIZE[0]) // 2, 82),
    )
    canvas.convert("RGB").save(output_dir / "splash_preview.png")

    print(
        f"logo TIM: {(output_dir / 'splash_logo.tim').stat().st_size} bytes; "
        f"title TIM: {(output_dir / 'splash_title.tim').stat().st_size} bytes"
    )


if __name__ == "__main__":
    main()
