"""
One-shot generator for the canonical Symbion icon set (Variant B disc design).

Writes to:
  C:\\Users\\symbi\\Downloads\\files\\         -- source kit (canonical)
  C:\\Users\\symbi\\symbion\\electron\\assets\\ -- repo (used by the build)

Pipeline:
  - Render once at 2048 px (super-sample), downscale via LANCZOS to each
    target. Eliminates the chunky aliasing you'd get drawing at 16 px directly.
  - PNG series: 16, 32, 48, 64, 128, 256, 512, 1024
  - .ico bundle: 16/24/32/48/64/128/256
  - .icns bundle: 16/32/64/128/256/512/1024
  - SVG: hand-written, same design, scales perfectly.

Throwaway helper -- not part of the runtime install flow.
"""
from pathlib import Path
import shutil
from PIL import Image, ImageDraw

KIT  = Path(r"C:\Users\symbi\Downloads\files")
REPO = Path(r"C:\Users\symbi\symbion\electron\assets")
KIT.mkdir(parents=True, exist_ok=True)
REPO.mkdir(parents=True, exist_ok=True)

EDGE_FRAC  = 22 / 512   # edge stroke as fraction of icon diameter (Variant B)
MARK_SCALE = 0.74

def render_master(diameter: int) -> Image.Image:
    img = Image.new("RGBA", (diameter, diameter), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = diameter / 2
    edge_px = max(2, round(diameter * EDGE_FRAC))

    r_disc = diameter / 2 - 1
    d.ellipse((cx - r_disc, cy - r_disc, cx + r_disc, cy + r_disc),
              fill=(0, 0, 0, 255))

    r_ring = r_disc - edge_px / 2
    d.ellipse((cx - r_ring, cy - r_ring, cx + r_ring, cy + r_ring),
              outline=(255, 255, 255, 255), width=edge_px)

    s = diameter / 100.0
    def U(x): return cx + (x - 50) * MARK_SCALE * s

    d.polygon([
        (U(18), U(18)), (U(78), U(18)), (U(78), U(33)),
        (U(35), U(33)), (U(35), U(41)), (U(18), U(41)),
    ], fill=(255, 255, 255, 255))
    d.polygon([
        (U(82), U(82)), (U(22), U(82)), (U(22), U(67)),
        (U(65), U(67)), (U(65), U(59)), (U(82), U(59)),
    ], fill=(255, 255, 255, 255))

    rdot = 4 * MARK_SCALE * s
    for ux, uy in [(30, 50), (70, 50)]:
        d.ellipse((U(ux) - rdot, U(uy) - rdot, U(ux) + rdot, U(uy) + rdot),
                  fill=(255, 255, 255, 255))
    rring_out = 6.5 * MARK_SCALE * s
    sw = max(1, round(3.5 * MARK_SCALE * s))
    d.ellipse((U(50) - rring_out, U(50) - rring_out,
               U(50) + rring_out, U(50) + rring_out),
              outline=(255, 255, 255, 255), width=sw)
    return img


master = render_master(2048)

PNG_SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]
pngs = {}
for sz in PNG_SIZES:
    img = master.resize((sz, sz), Image.LANCZOS)
    pngs[sz] = img
    img.save(KIT / f"symbion-white-{sz}.png")
print(f"Wrote {len(PNG_SIZES)} PNGs to kit ({', '.join(str(s) for s in PNG_SIZES)})")

ico_path = REPO / "symbion.ico"
pngs[256].save(ico_path, format="ICO",
               sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
shutil.copy(ico_path, KIT / "symbion.ico")
print(f".ico:    {ico_path}  ({ico_path.stat().st_size} bytes)")

icns_path = REPO / "symbion.icns"
pngs[1024].save(icns_path, format="ICNS",
                sizes=[(16,16),(32,32),(64,64),(128,128),(256,256),(512,512),(1024,1024)])
shutil.copy(icns_path, KIT / "symbion.icns")
print(f".icns:   {icns_path}  ({icns_path.stat().st_size} bytes)")

shutil.copy(KIT / "symbion-white-512.png", REPO / "symbion-512.png")

svg = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="512" height="512">
  <title>Symbion</title>
  <desc>Symbion logo mark: white split-S on a black disc with a white edge ring.</desc>
  <circle cx="50" cy="50" r="49.8" fill="#000000"/>
  <circle cx="50" cy="50" r="47.85" fill="none" stroke="#FFFFFF" stroke-width="4.3"/>
  <g transform="translate(50 50) scale(0.74) translate(-50 -50)">
    <path d="M 18 18 L 78 18 L 78 33 L 35 33 L 35 41 L 18 41 Z" fill="#FFFFFF"/>
    <path d="M 82 82 L 22 82 L 22 67 L 65 67 L 65 59 L 82 59 Z" fill="#FFFFFF"/>
    <circle cx="30" cy="50" r="4" fill="#FFFFFF"/>
    <circle cx="50" cy="50" r="6.5" fill="none" stroke="#FFFFFF" stroke-width="3.5"/>
    <circle cx="70" cy="50" r="4" fill="#FFFFFF"/>
  </g>
</svg>
"""
(REPO / "symbion-mark.svg").write_text(svg, encoding="utf-8")
(KIT  / "symbion-mark.svg").write_text(svg, encoding="utf-8")
(KIT  / "symbion-mark-white.svg").write_text(svg, encoding="utf-8")
print(f"SVG written to kit + repo")
