"""Renders the Try2Create logo: a vivid tile (violet into magenta into amber), a white rounded "play" mark - pictures that move, videos,
things made - and a gold creation sparkle on its corner. Drawn on a 1024 grid at SS x supersampling and scaled down; small sizes drop detail.

    python make_logo.py <out dir>      (Pillow)   ->  logo_<size>.png, logo_1024.png, app.ico, preview.png
"""
import math, sys
from PIL import Image, ImageDraw, ImageFilter

OUT = sys.argv[1] if len(sys.argv) > 1 else '.'
G = 1024  # design grid

def hexc(h, a=255):
    h = h.lstrip('#')
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(4))

def diag_grad(w, h, stops):
    """A gradient from the top-left corner to the bottom-right one through `stops` (colour list)."""
    small = 256
    g = Image.new('RGBA', (small, small))
    px = g.load()
    n = len(stops) - 1
    for y in range(small):
        for x in range(small):
            t = (x + y) / (2 * (small - 1))
            k = min(n - 1, int(t * n))
            px[x, y] = lerp(stops[k], stops[k + 1], t * n - k)
    return g.resize((w, h), Image.BICUBIC)

def rr_mask(size, box, r):
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, r, fill=255)
    return m

def sparkle(img, s, cx, cy, r, col_a, col_b):
    """A four-point star with curved (concave) sides, gold gradient."""
    m = Image.new('L', img.size, 0)
    pts = []
    for i in range(360):
        t = i / 360 * 2 * math.pi
        x = r * (math.cos(t) ** 3)
        y = r * (math.sin(t) ** 3)
        k = 0.78  # fattened so it reads at small sizes
        x = x * k + r * (1 - k) * math.cos(t) * 0.55
        y = y * k + r * (1 - k) * math.sin(t) * 0.55
        pts.append(((cx + x) * s, (cy + y) * s))
    ImageDraw.Draw(m).polygon(pts, fill=255)
    g = Image.new('RGBA', img.size)
    gd = ImageDraw.Draw(g)
    for yy in range(img.size[1]):
        gd.line([(0, yy), (img.size[0], yy)], fill=lerp(col_a, col_b, yy / max(1, img.size[1] - 1)))
    img.paste(g, (0, 0), m)

def rounded_triangle(size, pts, r):
    """A filled triangle with round corners: the triangle shrunk by r, then grown back by r with round joins."""
    m = Image.new('L', size, 0)
    d = ImageDraw.Draw(m)
    cx = sum(p[0] for p in pts) / 3
    cy = sum(p[1] for p in pts) / 3
    inner = []
    for (x, y) in pts:  # pull each corner toward the centre so the rounded result keeps the size
        dx, dy = x - cx, y - cy
        ln = math.hypot(dx, dy)
        inner.append((x - dx / ln * r * 2.0, y - dy / ln * r * 2.0))
    d.polygon(inner, fill=255)
    d.line(inner + [inner[0]], fill=255, width=int(r * 2), joint='curve')
    for (x, y) in inner:
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    return m

def render(px, detail):
    """detail 3 = full, 2 = no second sparkle, 1 = 16-24 px (bold, minimal)."""
    SS = max(2, 2048 // px) if px < 256 else 4
    W = px * SS
    s = W / G
    img = Image.new('RGBA', (W, W), (0, 0, 0, 0))

    # --- the tile: violet into magenta into amber
    pad = 40 if detail >= 2 else 8
    rad = 230 if detail >= 2 else 200
    tile = (pad * s, pad * s, (G - pad) * s, (G - pad) * s)
    img.paste(diag_grad(W, W, [hexc('#6D28D9'), hexc('#DB2777'), hexc('#F59E0B')]), (0, 0), rr_mask((W, W), tile, rad * s))
    if detail >= 2:  # a soft light from the top and a thin rim
        glow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse((150 * s, -380 * s, 870 * s, 380 * s), fill=(255, 255, 255, 46))
        glow = glow.filter(ImageFilter.GaussianBlur(70 * s))
        tile_m = rr_mask((W, W), tile, rad * s)
        clipped = Image.new('RGBA', (W, W), (0, 0, 0, 0))
        clipped.paste(glow, (0, 0), tile_m)
        img.alpha_composite(clipped)
        rim = Image.new('RGBA', (W, W), (0, 0, 0, 0))
        ImageDraw.Draw(rim).rounded_rectangle(tile, rad * s, outline=(255, 255, 255, 40), width=max(1, int(10 * s)))
        img.alpha_composite(rim)

    # --- the play mark: a rounded triangle, a little left of centre (it balances the sparkle)
    if detail >= 2:
        tri = [(330, 250), (330, 774), (780, 512)]
        corner = 64
    else:
        tri = [(280, 200), (280, 824), (840, 512)]
        corner = 70
    tri_s = [(x * s, y * s) for x, y in tri]
    sh = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    sh.paste((40, 0, 40, 110), (0, 0), rounded_triangle((W, W), [(x + 8 * s, y + 24 * s) for x, y in tri_s], corner * s))
    sh = sh.filter(ImageFilter.GaussianBlur(26 * s))
    img.alpha_composite(sh)
    white = Image.new('RGBA', (W, W), (255, 255, 255, 255))
    img.paste(white, (0, 0), rounded_triangle((W, W), tri_s, corner * s))

    # --- the creation sparkle on the top-right (with a halo of the tile's violet so it separates)
    if detail >= 2:
        spx, spy, spr = 770, 250, 150
    else:
        spx, spy, spr = 790, 230, 200
    halo = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    sparkle(halo, s, spx, spy, spr + (30 if detail >= 2 else 40), hexc('#5B21B6'), hexc('#7C1D6F'))
    img.alpha_composite(halo)
    sparkle(img, s, spx, spy, spr, hexc('#FFF3B0'), hexc('#FFB020'))
    if detail >= 3:
        sparkle(img, s, 880, 430, 54, hexc('#FFF3B0'), hexc('#FFB020'))

    return img.resize((px, px), Image.LANCZOS)

if __name__ == '__main__':
    import os
    os.makedirs(OUT, exist_ok=True)
    sizes = {256: 3, 128: 3, 64: 2, 48: 2, 40: 2, 32: 2, 24: 1, 20: 1, 16: 1}
    imgs = {}
    for p, det in sizes.items():
        imgs[p] = render(p, det)
        imgs[p].save(os.path.join(OUT, f'logo_{p}.png'))
    render(1024, 3).save(os.path.join(OUT, 'logo_1024.png'))
    imgs[256].save(os.path.join(OUT, 'app.ico'), sizes=[(p, p) for p in sorted(sizes)], append_images=[imgs[p] for p in sorted(sizes) if p != 256])
    # a preview sheet on the app's dark background and on a light one
    sheet = Image.new('RGBA', (1180, 560), hexc('#14161A'))
    x = 20
    for p in (256, 128, 64, 48, 32, 24, 16):
        sheet.alpha_composite(imgs[p], (x, 20))
        x += p + 24
    light = Image.new('RGBA', (1180, 280), hexc('#F0F2F6'))
    x = 20
    for p in (256, 128, 64, 48, 32, 24, 16):
        light.alpha_composite(imgs[p], (x, 12))
        x += p + 24
    sheet.alpha_composite(light, (0, 280))
    z = 0
    for p in (16, 24, 32):
        big = imgs[p].resize((p * 4, p * 4), Image.NEAREST)
        sheet.alpha_composite(big, (720 + z, 140))
        z += p * 4 + 16
    sheet.save(os.path.join(OUT, 'preview.png'))
    print('ok')
