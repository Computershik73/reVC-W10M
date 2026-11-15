"""Generate the package's tile and splash images from reVC's own logo (res/images/logo_1024.png).

    python w10m/tools/make_assets.py [--out w10m/app/Assets]

Needs Pillow. The images are in the repository, so this is only for changing them. The package
has no resources.pri with scale variants, so each image is drawn at 4x its nominal size (2x for
the splash screen): the phone scales it down to fit, and it stays sharp on a 350% display.
"""
import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

LOGO = os.path.join(env.ROOT, "res", "images", "logo_1024.png")
BACKGROUND = (0x14, 0x09, 0x1C, 255)  # the files screen's night purple (app/GameFiles.h)

# name -> (nominal width, height, the logo's size as a fraction of the shorter side)
SIZES = {
    "Square44x44Logo.png": (44, 44, 0.96),
    "Square71x71Logo.png": (71, 71, 0.86),
    "Square150x150Logo.png": (150, 150, 0.78),
    "Square310x310Logo.png": (310, 310, 0.72),
    "Wide310x150Logo.png": (310, 150, 0.9),
    "StoreLogo.png": (50, 50, 0.96),
    "SplashScreen.png": (620, 300, 0.8),
}


def render(logo, w, h, fraction, oversample):
    w, h = w * oversample, h * oversample
    canvas = Image.new("RGBA", (w, h), BACKGROUND)
    side = int(min(w, h) * fraction)
    scaled = logo.resize((side, side), Image.LANCZOS)
    canvas.alpha_composite(scaled, ((w - side) // 2, (h - side) // 2))
    return canvas.convert("RGB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(env.W10M, "app", "Assets"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logo = Image.open(LOGO).convert("RGBA")
    # the drawing does not fill its square: crop to what is drawn, then pad back to a square
    box = logo.getbbox()
    if box:
        logo = logo.crop(box)
    side = max(logo.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.alpha_composite(logo, ((side - logo.width) // 2, (side - logo.height) // 2))
    for name, (w, h, f) in SIZES.items():
        render(square, w, h, f, 2 if name == "SplashScreen.png" else 4).save(os.path.join(a.out, name), optimize=True)
    print(f"{len(SIZES)} images -> {a.out}")


if __name__ == "__main__":
    main()
