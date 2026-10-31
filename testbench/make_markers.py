"""Writes one marker effect per Mark("...") beat and per TourMark("...") chapter in bench_script.lua.

Each marker is a sprite effect whose image is named fortsgfx_capture_<label>.png (a bench beat: FortsGFX
saves a capture labelled <label>) or fortsgfx_tour_<NN_name>.png (the showcase: FortsGFX shows tour
chapter NN). The image is almost fully transparent, so without FortsGFX nothing shows; FortsGFX
recognises the name and hides the sprite. Run again after adding or renaming beats: python make_markers.py
"""
import re
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "markers" / "effects"
MAP_DIR = "maps/fortsgfx testbench/effects/media/"   # data-root path: the same from any script or map


def png(w, h, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgba) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


EFFECT = """-- FortsGFX test bench marker "{name}": an invisible sprite FortsGFX answers with {what}.
LifeSpan = 0.6

Sprites =
{{
    {{
        Name = "{name}",
        States =
        {{
            Normal =
            {{
                Frames =
                {{
                    {{ texture = "{media}{name}.png" }},
                    duration = 1,
                }},
                NextState = "Normal",
            }},
        }},
    }},
}}

Effects =
{{
    {{
        Type = "sprite",
        TimeToTrigger = 0,
        LocalPosition = {{ x = 0, y = 0, z = 0 }},
        LocalVelocity = {{ x = 0, y = 0, z = 0 }},
        Acceleration = {{ x = 0, y = 0, z = 0 }},
        Drag = 0.0,
        Sprite = "{name}",
        Additive = false,
        TimeToLive = 0.5,
        Angle = 0,
        InitialSize = 2,
        ExpansionRate = 0,
        AngularVelocity = 0,
        RandomAngularVelocityMagnitude = 0,
        Colour1 = {{ 255, 255, 255, 255 }},
        Colour2 = {{ 255, 255, 255, 255 }},
    }},
}}
"""


def main():
    script = (HERE / "bench_script.lua").read_text(encoding="utf-8")
    kinds = [
        ("fortsgfx_capture_", "a capture", re.findall(r'(?<![A-Za-z_])Mark\("([A-Za-z0-9_]+)"\)', script)),
        ("fortsgfx_tour_", "a tour chapter", re.findall(r'(?<![A-Za-z_])TourMark\("([A-Za-z0-9_]+)"\)', script)),
    ]
    (OUT / "media").mkdir(parents=True, exist_ok=True)
    for old in list(OUT.glob("fortsgfx_*.lua")) + list((OUT / "media").glob("fortsgfx_*.png")):
        old.unlink()
    image = png(32, 32, (255, 255, 255, 1))   # alpha 1/255: invisible without FortsGFX
    for prefix, what, labels in kinds:
        for label in labels:
            name = prefix + label
            (OUT / f"{name}.lua").write_text(EFFECT.format(name=name, media=MAP_DIR, what=what), encoding="utf-8")
            (OUT / "media" / f"{name}.png").write_bytes(image)
        print(f"{len(labels)} {prefix}* markers: {', '.join(labels)}")


if __name__ == "__main__":
    main()
