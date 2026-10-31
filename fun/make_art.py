"""Draws the art for the milk miko joke (original, drawn in code): a chibi shrine maiden in three moods, her giant
milk-bottle cannon, and manga sound words. Writes PNGs into fun/anime/.

    python fun/make_art.py
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = Path(__file__).with_name("anime")
OUT.mkdir(exist_ok=True)
S = 4                       # supersampling
LINE = (58, 36, 48, 255)    # outline colour
SKIN, SKIN_SH = (255, 228, 212, 255), (246, 196, 182, 255)
HAIR, HAIR_HI = (30, 26, 40, 255), (70, 74, 112, 255)
WHITE, WHITE_SH = (252, 252, 255, 255), (214, 218, 236, 255)
RED, RED_SH = (214, 40, 52, 255), (162, 22, 40, 255)
BLUSH = (255, 120, 140, 110)


def P(pts):
    return [(x * S, y * S) for x, y in pts]


def ell(d, cx, cy, rx, ry, fill, outline=LINE, w=3):
    d.ellipse([(cx - rx) * S, (cy - ry) * S, (cx + rx) * S, (cy + ry) * S], fill=fill, outline=outline, width=w * S)


def poly(d, pts, fill, outline=LINE, w=3):
    d.polygon(P(pts), fill=fill)
    if outline:
        d.line(P(pts + [pts[0]]), fill=outline, width=w * S, joint="curve")


def curve(p0, p1, p2, n=24):   # quadratic bezier points
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0], (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1])
            for t in [i / n for i in range(n + 1)]]


BANG = [(104, 200), (110, 120), (160, 74), (256, 58), (352, 74), (402, 120), (408, 200), (380, 170), (360, 196), (340, 140),
        (318, 176), (300, 128), (272, 170), (256, 120), (240, 170), (212, 128), (194, 176), (172, 140), (152, 196), (132, 170)]


def girl(mood):
    W, H = 512, 640
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = 256
    # long hair behind
    poly(d, [(150, 230), (120, 330), (118, 470), (150, 520), (362, 520), (394, 470), (392, 330), (362, 230)], HAIR)
    # hakama (red, widening, pleats)
    poly(d, [(196, 400), (168, 600), (344, 600), (316, 400)], RED)
    poly(d, [(256, 410), (240, 600), (272, 600)], RED_SH, outline=None)
    for x0, x1 in [(222, 205), (290, 307)]:
        d.line(P([(x0, 420), (x1, 596)]), fill=RED_SH, width=3 * S)
    # tabi feet + zori
    for fx in (214, 298):
        ell(d, fx, 608, 26, 12, WHITE)
        d.rectangle([(fx - 28) * S, 616 * S, (fx + 28) * S, 624 * S], fill=(120, 70, 50, 255))
    # kosode (white top) with wide sleeves
    poly(d, [(200, 300), (176, 410), (336, 410), (312, 300)], WHITE)
    poly(d, [(200, 300), (122, 330), (108, 450), (176, 452), (196, 360)], WHITE)    # left sleeve
    poly(d, [(312, 300), (390, 330), (404, 450), (336, 452), (316, 360)], WHITE)    # right sleeve
    poly(d, [(122, 420), (108, 450), (176, 452), (178, 424)], WHITE_SH, outline=None)
    poly(d, [(390, 420), (404, 450), (336, 452), (334, 424)], WHITE_SH, outline=None)
    # red sleeve cords and the collar crossing (kosode V) + the hakama tie
    d.line(P(curve((124, 446), (140, 470), (170, 448))), fill=RED, width=4 * S)
    d.line(P(curve((388, 446), (372, 470), (342, 448))), fill=RED, width=4 * S)
    poly(d, [(236, 300), (256, 352), (276, 300), (266, 300), (256, 330), (246, 300)], RED, outline=None)
    d.line(P([(222, 300), (256, 370), (290, 300)]), fill=LINE, width=3 * S)
    d.rectangle([186 * S, 396 * S, 326 * S, 412 * S], fill=RED, outline=LINE, width=3 * S)
    poly(d, [(256, 404), (232, 440), (248, 444), (256, 418), (264, 444), (280, 440)], RED)   # bow
    # hands (little mitts out of the sleeves)
    ell(d, 150, 458, 18, 14, SKIN)
    ell(d, 362, 458, 18, 14, SKIN)
    # head
    ell(d, cx, 200, 150, 140, SKIN)
    # soft shade under the fringe and a blush, blended on their own layers (clipped to the face)
    face = Image.new("L", im.size, 0)
    ImageDraw.Draw(face).ellipse([(cx - 147) * S, 63 * S, (cx + 147) * S, 337 * S], fill=255)
    shade = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(shade).polygon(P([(x, y + 16) for x, y in BANG]), fill=SKIN_SH)
    shade.putalpha(Image.composite(shade.getchannel("A"), Image.new("L", im.size, 0), face).filter(ImageFilter.GaussianBlur(3 * S)))
    im.alpha_composite(shade)
    blush = Image.new("RGBA", im.size, (0, 0, 0, 0))
    bd = ImageDraw.Draw(blush)
    for bx in (176, 336):
        bd.ellipse([(bx - 28) * S, 230 * S, (bx + 28) * S, 252 * S], fill=BLUSH)
    im.alpha_composite(blush.filter(ImageFilter.GaussianBlur(2 * S)))
    d = ImageDraw.Draw(im)
    # eyes
    if mood == "happy":     # ^ ^
        for ex in (196, 316):
            d.line(P(curve((ex - 30, 214), (ex, 176), (ex + 30, 214))), fill=LINE, width=7 * S)
    elif mood == "hurt":    # > <
        d.line(P([(170, 182), (214, 202), (170, 222)]), fill=LINE, width=7 * S, joint="curve")
        d.line(P([(342, 182), (298, 202), (342, 222)]), fill=LINE, width=7 * S, joint="curve")
    else:                   # big shiny anime eyes
        for ex in (196, 316):
            ell(d, ex, 204, 30, 38, (255, 255, 255, 255), w=4)
            ell(d, ex, 210, 24, 32, (122, 34, 70, 255), outline=None)
            ell(d, ex, 218, 16, 20, (190, 60, 110, 255), outline=None)
            ell(d, ex, 206, 12, 14, (40, 10, 30, 255), outline=None)
            ell(d, ex - 9, 192, 9, 10, (255, 255, 255, 255), outline=None)
            ell(d, ex + 10, 222, 4, 5, (255, 255, 255, 255), outline=None)
            d.line(P(curve((ex - 34, 168), (ex, 156), (ex + 34, 170))), fill=LINE, width=6 * S)   # lashes
    # mouth
    if mood == "happy":
        d.chord([236 * S, 246 * S, 276 * S, 282 * S], 0, 180, fill=(190, 50, 70, 255), outline=LINE, width=3 * S)
        d.chord([246 * S, 262 * S, 266 * S, 280 * S], 0, 180, fill=(255, 130, 150, 255))
    elif mood == "hurt":
        d.line(P([(236, 262), (246, 256), (256, 264), (266, 256), (276, 262)]), fill=LINE, width=4 * S, joint="curve")
        poly(d, [(372, 140), (362, 170), (372, 180), (382, 170)], (150, 210, 255, 255), outline=(60, 110, 200, 255), w=2)   # sweat drop
    else:
        d.line(P(curve((240, 258), (248, 268), (256, 258)) + curve((256, 258), (264, 268), (272, 258))), fill=LINE, width=4 * S)   # :3
    # bangs over the forehead
    poly(d, BANG, HAIR)
    d.line(P(curve((150, 96), (256, 66), (362, 96))), fill=HAIR_HI, width=10 * S)     # angel ring shine
    # side locks
    poly(d, [(110, 190), (98, 300), (126, 330), (138, 230)], HAIR)
    poly(d, [(402, 190), (414, 300), (386, 330), (374, 230)], HAIR)
    # red ribbon (bow) on top
    poly(d, [(256, 62), (196, 30), (204, 86)], RED)
    poly(d, [(256, 62), (316, 30), (308, 86)], RED)
    ell(d, 256, 62, 14, 12, RED_SH)
    img = im.resize((W, H), Image.LANCZOS)
    img.save(OUT / f"miko_{mood}.png")


def cannon():
    W, H = 512, 224
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # a big glass milk bottle on its side, neck pointing right (+x): body, shoulder, neck, blue cap
    body = [(24, 52), (300, 52), (360, 78), (420, 92), (420, 132), (360, 146), (300, 172), (24, 172)]
    poly(d, body, (246, 248, 255, 255), w=4)
    poly(d, [(24, 140), (300, 140), (360, 146), (300, 172), (24, 172)], (214, 222, 240, 255), outline=None)
    poly(d, body, None, w=4)
    d.rounded_rectangle([420 * S, 84 * S, 470 * S, 140 * S], radius=10 * S, fill=(70, 140, 230, 255), outline=LINE, width=4 * S)
    d.line(P([(434, 84), (434, 140)]), fill=(150, 200, 255, 255), width=4 * S)
    # glass shine
    d.line(P([(48, 70), (280, 70)]), fill=(255, 255, 255, 255), width=8 * S)
    # label with cow spots and the word
    d.rounded_rectangle([90 * S, 76 * S, 270 * S, 150 * S], radius=12 * S, fill=(70, 140, 230, 255), outline=LINE, width=3 * S)
    for sx, sy, r in [(62, 120, 14), (284, 84, 10), (300, 150, 9), (44, 70, 8)]:
        ell(d, sx, sy, r * 1.3, r, (40, 40, 52, 255), outline=None)
    try:
        f = ImageFont.truetype("C:/Windows/Fonts/YuGothB.ttc", 46 * S)
    except OSError:
        f = ImageFont.truetype("C:/Windows/Fonts/meiryob.ttc", 46 * S)
    d.text((180 * S, 113 * S), "ミルク", font=f, fill=(255, 255, 255, 255), anchor="mm", stroke_width=3 * S, stroke_fill=LINE)
    img = im.resize((W, H), Image.LANCZOS)
    img.save(OUT / "milk_cannon.png")


def words():
    try:
        f = ImageFont.truetype("C:/Windows/Fonts/YuGothB.ttc", 150)
    except OSError:
        f = ImageFont.truetype("C:/Windows/Fonts/meiryob.ttc", 150)
    items = {"don": ("ドーン！", (255, 230, 60)), "basha": ("バシャ！", (120, 200, 255)), "pyu": ("ピュー！", (255, 255, 255)),
             "kira": ("キラッ！", (255, 200, 240)), "yatta": ("やったぁ！", (255, 170, 60)), "ita": ("いたっ！", (255, 90, 90)),
             "sayonara": ("さよなら〜", (200, 160, 255))}
    for key, (text, col) in items.items():
        im = Image.new("RGBA", (900, 260), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.text((452, 136), text, font=f, fill=(0, 0, 0, 255), anchor="mm", stroke_width=22, stroke_fill=(0, 0, 0, 255))   # thick manga outline
        d.text((450, 130), text, font=f, fill=col + (255,), anchor="mm", stroke_width=9, stroke_fill=(255, 255, 255, 255))
        bb = im.getbbox()
        im.crop((max(bb[0] - 8, 0), max(bb[1] - 8, 0), bb[2] + 8, bb[3] + 8)).save(OUT / f"word_{key}.png")
    # speed lines (radial burst) for big moments
    N = 1024
    im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for k in range(72):
        a = k / 72 * 2 * math.pi + (k % 3) * 0.01
        r0 = N * (0.20 + 0.06 * ((k * 37) % 5) / 5)
        w = 0.010 + 0.008 * ((k * 13) % 4) / 4
        p = [(N / 2 + math.cos(a - w) * r0, N / 2 + math.sin(a - w) * r0), (N / 2 + math.cos(a) * N * 0.5, N / 2 + math.sin(a) * N * 0.5),
             (N / 2 + math.cos(a + w) * r0, N / 2 + math.sin(a + w) * r0)]
        d.polygon(p, fill=(255, 255, 255, 230))
    im.filter(ImageFilter.GaussianBlur(1.5)).save(OUT / "speedlines.png")
    # a 4-point sparkle
    im = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.polygon([(64, 0), (74, 54), (128, 64), (74, 74), (64, 128), (54, 74), (0, 64), (54, 54)], fill=(255, 255, 255, 255))
    im.filter(ImageFilter.GaussianBlur(1.2)).save(OUT / "sparkle.png")


for m in ("normal", "happy", "hurt"):
    girl(m)
cannon()
words()
print("wrote", sorted(p.name for p in OUT.iterdir()))
