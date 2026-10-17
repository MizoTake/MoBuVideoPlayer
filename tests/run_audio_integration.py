#!/usr/bin/env python3
"""Verify OS PCM exports against original generated samples and FFmpeg references."""

from __future__ import annotations

import argparse
import array
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

from run_decoder_integration import generate_cfr, generate_vfr


def execute(command: list[str], *, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                            timeout=120, check=False)
    if check and result.returncode:
        raise RuntimeError(f"Failed command {command!r}: "
                           + result.stderr.decode("utf-8", errors="replace"))
    return result


def read_wave(path: Path) -> tuple[int, int, int, int, bytes]:
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    assert struct.unpack_from("<I", data, 4)[0] == len(data) - 8
    position = 12
    channels = rate = bits = mask = 0
    pcm = b""
    while position + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, position)
        chunk = data[position + 8:position + 8 + size]
        assert len(chunk) == size
        if tag == b"fmt ":
            encoding, channels, rate, byte_rate, block, bits = struct.unpack_from("<HHIIHH", chunk)
            assert encoding in (1, 65534)
            assert byte_rate == rate * block and block == channels * (bits // 8) and bits in (16, 24, 32)
            if encoding == 65534:
                assert len(chunk) == 40
                assert struct.unpack_from("<H", chunk, 18)[0] == bits
                mask = struct.unpack_from("<I", chunk, 20)[0]
                assert chunk[24:] == bytes.fromhex("0100000000001000800000aa00389b71")
        elif tag == b"data":
            pcm = chunk
        position += 8 + size + (size & 1)
    assert pcm and len(pcm) % (channels * (bits // 8)) == 0
    return channels, rate, bits, mask, pcm


def integers(pcm: bytes, bits: int) -> list[int]:
    width = bits // 8
    return [int.from_bytes(pcm[position:position + width], "little", signed=True)
            for position in range(0, len(pcm), width)]


def compare_pcm_samples(actual: array.array, reference: array.array, channels: int,
                        name: str) -> float:
    count = min(len(actual), len(reference))
    if not count:
        raise AssertionError(name + " has no samples to compare")
    errors = [a - b for a, b in zip(actual, reference)]
    difference = sum(abs(error) for error in errors) / len(actual)
    error_energy = sum(error * error for error in errors)
    actual_energy = sum(sample * sample for sample in actual[:count])
    reference_energy = sum(sample * sample for sample in reference[:count])
    actual_mean = sum(actual[:count]) / count
    reference_mean = sum(reference[:count]) / count
    covariance = sum((a - actual_mean) * (b - reference_mean)
                     for a, b in zip(actual, reference))
    variance_product = ((actual_energy - count * actual_mean * actual_mean)
                        * (reference_energy - count * reference_mean * reference_mean))
    frame_count = count // channels
    maximum_lag = min(2048, (frame_count - 1) // 3)
    positions = range(maximum_lag, min(frame_count - maximum_lag, maximum_lag + 8192), 16)

    def lag_difference(lag: int) -> float:
        return sum(abs(actual[(frame + lag) * channels + channel]
                       - reference[frame * channels + channel])
                   for frame in positions for channel in range(channels)) / (len(positions) * channels)

    coarse = sorted((lag_difference(lag), abs(lag), lag)
                    for lag in range(-maximum_lag, maximum_lag + 1, 16))[:8]
    candidates = {0}
    for _, _, lag in coarse:
        candidates.update(range(max(-maximum_lag, lag - 16), min(maximum_lag, lag + 16) + 1))
    best_difference, _, best_lag = min((lag_difference(lag), abs(lag), lag) for lag in candidates)
    segment_frames = min(1024, frame_count)
    segments = {}
    for segment, first in (("head", 0), ("middle", (frame_count - segment_frames) // 2),
                           ("tail", frame_count - segment_frames)):
        begin, end = first * channels, (first + segment_frames) * channels
        segments[segment] = sum(abs(error) for error in errors[begin:end]) / (end - begin)
    metrics = {
        "name": name, "channels": channels,
        "actual_frames": len(actual) // channels, "reference_frames": len(reference) // channels,
        "mean_absolute_error": difference, "peak_absolute_error": max(map(abs, errors)),
        "actual_rms": math.sqrt(actual_energy / count),
        "reference_rms": math.sqrt(reference_energy / count),
        "error_rms": math.sqrt(error_energy / count),
        "snr_db": (10 * math.log10(reference_energy / error_energy) if reference_energy else "negative infinity")
                  if error_energy else "infinity",
        "zero_lag_correlation": covariance / math.sqrt(variance_product) if variance_product > 0 else 0,
        "best_lag_frames": best_lag, "best_lag_mean_absolute_error": best_difference,
        "lag_search_limit_frames": maximum_lag, "segment_mean_absolute_error": segments,
    }
    print("INFO PCM comparison " + json.dumps(metrics, sort_keys=True), flush=True)
    return difference


def alter_edit_list(data: bytes, *, version_one: bool = False, invalid_rate: bool = False,
                    truncated: bool = False, multiple_edits: bool = False) -> bytes:
    output = bytearray()
    position = 0
    while position + 8 <= len(data):
        size, tag = struct.unpack_from(">I4s", data, position)
        assert 8 <= size <= len(data) - position
        payload = data[position + 8:position + size]
        if tag in (b"moov", b"trak", b"edts"):
            payload = alter_edit_list(payload, version_one=version_one,
                                      invalid_rate=invalid_rate, truncated=truncated,
                                      multiple_edits=multiple_edits)
        elif tag == b"elst":
            assert payload[0] == 0
            count = struct.unpack_from(">I", payload, 4)[0]
            if invalid_rate:
                payload = payload[:-4] + bytes(4)
            elif truncated:
                payload = payload[:4] + struct.pack(">I", count + 1) + payload[8:]
            elif multiple_edits:
                payload = payload[:4] + struct.pack(">I", count + 1) + payload[8:] + payload[-12:]
            elif version_one:
                entries = []
                for index in range(count):
                    duration, media_time, rate = struct.unpack_from(">IiI", payload, 8 + index * 12)
                    entries.append(struct.pack(">QqI", duration, media_time, rate))
                payload = bytes((1, 0, 0, 0)) + struct.pack(">I", count) + b"".join(entries)
        output += struct.pack(">I4s", len(payload) + 8, tag) + payload
        position += size
    assert position == len(data)
    return bytes(output)


def synthetic_timing(decode: list[tuple[int, int]], composition: list[tuple[int, int]],
                     *, signed: bool = True, media_start: int | None = None) -> bytes:
    def atom(tag: bytes, payload: bytes) -> bytes:
        return struct.pack(">I4s", len(payload) + 8, tag) + payload

    timing = atom(b"stts", bytes(4) + struct.pack(">I", len(decode))
                  + b"".join(struct.pack(">II", *run) for run in decode))
    if composition:
        timing += atom(b"ctts", bytes((int(signed), 0, 0, 0)) + struct.pack(">I", len(composition))
                       + b"".join(struct.pack(">Ii" if signed else ">II", *run) for run in composition))
    media = atom(b"mdhd", bytes(12) + struct.pack(">I", 1000) + bytes(8))
    media += atom(b"hdlr", bytes(8) + b"vide" + bytes(12))
    media += atom(b"minf", atom(b"stbl", timing))
    track = atom(b"mdia", media)
    if media_start is not None:
        track += atom(b"edts", atom(b"elst", bytes(4) + struct.pack(">I", 1)
                                   + struct.pack(">IiI", 1000, media_start, 65536)))
    movie = atom(b"mvhd", bytes(12) + struct.pack(">I", 1000) + bytes(8)) + atom(b"trak", track)
    return atom(b"ftyp", b"isom" + bytes(4) + b"isom") + atom(b"moov", movie)


def ticks_from_fraction(value: Fraction) -> int:
    scaled = value * 10_000_000
    return (scaled.numerator * 2 + scaled.denominator) // (scaled.denominator * 2)


def mp4_boxes(data: bytes):
    position = 0
    while position + 8 <= len(data):
        size, tag = struct.unpack_from(">I4s", data, position)
        assert 8 <= size <= len(data) - position
        yield tag, data[position + 8:position + size]
        position += size
    assert position == len(data)


def edit_list_count(data: bytes) -> int:
    count = 0
    for tag, payload in mp4_boxes(data):
        if tag == b"elst":
            count += 1
        elif tag in (b"moov", b"trak", b"edts"):
            count += edit_list_count(payload)
    return count


def edit_list_ends(data: bytes) -> dict[bytes, int]:
    movie = next(payload for tag, payload in mp4_boxes(data) if tag == b"moov")
    header = next(payload for tag, payload in mp4_boxes(movie) if tag == b"mvhd")
    scale = struct.unpack_from(">I", header, 20 if header[0] else 12)[0]
    result = {}
    for tag, track in mp4_boxes(movie):
        if tag != b"trak":
            continue
        handler = edits = None
        for child_tag, child in mp4_boxes(track):
            if child_tag == b"mdia":
                handler = next(payload[8:12] for name, payload in mp4_boxes(child) if name == b"hdlr")
            elif child_tag == b"edts":
                edits = next((payload for name, payload in mp4_boxes(child) if name == b"elst"), None)
        if edits is not None:
            count = struct.unpack_from(">I", edits, 4)[0]
            size, format_code = (20, ">Q") if edits[0] else (12, ">I")
            total = sum(struct.unpack_from(format_code, edits, 8 + index * size)[0] for index in range(count))
            result[handler] = ticks_from_fraction(Fraction(total, scale))
    return result


def generate_track_end_fixtures(ffmpeg: str, ffprobe: str, directory: Path) -> list[dict]:
    short_video = directory / "track-ends-short-video.mp4"
    encoder = "h264_nvenc"
    result = generate_cfr(ffmpeg, short_video, encoder, 192, 128, 6, 12, 0, check=False)
    if result.returncode:
        encoder = "libx264"
        generate_cfr(ffmpeg, short_video, encoder, 192, 128, 6, 12, 0)
    long_video = directory / "track-ends-long-video.mp4"
    bframe_video = directory / "track-ends-bframe-video.mp4"
    generate_cfr(ffmpeg, long_video, encoder, 192, 128, 6, 18, 0)
    generate_cfr(ffmpeg, bframe_video, encoder, 192, 128, 6, 12, 2)
    vfr_directory = directory / "track-ends-vfr-source"
    vfr_directory.mkdir(exist_ok=True)
    vfr_video = generate_vfr(ffmpeg, vfr_directory, encoder)
    tones = {}
    for seconds in (2, 3):
        tone = directory / f"track-ends-tone-{seconds}.aac"
        execute([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
                 f"sine=frequency=440:sample_rate=48000:duration={seconds}",
                 "-c:a", "aac", "-b:a", "128k", "-f", "adts", str(tone)])
        tones[seconds] = tone
    references = []
    for name, source, seconds, frame_count, variable in (
            ("cfr-audio-tail", short_video, 3, 12, False),
            ("cfr-video-tail", long_video, 2, 18, False),
            ("bframe-audio-tail", bframe_video, 3, 12, False),
            ("vfr-audio-tail", vfr_video, 3, 0, True)):
        for edited in (False, True):
            filename = f"track-ends-{name}-{'edit' if edited else 'noedit'}.mp4"
            path = directory / filename
            execute([ffmpeg, "-v", "error", "-y", "-i", str(source), "-i", str(tones[seconds]),
                     "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "copy",
                     "-bsf:a", "aac_adtstoasc", "-use_editlist", str(int(edited)), str(path)])
            probe_command = [ffprobe, "-v", "error", "-show_streams", "-of", "json", str(path)]
            stream_info = json.loads(execute(probe_command).stdout)["streams"]
            video = next(stream for stream in stream_info if stream["codec_type"] == "video")
            audio = next(stream for stream in stream_info if stream["codec_type"] == "audio")
            start = Fraction(int(video["start_pts"])) * Fraction(video["time_base"])
            duration = Fraction(int(video["duration_ts"])) * Fraction(video["time_base"])
            audio_end = Fraction(int(audio["start_pts"]) + int(audio["duration_ts"])) * Fraction(audio["time_base"])
            reference_command = [ffmpeg, "-v", "error", "-i", str(path), "-map", "0:a:0",
                                 "-af", "aresample=async=1:first_pts=0", "-f", "s16le", "-c:a", "pcm_s16le", "pipe:1"]
            pcm = execute(reference_command).stdout
            reference_name = filename + ".timeline.pcm"
            (directory / reference_name).write_bytes(pcm)
            audio_frames = len(pcm) // (2 * int(audio["channels"]))
            frame_command = [ffprobe, "-v", "error", "-select_streams", "a:0", "-show_frames",
                             "-show_entries", "frame=pts,nb_samples", "-of", "json", str(path)]
            decoded_frames = json.loads(execute(frame_command).stdout)["frames"]
            gaps = []
            cursor = 0
            for frame in decoded_frames:
                position = Fraction(int(frame["pts"])) * Fraction(audio["time_base"]) * int(audio["sample_rate"])
                assert position.denominator == 1
                position = int(position)
                if position > cursor:
                    gaps.append({"start_frame": cursor, "frames": position - cursor})
                cursor = max(cursor, position + int(frame["nb_samples"]))
            edit_ends = edit_list_ends(path.read_bytes())
            video_first_ticks = ticks_from_fraction(start)
            video_end_ticks = edit_ends.get(b"vide", ticks_from_fraction(start + duration))
            references.append({"file": filename, "edited": edited, "encoder": encoder,
                               "video_first": video_first_ticks, "video_end": video_end_ticks,
                               "video_duration": video_end_ticks - video_first_ticks, "video_frames": frame_count,
                               "variable": variable, "audio_frames": audio_frames,
                               "audio_duration": ticks_from_fraction(Fraction(audio_frames, int(audio["sample_rate"]))),
                               "audio_end": edit_ends.get(b"soun", ticks_from_fraction(audio_end)), "audio_rate": int(audio["sample_rate"]),
                               "audio_channels": int(audio["channels"]), "b_frames": int(video.get("has_b_frames", 0)),
                               "edit_lists": edit_list_count(path.read_bytes()), "audio_gaps": gaps,
                               "audio_reference_file": reference_name, "audio_reference_command": reference_command,
                               "stream_reference_command": probe_command, "gap_reference_command": frame_command,
                               "end_reference": "elst segment-duration sum / movie time scale" if edited else "FFprobe stream start_pts + duration_ts at stream time_base"})
    rows = ["track_ends\t1"]
    for item in references:
        rows.append("\t".join(map(str, (item["file"], item["video_first"], item["video_duration"],
                                       item["video_frames"], int(item["variable"]), item["audio_frames"], item["audio_duration"]))))
    (directory / "track-ends.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    (directory / "track-ends-reference.json").write_text(json.dumps(references, indent=2) + "\n", encoding="utf-8", newline="\n")
    return references


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-exe", type=Path, required=True)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    directory = args.fixture_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    checks: list[dict] = []

    def require(condition: bool, name: str) -> None:
        checks.append({"name": name, "passed": bool(condition)})
        if not condition:
            raise AssertionError(name)
        print("PASS", name, flush=True)

    def ffmpeg(*command: str) -> None:
        execute([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *command])

    def export(source: Path, name: str, origin: int = 0, limit: int | None = None,
               success: bool = True, cancel_at: int | None = None) -> tuple[dict, Path]:
        output = directory / (name + ".export.wav")
        if output.exists():
            output.unlink()
        command = [str(args.test_exe), str(source), str(output), str(origin)]
        if limit is not None:
            command.append(str(limit))
        if cancel_at is not None:
            if limit is None:
                command.append(str(0xFFFFFFB0))
            command.append(str(cancel_at))
        result = execute(command, check=False)
        require((result.returncode == 0) == success, name + " export result")
        if not success:
            require(not output.exists() and not list(directory.glob(output.name + ".partial.*")),
                    name + " failure removes incomplete output")
            return {}, output
        return json.loads(result.stdout.decode("utf-8")), output

    fixtures = [
        ("mono", "sine=frequency=440:sample_rate=44100:duration=1", "pcm_s16le", 1, 44100, 16, False),
        ("stereo", "aevalsrc=0.2*sin(2*PI*440*t)|0.1*sin(2*PI*880*t):s=48000:d=1", "pcm_s16le", 2, 48000, 16, False),
        ("surround", "aevalsrc=0.1*sin(2*PI*220*t)|0.1*sin(2*PI*330*t)|0.1*sin(2*PI*440*t)|0.1*sin(2*PI*55*t)|0.1*sin(2*PI*550*t)|0.1*sin(2*PI*660*t):s=48000:d=1:c=5.1", "pcm_s16le", 6, 48000, 16, False),
        ("pcm24", "aevalsrc=0.123456789*sin(2*PI*431*t)+0.000012345*sin(2*PI*1733*t):s=48000:d=1", "pcm_s24le", 1, 48000, 24, False),
        ("float", "aevalsrc=0.123456789*sin(2*PI*431*t)+0.000012345*sin(2*PI*1733*t):s=48000:d=1", "pcm_f32le", 1, 48000, 24, True),
        ("pcm8", "sine=frequency=330:sample_rate=48000:duration=1", "pcm_u8", 1, 48000, 16, False),
        ("pcm32mono", "aevalsrc=0.123456789*sin(2*PI*431*t):s=48000:d=1", "pcm_s32le", 1, 48000, 32, False),
        ("pcm32stereo", "aevalsrc=0.123456789*sin(2*PI*431*t)|0.073456789*sin(2*PI*647*t):s=48000:d=1", "pcm_s32le", 2, 48000, 24, True),
    ]
    try:
        for name, expression, codec, channels, rate, expected_bits, reduced in fixtures:
            source = directory / (name + ".wav")
            ffmpeg("-f", "lavfi", "-i", expression, "-c:a", codec, str(source))
            info, output = export(source, name)
            actual_channels, actual_rate, bits, mask, pcm = read_wave(output)
            require(info["hasAudio"] and (actual_channels, actual_rate, bits) == (channels, rate, expected_bits),
                    name + " preserves channels and sample rate with automatic precision")
            require(info["precisionReduced"] == reduced and info["bitsPerSample"] == expected_bits,
                    name + " reports selected precision and playback conversion")
            require(info["frameCount"] == rate and info["duration100ns"] == 10_000_000,
                    name + " preserves duration and sample count")
            reference = execute([args.ffmpeg, "-v", "error", "-i", str(source),
                                 "-f", "s" + str(expected_bits) + "le", "-c:a",
                                 "pcm_s" + str(expected_bits) + "le", "pipe:1"]).stdout
            if not reduced:
                require(pcm == reference, name + " native PCM sample values are unchanged")
            else:
                actual_samples = integers(pcm, bits)
                reference_samples = integers(reference, bits)
                maximum = max(abs(a - b) for a, b in zip(actual_samples, reference_samples))
                print("INFO PCM precision " + json.dumps({"name": name, "actual_bytes": len(pcm),
                                                          "reference_bytes": len(reference),
                                                          "peak_absolute_error": maximum}), flush=True)
                require(len(pcm) == len(reference) and maximum <= 1,
                        name + " PCM24 playback conversion differs by at most one quantization level")
            if channels == 6:
                require(mask == info["channelMask"] == 63, "surround preserves speaker mask")

        odd_source = directory / "odd-pcm24.wav"
        ffmpeg("-f", "lavfi", "-i", "aevalsrc=0.123456789*sin(2*PI*431*t):s=48000:d=1.1",
               "-af", "atrim=end_sample=48001", "-c:a", "pcm_s24le", str(odd_source))
        odd_info, odd_output = export(odd_source, "odd-pcm24")
        odd_pcm = read_wave(odd_output)[4]
        reference = execute([args.ffmpeg, "-v", "error", "-i", str(odd_source),
                             "-f", "s24le", "-c:a", "pcm_s24le", "pipe:1"]).stdout
        require(odd_info["frameCount"] == 48001 and odd_pcm == reference,
                "odd PCM24 sample count preserves every source byte")
        require(len(odd_output.read_bytes()) == 44 + len(odd_pcm) + 1,
                "odd PCM24 data chunk includes its RIFF alignment padding")

        stereo = directory / "stereo.wav"
        source_pcm = read_wave(directory / "stereo.export.wav")[4]
        delayed, delayed_path = export(stereo, "negative-origin", -2_500_000)
        delayed_pcm = read_wave(delayed_path)[4]
        require(delayed_pcm == bytes(12000 * 4) + source_pcm,
                "negative timeline origin inserts exact leading silence")
        require(delayed["frameCount"] == 60000, "leading silence contributes to duration")
        _, trimmed_path = export(stereo, "positive-origin", 2_500_000)
        require(read_wave(trimmed_path)[4] == source_pcm[12000 * 4:],
                "positive timeline origin trims exact preceding samples")

        unicode_source = directory / "音声 参照.wav"
        unicode_source.write_bytes(stereo.read_bytes())
        _, unicode_output = export(unicode_source, "日本語 出力")
        require(read_wave(unicode_output)[4] == source_pcm, "Unicode source and destination paths")

        aac = directory / "compressed.m4a"
        ffmpeg("-i", str(stereo), "-c:a", "aac", "-b:a", "192k", str(aac))
        inspection = json.loads(execute([str(args.test_exe), "--inspect-audio", str(aac)]).stdout)
        print("INFO AAC Source Reader " + json.dumps(inspection, sort_keys=True), flush=True)
        timeline = json.loads(execute([str(args.test_exe), "--timeline", str(aac)]).stdout)
        print("INFO AAC timeline " + json.dumps(timeline, sort_keys=True), flush=True)
        info, aac_output = export(aac, "aac")
        require(info["firstPresentation100ns"] == timeline["audioFirst100ns"],
                "AAC source clock retains the canonical MP4 priming presentation time")
        _, aac_rate, _, _, pcm = read_wave(aac_output)
        require(info["hasAudio"] and info["channels"] == 2 and aac_rate == 48000,
                "AAC decodes through Windows PCM output")
        require(info["bitsPerSample"] == 16 and not info["precisionReduced"],
                "AAC keeps the existing PCM16 policy without a new precision conversion")
        values = array.array("h", pcm)
        rms = math.sqrt(sum(value * value for value in values) / len(values))
        print("INFO AAC output " + json.dumps({"rms": rms, "actual_frames": len(pcm) // 4,
                                               "reported_frames": info["frameCount"],
                                               "first_presentation100ns": info["firstPresentation100ns"]}), flush=True)
        require(2000 < rms < 5000 and abs(len(pcm) // 4 - 48000) <= 2048,
                "AAC decoded audio has expected energy and duration")
        require(info["frameCount"] == 48000, "AAC edit list removes priming and trailing padding")
        reference = execute([args.ffmpeg, "-v", "error", "-i", str(aac),
                             "-f", "s16le", "-c:a", "pcm_s16le", "pipe:1"]).stdout
        reference_samples = array.array("h", reference)
        difference = compare_pcm_samples(values, reference_samples, info["channels"], "AAC reference phase")
        require(difference <= 3, "AAC phase matches independent decoded reference within three levels on average")

        silent_video = directory / "silent.mp4"
        video_command = ["-f", "lavfi", "-i", "color=c=blue:s=64x64:r=10:d=1", "-an",
                         "-c:v", "h264_nvenc", "-bf", "2", "-pix_fmt", "yuv420p", str(silent_video)]
        video_result = execute([args.ffmpeg, "-v", "error", "-y", *video_command], check=False)
        if video_result.returncode:
            video_command[video_command.index("h264_nvenc")] = "libx264"
            ffmpeg(*video_command)
        ffmpeg("-i", str(silent_video), "-i", str(directory / "surround.wav"),
               "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac",
               "-b:a", "384k", str(directory / "surround-video.mp4"))
        info, no_audio_path = export(silent_video, "no-audio")
        require(not info["hasAudio"] and not no_audio_path.exists(),
                "video without audio succeeds without creating WAV")

        offset_mp4 = directory / "offset.mp4"
        ffmpeg("-i", str(silent_video), "-itsoffset", "0.5", "-i", str(stereo),
               "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac",
               "-b:a", "192k", str(offset_mp4))
        info, offset_output = export(offset_mp4, "offset")
        timeline = json.loads(execute([str(args.test_exe), "--timeline", str(offset_mp4)]).stdout)
        require(info["firstPresentation100ns"] == timeline["audioFirst100ns"],
                "delayed AAC source clock retains its canonical MP4 presentation time")
        offset_pcm = read_wave(offset_output)[4]
        require(info["firstPresentation100ns"] >= 4_000_000,
                "delayed source reports movie audio presentation origin")
        require(not any(offset_pcm[:40000]), "delayed source preserves leading silence")
        require(info["frameCount"] > 68000, "delayed source retains delayed tail")
        require(info["frameCount"] == 72000, "delayed source ends at edit list duration")
        origin = json.loads(execute([str(args.test_exe), "--video-origin", str(offset_mp4)]).stdout)
        require(origin["nativeVideoOrigin100ns"] > 0 and origin["movieVideoOrigin100ns"] == 0,
                "B-frame video edit list maps native origin to movie origin")
        _, aligned_output = export(offset_mp4, "aligned-offset", origin["movieVideoOrigin100ns"])
        require(read_wave(aligned_output)[4] == offset_pcm,
                "B-frame video and delayed AAC share the movie timeline")

        combined = directory / "combined.mp4"
        ffmpeg("-i", str(silent_video), "-i", str(stereo), "-map", "0:v", "-map", "1:a",
               "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", str(combined))
        origin = json.loads(execute([str(args.test_exe), "--video-origin", str(combined)]).stdout)
        combined_info, combined_output = export(combined, "combined", origin["movieVideoOrigin100ns"])
        timeline = json.loads(execute([str(args.test_exe), "--timeline", str(combined)]).stdout)
        require(combined_info["firstPresentation100ns"] == timeline["audioFirst100ns"],
                "B-frame AAC source clock retains its canonical MP4 presentation time")
        require(origin["movieVideoOrigin100ns"] == 0 and combined_info["frameCount"] == 48000,
                "B-frame video with AAC retains the complete aligned second")
        combined_samples = array.array("h", read_wave(combined_output)[4])
        difference = compare_pcm_samples(combined_samples, reference_samples, combined_info["channels"],
                                         "B-frame video AAC reference phase")
        require(difference <= 3, "B-frame video and AAC preserve reference phase")

        delayed_video = directory / "video-delayed.mp4"
        ffmpeg("-itsoffset", "0.5", "-i", str(silent_video), "-i", str(stereo), "-map", "0:v", "-map", "1:a",
               "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", str(delayed_video))
        origin = json.loads(execute([str(args.test_exe), "--video-origin", str(delayed_video)]).stdout)
        require(origin["movieVideoOrigin100ns"] == 5_000_000,
                "leading empty video edit has canonical half-second presentation origin")
        delayed_info, delayed_output = export(delayed_video, "video-delayed", origin["movieVideoOrigin100ns"])
        timeline = json.loads(execute([str(args.test_exe), "--timeline", str(delayed_video)]).stdout)
        require(delayed_info["firstPresentation100ns"] == timeline["audioFirst100ns"],
                "video-delayed AAC source clock retains its canonical MP4 presentation time")
        require(delayed_info["frameCount"] == 24000,
                "video-normalized audio trims exactly the half-second before first video")
        delayed_samples = array.array("h", read_wave(delayed_output)[4])
        reference_tail = reference_samples[24000 * 2:]
        difference = compare_pcm_samples(delayed_samples, reference_tail, delayed_info["channels"],
                                         "Leading empty edit AAC reference phase")
        require(difference <= 3, "leading empty video edit keeps the correct AAC phase")

        signed_timing = directory / "signed-timing.mp4"
        signed_timing.write_bytes(synthetic_timing([(2, 100), (2, 200)], [(1, 200), (1, -100), (2, 0)]))
        timeline = json.loads(execute([str(args.test_exe), "--timeline", str(signed_timing)]).stdout)
        require(timeline["videoFirst100ns"] == 0, "signed composition offsets determine earliest presentation across runs")
        large_timing = directory / "large-timing.mp4"
        large_timing.write_bytes(synthetic_timing([(4_000_000_000, 1)], [(4_000_000_000, 100)], signed=False))
        timeline = json.loads(execute([str(args.test_exe), "--timeline", str(large_timing)]).stdout)
        require(timeline["videoFirst100ns"] == 1_000_000, "large timing runs do not require expanding individual samples")
        trimmed_timing = directory / "trimmed-timing.mp4"
        trimmed_timing.write_bytes(synthetic_timing([(10, 100)], [], media_start=200))
        require(execute([str(args.test_exe), "--timeline", str(trimmed_timing)], check=False).returncode != 0,
                "video edit trimming preroll is rejected instead of guessing its first decoded frame")
        mismatched_timing = directory / "mismatched-timing.mp4"
        mismatched_timing.write_bytes(synthetic_timing([(10, 100)], [(9, 0)]))
        require(execute([str(args.test_exe), "--timeline", str(mismatched_timing)], check=False).returncode != 0,
                "decode and composition timing sample count mismatch is rejected")

        wide_edit = directory / "wide-edit.m4a"
        wide_edit.write_bytes(alter_edit_list(aac.read_bytes(), version_one=True))
        normal_timeline = json.loads(execute([str(args.test_exe), "--timeline", str(aac)]).stdout)
        wide_timeline = json.loads(execute([str(args.test_exe), "--timeline", str(wide_edit)]).stdout)
        require(normal_timeline == wide_timeline, "64-bit edit list preserves timeline mapping")
        bad_rate = directory / "bad-rate.m4a"
        bad_rate.write_bytes(alter_edit_list(aac.read_bytes(), invalid_rate=True))
        require(execute([str(args.test_exe), "--timeline", str(bad_rate)], check=False).returncode != 0,
                "changed edit rate is rejected instead of producing incorrect timing")
        malformed_edit = directory / "malformed-edit.m4a"
        malformed_edit.write_bytes(alter_edit_list(aac.read_bytes(), truncated=True))
        require(execute([str(args.test_exe), "--timeline", str(malformed_edit)], check=False).returncode != 0,
                "truncated edit list is rejected")
        repeated_edit = directory / "repeated-edit.m4a"
        repeated_edit.write_bytes(alter_edit_list(aac.read_bytes(), multiple_edits=True))
        require(execute([str(args.test_exe), "--timeline", str(repeated_edit)], check=False).returncode != 0,
                "multiple active edits are rejected instead of flattening their timing")

        export(stereo, "limited", limit=100, success=False)
        export(stereo, "zero-limit", limit=0, success=False)
        export(stereo, "cancel-initial", cancel_at=0, success=False)
        export(stereo, "cancel-partial", cancel_at=4096, success=False)
        export(directory / "missing.mp4", "missing", success=False)
        export(directory, "directory", success=False)
        corrupt = directory / "corrupt.mp4"
        corrupt.write_bytes(b"not a valid media source\x00")
        export(corrupt, "corrupt", success=False)
        destination = directory / "existing.wav"
        sentinel = b"retain original destination bytes"
        destination.write_bytes(sentinel)
        retained = execute([str(args.test_exe), str(stereo), str(destination)], check=False)
        require(retained.returncode != 0 and destination.read_bytes() == sentinel,
                "existing destination is never overwritten")
        require(not list(directory.glob("*.partial.*")), "all completed operations leave no partial files")
        track_references = generate_track_end_fixtures(args.ffmpeg, args.ffprobe, directory)
        for item in track_references:
            name = item["file"]
            source = directory / name
            require((item["edit_lists"] > 0) == item["edited"], name + " has the requested edit-list structure")
            require(not name.startswith("track-ends-bframe") or item["b_frames"] > 0,
                    name + " retains its encoded frame structure")
            timeline = json.loads(execute([str(args.test_exe), "--timeline", str(source)]).stdout)
            require(timeline["videoHasEdit"] == item["edited"], name + " reports its video edit-list presence")
            require(timeline["videoFirst100ns"] == item["video_first"], name + " retains its independent video start")
            require(timeline["videoEnd100ns"] == item["video_end"], name + " ends video at the independent stream boundary")
            require(timeline["audioEnd100ns"] == item["audio_end"], name + " ends audio at the independent stream boundary")
            info, output = export(source, source.stem)
            require(info["firstPresentation100ns"] == timeline["audioFirst100ns"],
                    name + " source clock retains its canonical MP4 presentation time")
            channels, rate, bits, _, pcm = read_wave(output)
            require((channels, rate, bits) == (item["audio_channels"], item["audio_rate"], 16), name + " keeps the AAC PCM format")
            require(info["frameCount"] == item["audio_frames"] and len(pcm) == item["audio_frames"] * channels * 2,
                    name + " preserves every decoded audio sample")
            require(info["duration100ns"] == item["audio_duration"], name + " preserves its independent audio duration")
            reference_pcm = (directory / item["audio_reference_file"]).read_bytes()
            samples = array.array("h", pcm)
            reference_samples = array.array("h", reference_pcm)
            difference = compare_pcm_samples(samples, reference_samples, channels, name + " timestamp-aware AAC")
            require(len(pcm) == len(reference_pcm) and difference <= 3,
                    name + " matches timestamp-aware AAC samples within three levels on average")
            gap_bytes = channels * 2
            require(all(not any(pcm[gap["start_frame"] * gap_bytes:(gap["start_frame"] + gap["frames"]) * gap_bytes])
                        and not any(reference_pcm[gap["start_frame"] * gap_bytes:(gap["start_frame"] + gap["frames"]) * gap_bytes])
                        for gap in item["audio_gaps"]), name + " preserves silence at independently probed timestamp gaps")
        signed_timeline = json.loads(execute([str(args.test_exe), "--timeline", str(signed_timing)]).stdout)
        require(signed_timeline["videoEnd100ns"] == 6_000_000,
                "signed nonmonotonic composition runs use the latest sample end")
        large_timeline = json.loads(execute([str(args.test_exe), "--timeline", str(large_timing)]).stdout)
        require(large_timeline["videoEnd100ns"] == 40_000_001_000_000,
                "large sample runs preserve their end without expanding billions of samples")
        variable_timing = directory / "variable-timing.mp4"
        variable_timing.write_bytes(synthetic_timing([(1, 120), (1, 280), (1, 80), (1, 360)], []))
        variable_timeline = json.loads(execute([str(args.test_exe), "--timeline", str(variable_timing)]).stdout)
        require(variable_timeline["videoFirst100ns"] == 0 and variable_timeline["videoEnd100ns"] == 8_400_000,
                "variable decode durations retain the independent final sample boundary")
        zero_duration = directory / "zero-duration-timing.mp4"
        zero_duration.write_bytes(synthetic_timing([(2, 0)], []))
        require(execute([str(args.test_exe), "--timeline", str(zero_duration)], check=False).returncode != 0,
                "zero-duration MP4 sample runs are rejected")
        decode_overflow = directory / "decode-overflow-timing.mp4"
        decode_overflow.write_bytes(synthetic_timing([(0xFFFFFFFF, 0xFFFFFFFF)], []))
        require(execute([str(args.test_exe), "--timeline", str(decode_overflow)], check=False).returncode != 0,
                "decode-end overflow is rejected without wrapping the track duration")
        composition_overflow = directory / "composition-overflow-timing.mp4"
        composition_overflow.write_bytes(synthetic_timing([(0xFFFFFFFF, 0x80000000)], [(0xFFFFFFFF, 0xFFFFFFFF)], signed=False))
        require(execute([str(args.test_exe), "--timeline", str(composition_overflow)], check=False).returncode != 0,
                "composition-end overflow is rejected even when decode duration fits")
        report = {"status": "passed", "checks": checks, "count": len(checks)}
        (directory / "integration-results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"PASS {len(checks)} audio integration checks")
        return 0
    except Exception as error:
        report = {"status": "failed", "checks": checks, "error": str(error)}
        (directory / "integration-results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        raise


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    sys.exit(main())
