# Draws the vote arrow used next to the price: assets/arrow_up.png (40x46, white, anti-aliased).
from PIL import Image, ImageDraw

S = 8  # supersample
im = Image.new("RGBA", (40 * S, 46 * S))
pts = [(20, 0), (40, 22), (29, 22), (29, 46), (11, 46), (11, 22), (0, 22)]
ImageDraw.Draw(im).polygon([(x * S, y * S) for x, y in pts], fill=(255, 255, 255, 255))
im.resize((40, 46), Image.Resampling.LANCZOS).save("assets/arrow_up.png")
