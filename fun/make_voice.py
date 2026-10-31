"""Generates the milk miko's voice lines with ElevenLabs (Japanese, voice "Kano - Cute & Anime") as 22 kHz mono WAVs
in fun/anime/. Reads the key from ELEVENLABS_API_KEY, or from the file named by ELEVENLABS_API_KEY_FILE (never
printed). Lines already made are skipped unless named on the command line.

    python fun/make_voice.py [line_id ...]
"""
import json
import os
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

KEY = os.environ.get("ELEVENLABS_API_KEY") or Path(os.environ["ELEVENLABS_API_KEY_FILE"]).read_text().strip()
OUT = Path(__file__).with_name("anime")
VOICE = "OSwaPSNdfituxkWcjlkR"   # "Kano - Cute & Anime" (shared library): energetic, cheerful
MODEL = "eleven_v4"
RATE = 22050

LINES = {
    "arrive": "[cheerful] 巫女ミルク、参上！",
    "fire1": "[excited] ミルクビーム！",
    "fire2": "[energetic] いっくよ〜！",
    "fire3": "[playful] えいっ、えいっ！",
    "hit1": "[happy] やったぁ！",
    "hit2": "[giggles] すごーい！",
    "hurt1": "[surprised] いたっ！",
    "hurt2": "[pouting] もう〜、ひどい！",
    "bye": "[dramatic] さよなら〜！",
}


def wav(pcm):
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
            + b"data" + struct.pack("<I", len(pcm)) + pcm)


def make(line_id, text):
    body = json.dumps({"text": text, "model_id": MODEL}).encode()
    req = urllib.request.Request(f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE}?output_format=pcm_{RATE}", data=body,
                                 headers={"xi-api-key": KEY, "Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            pcm = r.read()
    except urllib.error.HTTPError as e:
        print(f"{line_id}: HTTP {e.code} {e.read()[:300]!r}")
        return False
    (OUT / f"voice_{line_id}.wav").write_bytes(wav(pcm))
    print(f"{line_id}: {len(pcm) / 2 / RATE:.2f} s")
    return True


def main():
    OUT.mkdir(exist_ok=True)
    wanted = sys.argv[1:] or list(LINES)
    for line_id in wanted:
        if not sys.argv[1:] and (OUT / f"voice_{line_id}.wav").exists():
            continue
        if not make(line_id, LINES[line_id]):
            break


if __name__ == "__main__":
    main()
