"""Render the client icon master into the PNG sizes used by Qt and hicolor."""

from pathlib import Path

from PIL import Image, ImageDraw

SIZES = (16, 22, 24, 32, 48, 64, 128, 256)
MASTER_SCALE = 16


def draw(size):
    image = Image.new("RGBA", (64 * MASTER_SCALE, 64 * MASTER_SCALE))
    canvas = ImageDraw.Draw(image)

    def round_rect(box, radius, fill):
        canvas.rounded_rectangle(
            tuple(coordinate * MASTER_SCALE for coordinate in box),
            radius=radius * MASTER_SCALE,
            fill=fill,
        )

    def polygon(points, fill):
        canvas.polygon(
            [(x * MASTER_SCALE, y * MASTER_SCALE) for x, y in points],
            fill=fill,
        )

    round_rect((2, 2, 62, 62), 15, "#203e4d")
    round_rect((23, 11, 54, 35), 7, "#84bcba")
    polygon([(45, 32), (50, 32), (50, 41)], "#84bcba")
    round_rect((10, 25, 43, 49), 7, "#eef6eb")
    polygon([(16, 47), (26, 47), (16, 55)], "#eef6eb")
    round_rect((16, 33, 36, 36), 1.5, "#2f5962")
    round_rect((16, 39, 29, 42), 1.5, "#2f5962")

    return image.resize((size, size), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    destination = Path(__file__).parent
    for pixels in SIZES:
        draw(pixels).save(destination / f"{pixels}.png", optimize=True)
