#!/usr/bin/env python3
"""Generate original H.264 fixtures and verify the Windows native decoder.

FFmpeg and FFprobe are development tools only; the plug-in never invokes them.
The reference uses decoded pixels and presentation timestamps from the encoded
file so encoder reordering and variable frame timing are covered by the test.
"""

from __future__ import annotations

import argparse
from decimal import Decimal
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


PALETTE = (
    (240, 16, 16), (16, 220, 24), (20, 32, 240), (235, 210, 20),
    (210, 20, 220), (20, 215, 210), (210, 115, 30), (125, 30, 205),
    (40, 130, 210), (210, 60, 110), (100, 210, 40), (180, 180, 180),
)


def execute(command: list[str], *, data: bytes | None = None, check: bool = True,
            timeout: int = 120) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        timeout=timeout, check=False,
    )
    if check and result.returncode:
        raise RuntimeError(
            f"Command failed ({result.returncode}): {command!r}\n"
            + result.stderr.decode("utf-8", errors="replace")
        )
    return result


def ticks(value: str | Decimal) -> int:
    return int((Decimal(value) * 10_000_000).to_integral_value())


def rgb_frame(width: int, height: int, index: int) -> bytes:
    red, green, blue = PALETTE[index % len(PALETTE)]
    pixels = bytearray(bytes((red, green, blue)) * width * height)
    marker_size = max(4, min(width, height) // 12)
    for y in range(marker_size):
        for x in range(marker_size):
            offset = (y * width + x) * 3
            pixels[offset:offset + 3] = bytes((255, 255, 255))
    return bytes(pixels)


def encoder_options(encoder: str, bframes: int) -> list[str]:
    if encoder == "h264_nvenc":
        return ["-c:v", encoder, "-preset", "p4", "-rc", "constqp", "-qp", "18",
                "-bf", str(bframes), "-g", "12"]
    return ["-c:v", "libx264", "-preset", "fast", "-crf", "15",
            "-bf", str(bframes), "-g", "12", "-x264-params", "scenecut=0"]


def generate_cfr(ffmpeg: str, output: Path, encoder: str, width: int, height: int,
                 fps: int, frame_count: int, bframes: int, *, check: bool = True):
    raw = b"".join(rgb_frame(width, height, index) for index in range(frame_count))
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
               "-pixel_format", "rgb24", "-video_size", f"{width}x{height}",
               "-framerate", str(fps), "-i", "pipe:0", "-an"]
    command += encoder_options(encoder, bframes)
    command += ["-pix_fmt", "yuv420p", "-movflags", "+faststart", str(output)]
    return execute(command, data=raw, check=check)


def generate_vfr(ffmpeg: str, directory: Path, encoder: str) -> Path:
    source = directory / "vfr-source"
    source.mkdir(exist_ok=True)
    durations = ("0.12", "0.28", "0.08", "0.36", "0.16", "0.24", "0.40", "0.20")
    lines = ["ffconcat version 1.0"]
    for index, duration in enumerate(durations):
        name = f"frame-{index:02d}.ppm"
        (source / name).write_bytes(b"P6\n256 144\n255\n" + rgb_frame(256, 144, index))
        lines += [f"file '{name}'", f"duration {duration}"]
    lines.append(f"file 'frame-{len(durations) - 1:02d}.ppm'")
    concat = source / "input.ffconcat"
    concat.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    output = directory / "vfr.mp4"
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "concat",
               "-safe", "0", "-i", str(concat), "-an"]
    command += encoder_options(encoder, 0)
    command += ["-pix_fmt", "yuv420p", "-fps_mode", "vfr", "-movflags", "+faststart", str(output)]
    execute(command)
    return output


def reference(ffmpeg: str, ffprobe: str, file: Path, name: str, exact_fps: bool,
              normalize_start: bool = False) -> dict:
    probe = json.loads(execute([
        ffprobe, "-v", "error", "-select_streams", "v:0", "-show_streams", "-show_format",
        "-show_frames", "-show_entries",
        "frame=best_effort_timestamp_time,pts_time,pkt_duration_time,duration_time,pict_type:"
        "stream=width,height,r_frame_rate,avg_frame_rate,duration,codec_name:format=start_time,duration",
        "-of", "json", str(file),
    ]).stdout)
    stream = probe["streams"][0]
    source_start = ticks(probe["format"].get("start_time", "0"))
    format_duration = ticks(probe["format"]["duration"])
    duration = source_start + format_duration
    width, height = int(stream["width"]), int(stream["height"])
    rate = Fraction(stream["r_frame_rate"])
    output = file.with_suffix(".reference.bgra")
    execute([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(file),
             "-map", "0:v:0", "-an", "-fps_mode", "passthrough", "-pix_fmt", "bgra",
             "-f", "rawvideo", str(output)])
    raw = output.read_bytes()
    output.unlink()
    size = width * height * 4
    frames = probe["frames"]
    if len(raw) != size * len(frames):
        raise RuntimeError(f"FFprobe/FFmpeg frame count mismatch for {name}")
    expected = []
    for index, frame in enumerate(frames):
        pts = ticks(frame.get("pts_time", frame["best_effort_timestamp_time"]))
        if "duration_time" in frame:
            frame_duration = ticks(frame["duration_time"])
        elif "pkt_duration_time" in frame:
            frame_duration = ticks(frame["pkt_duration_time"])
        elif index + 1 < len(frames):
            frame_duration = ticks(frames[index + 1]["best_effort_timestamp_time"]) - pts
        else:
            frame_duration = duration - pts
        sums = [0, 0, 0]
        count = 0
        for y in range(height // 4, height * 3 // 4):
            for x in range(width // 4, width * 3 // 4):
                offset = index * size + (y * width + x) * 4
                for channel in range(3):
                    sums[channel] += raw[offset + channel]
                count += 1
        expected.append({"pts": pts, "source_pts": pts, "duration": frame_duration,
                         "bgr": [value / count for value in sums],
                         "pict_type": frame.get("pict_type", "?")})
    reference_origin = expected[0]["source_pts"] if normalize_start else 0
    if normalize_start:
        if reference_origin <= 0:
            raise RuntimeError("Nonzero-start fixture has no positive source PTS offset")
        for frame in expected:
            frame["pts"] -= reference_origin
    if any(a["pts"] >= b["pts"] for a, b in zip(expected, expected[1:])):
        raise RuntimeError(f"Reference PTS are not strictly increasing for {name}")
    if expected[-1]["pts"] >= duration:
        raise RuntimeError(f"Container duration does not cover the last presentation timestamp for {name}")
    if name == "bframes" and not any(frame["pict_type"] == "B" for frame in expected):
        raise RuntimeError("B-frame fixture contains no B-frames")
    if name == "vfr" and len({b["pts"] - a["pts"] for a, b in zip(expected, expected[1:])}) < 3:
        raise RuntimeError("VFR fixture has fewer than three distinct frame intervals")
    return {"name": name, "file": file.name, "width": width, "height": height,
            "duration": duration, "fps_n": rate.numerator, "fps_d": rate.denominator,
            "exact_fps": exact_fps, "frames": expected, "codec": stream["codec_name"],
            "reference_pts_origin100ns": reference_origin,
            "format_start100ns": source_start, "format_duration100ns": format_duration,
            "sha256": hashlib.sha256(file.read_bytes()).hexdigest()}


def write_manifest(directory: Path, fixtures: list[dict]) -> None:
    lines = ["version\t1"]
    for fixture in fixtures:
        lines.append("\t".join(map(str, (
            "fixture", fixture["name"], fixture["file"], fixture["width"], fixture["height"],
            fixture["duration"], fixture["fps_n"], fixture["fps_d"], int(fixture["exact_fps"]),
            len(fixture["frames"]), "BGRA",
        ))))
        for index, frame in enumerate(fixture["frames"]):
            lines.append("\t".join(map(str, (
                "frame", fixture["name"], frame["pts"], frame["duration"], *frame["bgr"], index,
            ))))
    (directory / "manifest.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def tool_path(value: str) -> str:
    found = shutil.which(value)
    if not found:
        raise RuntimeError(f"Existing development tool is required: {value}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-exe", type=Path)
    parser.add_argument("--fixture-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "test-fixtures")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--encoder", choices=("auto", "h264_nvenc", "libx264"), default="auto")
    parser.add_argument("--generate-only", action="store_true")
    arguments = parser.parse_args()
    if not arguments.generate_only and arguments.test_exe is None:
        parser.error("--test-exe is required unless --generate-only is selected")

    directory = arguments.fixture_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    ffmpeg, ffprobe = tool_path(arguments.ffmpeg), tool_path(arguments.ffprobe)
    report = {"started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "source": "Original procedural solid-color frames with a top-left orientation marker",
              "ffmpeg": execute([ffmpeg, "-version"]).stdout.decode("utf-8", errors="replace").splitlines()[0],
              "gpu": [], "encoder_fallback": None}
    nvidia_smi = shutil.which("nvidia-smi")
    if nvidia_smi:
        gpu = execute([nvidia_smi, "--query-gpu=name,driver_version", "--format=csv,noheader"], check=False)
        report["gpu"] = gpu.stdout.decode("utf-8", errors="replace").splitlines()

    cfr = directory / "cfr.mp4"
    encoder = "h264_nvenc" if arguments.encoder == "auto" else arguments.encoder
    attempt = generate_cfr(ffmpeg, cfr, encoder, 192, 128, 6, 12, 0, check=False)
    if attempt.returncode:
        if arguments.encoder != "auto":
            raise RuntimeError(attempt.stderr.decode("utf-8", errors="replace"))
        report["encoder_fallback"] = attempt.stderr.decode("utf-8", errors="replace")
        encoder = "libx264"
        generate_cfr(ffmpeg, cfr, encoder, 192, 128, 6, 12, 0)
    report["encoder"] = encoder
    bframes = directory / "bframes.mp4"
    generate_cfr(ffmpeg, bframes, encoder, 160, 96, 12, 24, 2)
    padded = directory / "padded.mp4"
    generate_cfr(ffmpeg, padded, encoder, 194, 130, 5, 10, 0)
    offset = directory / "offset.mp4"
    execute([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-itsoffset", "1",
             "-i", str(cfr), "-map", "0:v:0", "-c:v", "copy", "-an",
             "-movflags", "+faststart", str(offset)])
    vfr = generate_vfr(ffmpeg, directory, encoder)
    audio_tail = directory / "audio-tail.mp4"
    execute([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(cfr),
             "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=4",
             "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac",
             "-b:a", "96k", "-movflags", "+faststart", str(audio_tail)])
    execute([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(audio_tail),
             "-vn", "-c:a", "copy", str(directory / "audio-only.m4a")])
    unicode_file = directory / "動画_テスト_ñ_日本語.mp4"
    shutil.copyfile(cfr, unicode_file)
    (directory / "corrupt.mp4").write_bytes(b"Original deliberately invalid MP4 fixture\x00\xff")

    fixtures = [reference(ffmpeg, ffprobe, cfr, "cfr", True),
                reference(ffmpeg, ffprobe, bframes, "bframes", True),
                reference(ffmpeg, ffprobe, padded, "padded", True),
                reference(ffmpeg, ffprobe, offset, "offset", True, normalize_start=True),
                reference(ffmpeg, ffprobe, vfr, "vfr", False),
                reference(ffmpeg, ffprobe, audio_tail, "audio-tail", True),
                reference(ffmpeg, ffprobe, unicode_file, "unicode", True)]
    write_manifest(directory, fixtures)
    report["fixtures"] = fixtures
    print(f"Generated {len(fixtures)} original fixtures with {encoder}: {directory}")
    for fixture in fixtures:
        print(f"  {fixture['name']}: {fixture['width']}x{fixture['height']}, "
              f"{len(fixture['frames'])} frames, {fixture['duration'] / 10_000_000:.3f}s")

    result = 0
    if not arguments.generate_only:
        command = [str(arguments.test_exe.resolve()), "--fixture-dir", str(directory)]
        test = execute(command, check=False, timeout=180)
        stdout = test.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")
        stderr = test.stderr.decode("utf-8", errors="replace").replace("\r\n", "\n")
        report["test"] = {"command": command, "exit_code": test.returncode,
                          "stdout": stdout, "stderr": stderr}
        print(stdout, end="")
        if stderr:
            print(stderr, file=sys.stderr, end="")
        result = test.returncode
    report["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    report_file = directory / "integration-results.json"
    report_file.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8", newline="\n")
    print(f"Evidence: {report_file}")
    return result


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
