"""Check independent Story track ends in an explicitly isolated validation host."""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import time
import traceback

from pyfbsdk import *


class TrackEndChecks:
    def __init__(self):
        self.output = Path(os.environ['MOBU_TRACK_END_OUTPUT']).resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.result = self.output / 'track-ends-results.json'
        self.system = None
        self.started = False
        self.report = {'status': 'starting', 'started': time.time(), 'pid': os.getpid(),
                       'run_id': os.environ.get('MOBU_VIDEO_TEST_RUN_ID'),
                       'checks': [], 'cases': {},
                       'isolated': os.environ.get('MOBU_TRACK_END_ISOLATED') == '1',
                       'expected_plugin_sha256': os.environ.get('MOBU_TRACK_END_EXPECTED_PLUGIN_SHA256')}
        self.write()

    def write(self):
        temporary = self.result.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(self.report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        temporary.replace(self.result)

    def check(self, name, passed, detail=None):
        self.report['checks'].append({'name': name, 'passed': bool(passed), 'detail': detail})
        self.write()
        if not passed:
            raise AssertionError(name + ': ' + repr(detail))

    @staticmethod
    def digest(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    @staticmethod
    def seconds(value):
        result = FBTime()
        result.SetSecondDouble(value)
        return result

    @staticmethod
    def close(actual, expected):
        return abs(actual - expected) < 1e-5

    @staticmethod
    def pcm_info(path):
        data = Path(path).read_bytes()
        if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
            raise ValueError('Expected RIFF WAVE audio')
        position, info, sample_bytes = 12, None, None
        while position + 8 <= len(data):
            tag, size = struct.unpack_from('<4sI', data, position)
            chunk = data[position + 8:position + 8 + size]
            if len(chunk) != size:
                raise ValueError('Truncated WAVE chunk')
            if tag == b'fmt ':
                encoding, channels, rate, byte_rate, block, bits = struct.unpack_from('<HHIIHH', chunk)
                if encoding == 65534:
                    encoding = struct.unpack_from('<I', chunk, 24)[0]
                if encoding != 1 or bits not in (16, 24, 32) or block != channels * (bits // 8) or byte_rate != rate * block:
                    raise ValueError('Expected byte-aligned integer PCM')
                info = {'channels': channels, 'rate': rate, 'bits': bits, 'block': block}
            elif tag == b'data':
                sample_bytes = size
            position += 8 + size + (size & 1)
        if info is None or sample_bytes is None or sample_bytes % info['block']:
            raise ValueError('Incomplete PCM WAVE audio')
        info['frames'] = sample_bytes // info['block']
        return info

    def arm(self):
        self.check('explicit_isolated_host_required', self.report['isolated'])
        expected = self.report['expected_plugin_sha256']
        self.check('expected_plugin_hash_required', isinstance(expected, str) and bool(re.fullmatch(r'[0-9a-fA-F]{64}', expected)))
        self.fixtures = Path(os.environ['MOBU_TRACK_END_FIXTURES']).resolve()
        self.plugin = Path(os.environ['MOBU_VIDEO_TEST_PLUGIN']).resolve()
        self.report['plugin_sha256'] = self.digest(self.plugin)
        self.check('selected_plugin_matches_expected_hash', self.report['plugin_sha256'] == expected.lower())
        self.native = ctypes.CDLL(str(self.plugin))
        self.native.MoBuNativeMediaImport.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        self.native.MoBuNativeMediaImport.restype = ctypes.c_int
        self.native.MoBuNativeVideoLastError.restype = ctypes.c_char_p
        kernel = ctypes.WinDLL('kernel32.dll')
        kernel.GetModuleFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint]
        kernel.GetModuleFileNameW.restype = ctypes.c_uint
        loaded_path = ctypes.create_unicode_buffer(32768)
        length = kernel.GetModuleFileNameW(self.native._handle, loaded_path, len(loaded_path))
        self.check('loaded_plugin_path_available', 0 < length < len(loaded_path))
        self.report['loaded_plugin_sha256'] = self.digest(Path(loaded_path.value))
        self.check('loaded_plugin_matches_expected_hash', self.report['loaded_plugin_sha256'] == expected.lower())
        self.references = json.loads((self.fixtures / 'track-ends-reference.json').read_text(encoding='utf-8'))
        self.check('eight_independent_reference_cases', isinstance(self.references, list) and len(self.references) == 8)
        files = [item['file'] for item in self.references]
        self.check('reference_cases_have_distinct_local_filenames', len(set(files)) == 8 and all(
            isinstance(name, str) and Path(name).name == name and name.endswith('.mp4') for name in files))
        self.system, self.app, self.player = FBSystem(), FBApplication(), FBPlayerControl()
        self.check('validation_host_is_stopped', not bool(self.player.IsPlaying))
        self.report['status'] = 'waiting_for_idle'
        self.write()
        self.system.OnUIIdle.Add(self.idle)

    def resolved_media(self, value):
        path = Path(str(value))
        if not path.is_absolute():
            scene = Path(str(self.app.FBXFileName))
            if not str(self.app.FBXFileName):
                raise RuntimeError('A relative media path has no saved scene base')
            path = scene.parent / path
        return path.resolve()

    def prepared_media(self, reference, video_clip, audio_clip):
        name = reference['file']
        metadata = video_clip.PropertyList.Find('NativeVideo.PreparedMedia')
        self.check(name + '_prepared_video_metadata', metadata is not None)
        first_image = self.resolved_media(metadata.Data)
        directory = first_image.parent.parent
        records = [line.split() for line in (directory / 'complete.txt').read_text(encoding='utf-8').splitlines()]
        self.check(name + '_manifest_has_complete_header', len(records) >= 6 and len(records[0]) == 3
                   and records[0][0] == 'NativeMediaCache' and records[0][1].isdigit()
                   and records[0][2] == self.digest(self.fixtures / name))
        self.check(name + '_manifest_uses_movie_options', records[1] == ['1', '1', '1', '1', str(reference['video_first'])])
        self.check(name + '_manifest_media_field_counts', len(records[2]) == 6 and len(records[3]) == 5 and len(records[4]) == 9)
        video, audio = [int(value) for value in records[3]], [int(value) for value in records[4]]
        rate_numerator, rate_denominator, count, variable, duration = video
        self.check(name + '_manifest_video_end', duration == reference['video_duration'])
        self.check(name + '_manifest_audio_end', audio[0] == 1 and audio[5] == reference['audio_frames']
                   and audio[7] == reference['audio_duration'])
        self.check(name + '_manifest_frame_timing', bool(variable) == reference['variable']
                   and rate_numerator > 0 and rate_denominator > 0 and count > 0)
        expected_count = reference['video_frames']
        if reference['variable']:
            denominator = 10000000 * rate_denominator
            expected_count = (reference['video_duration'] * rate_numerator + denominator - 1) // denominator
        self.check(name + '_sequence_frame_count', count == expected_count,
                   {'actual': count, 'expected': expected_count})
        images = [directory / 'video' / ('frame%010d.png' % index) for index in range(count)]
        self.check(name + '_png_files_match_sequence', set(images) == set((directory / 'video').glob('*.png')))
        wave = self.resolved_media(audio_clip.AudioClip.Path)
        files = images + [wave]
        self.check(name + '_manifest_has_all_file_records', len(records) == 5 + len(files))
        for index, path in enumerate(files):
            record = records[5 + index]
            self.check(name + '_cache_file_' + str(index), len(record) == 2 and path.is_file()
                       and path.stat().st_size == int(record[0]) and self.digest(path) == record[1])
        info = self.pcm_info(wave)
        self.check(name + '_wave_sample_count', info['frames'] == reference['audio_frames']
                   and info['rate'] == reference['audio_rate'] and info['channels'] == reference['audio_channels'], info)
        self.check(name + '_wave_duration', self.close(info['frames'] / info['rate'], reference['audio_duration'] / 10000000))
        return {'cache_revision': int(records[0][1]), 'cache_directory': str(directory),
                'png_count': count, 'frame_rate': rate_numerator / rate_denominator,
                'audio_sha256': self.digest(wave), 'audio_frames': info['frames']}

    def clip_state(self, clip):
        return {'start': clip.Start.GetSecondDouble(), 'duration': (clip.Stop - clip.Start).GetSecondDouble(),
                'stop': clip.Stop.GetSecondDouble(), 'speed': clip.Speed,
                'first_loop': clip.FirstLoopMarkIn.GetSecondDouble(), 'last_loop': clip.LastLoopMarkOut.GetSecondDouble()}

    def check_pair(self, reference, tracks, phase, prepared):
        name = reference['file'] + '_' + phase
        video_track, audio_track = tracks
        self.check(name + '_one_standard_clip_per_track', len(video_track.Clips) == 1 and len(audio_track.Clips) == 1)
        video, audio = video_track.Clips[0], audio_track.Clips[0]
        actual = {'video': self.clip_state(video), 'audio': self.clip_state(audio)}
        self.check(name + '_video_start', self.close(actual['video']['start'], reference['video_first'] / 10000000), actual)
        self.check(name + '_video_duration', self.close(actual['video']['duration'], reference['video_duration'] / 10000000), actual)
        self.check(name + '_audio_start', self.close(actual['audio']['start'], 0), actual)
        self.check(name + '_audio_duration', self.close(actual['audio']['duration'], reference['audio_duration'] / 10000000), actual)
        self.check(name + '_independent_track_ends', self.close(actual['video']['stop'] - actual['audio']['stop'],
                   (reference['video_end'] - reference['audio_duration']) / 10000000), actual)
        self.check(name + '_standard_image_sequence', bool(video.ImageSequence) and video_track.TrackVideo is not None
                   and self.close(video.FrameRate, prepared['frame_rate']))
        self.check(name + '_audio_ready', audio.AudioClip is not None and audio.AudioClip.IsMediaReady())
        wave = self.resolved_media(audio.AudioClip.Path)
        info = self.pcm_info(wave)
        self.check(name + '_audio_content', info['frames'] == reference['audio_frames']
                   and self.digest(wave) == prepared['audio_sha256'], info)
        self.check(name + '_clip_edits_restored', all(self.close(values['speed'], 1)
                   and self.close(values['first_loop'], 0) for values in actual.values()))
        self.report['cases'][reference['file']][phase] = actual
        self.write()

    def standard_edits(self, reference, track, kind):
        clip = track.Clips[0]
        original = {name: FBTime(getattr(clip, name).Get()) for name in ('Start', 'Stop', 'FirstLoopMarkIn', 'LastLoopMarkOut')}
        speed, mute = clip.Speed, bool(track.Mute)
        name = reference['file'] + '_' + kind
        try:
            clip.MoveTo(self.seconds(original['Start'].GetSecondDouble() + 0.5), True)
            self.system.Scene.Evaluate()
            self.check(name + '_standard_move', self.close(clip.Start.GetSecondDouble(), original['Start'].GetSecondDouble() + 0.5)
                       and self.close((clip.Stop - clip.Start).GetSecondDouble(), (original['Stop'] - original['Start']).GetSecondDouble()))
            clip.MoveTo(original['Start'], True)
            clip.Speed = 2
            self.system.Scene.Evaluate()
            self.check(name + '_standard_speed', self.close(clip.Speed, 2))
            clip.Speed = speed
            clip.FirstLoopMarkIn = self.seconds(0.125)
            clip.Stop = self.seconds(original['Stop'].GetSecondDouble() - 0.125)
            self.system.Scene.Evaluate()
            self.check(name + '_standard_trim', self.close(clip.FirstLoopMarkIn.GetSecondDouble(), 0.125)
                       and self.close(clip.Stop.GetSecondDouble(), original['Stop'].GetSecondDouble() - 0.125))
            track.Mute = True
            self.system.Scene.Evaluate()
            self.check(name + '_standard_mute', bool(track.Mute))
            track.Mute = False
            self.system.Scene.Evaluate()
            self.check(name + '_standard_unmute', not bool(track.Mute))
        finally:
            clip.Speed = speed
            clip.MoveTo(original['Start'], True)
            for property_name in ('FirstLoopMarkIn', 'LastLoopMarkOut', 'Start', 'Stop'):
                setattr(clip, property_name, original[property_name])
            track.Mute = mute
            self.system.Scene.Evaluate()
        self.check(name + '_standard_edits_restore_state', all(getattr(clip, key).Get() == value.Get()
                   for key, value in original.items()) and self.close(clip.Speed, speed) and bool(track.Mute) == mute)

    def save_scene(self, path):
        options = FBFbxOptions(False)
        options.ShowOptionsDialog = False
        options.ShowFileDialog = False
        options.EmbedMedia = False
        self.check('save_' + path.name, bool(self.app.FileSave(str(path), options)) and path.is_file(), str(path))

    def run(self):
        self.player.Stop()
        FBStory().Mute = False
        prepared_cases = {}
        for reference in self.references:
            name = reference['file']
            self.report.update(status='running', phase='import_' + name)
            self.report['cases'][name] = {'reference': reference}
            self.write()
            self.player.Goto(self.seconds(0))
            self.system.Scene.Evaluate()
            before = list(FBStory().RootFolder.Tracks)
            source = self.fixtures / name
            ok = self.native.MoBuNativeMediaImport(str(source).encode('utf-8'), 3,
                str(self.output / 'NativeVideoMedia').encode('utf-8'), 1)
            error = self.native.MoBuNativeVideoLastError()
            self.check(name + '_native_import', ok == 1, error.decode('utf-8', errors='replace') if error else '')
            added = [track for track in FBStory().RootFolder.Tracks if track not in before]
            videos = [track for track in added if track.Type == FBStoryTrackType.kFBStoryTrackVideo]
            audios = [track for track in added if track.Type == FBStoryTrackType.kFBStoryTrackAudio]
            self.check(name + '_creates_standard_video_and_audio_tracks', len(added) == 2 and len(videos) == 1 and len(audios) == 1)
            tracks = videos[0], audios[0]
            for track, kind in zip(tracks, ('Video', 'Audio')):
                track.Name = 'NVTrackEnds_' + source.stem + '_' + kind
            prepared = self.prepared_media(reference, tracks[0].Clips[0], tracks[1].Clips[0])
            prepared['track_names'] = [track.Name for track in tracks]
            prepared_cases[name] = prepared
            self.report['cases'][name]['prepared'] = prepared
            self.check_pair(reference, tracks, 'imported', prepared)
            for track, kind in zip(tracks, ('video', 'audio')):
                self.standard_edits(reference, track, kind)
            self.check_pair(reference, tracks, 'edits_restored', prepared)
            for track in tracks:
                track.Mute = True
        self.player.Goto(self.seconds(0))
        self.system.Scene.Evaluate()
        scene = self.output / 'track-ends.fbx'
        self.report['phase'] = 'save_and_reopen'
        self.write()
        self.save_scene(scene)
        self.app.FileNew()
        self.check('file_new_clears_validation_tracks', not any(track.Name.startswith('NVTrackEnds_') for track in FBStory().RootFolder.Tracks))
        options = FBFbxOptions(True, str(scene))
        options.ShowOptionsDialog = False
        options.ShowFileDialog = False
        self.check('reopen_all_track_end_cases', bool(self.app.FileOpen(str(scene), False, options)))
        self.player.Stop()
        self.system.Scene.Evaluate()
        for reference in self.references:
            prepared = prepared_cases[reference['file']]
            tracks = []
            for name in prepared['track_names']:
                matches = [track for track in FBStory().RootFolder.Tracks if track.Name == name]
                self.check(name + '_reopened_exactly_once', len(matches) == 1)
                tracks.append(matches[0])
            self.check_pair(reference, tracks, 'reopened', prepared)
            self.check(reference['file'] + '_reopened_tracks_muted', all(bool(track.Mute) for track in tracks))
        self.player.Goto(self.seconds(0))
        self.system.Scene.Evaluate()
        self.save_scene(self.output / 'track-ends-reopened.fbx')
        self.report['reopened_scene'] = str(self.output / 'track-ends-reopened.fbx')
        self.report['status'] = 'passed'
        self.report['phase'] = 'complete'

    def fail(self):
        self.report['status'] = 'failed'
        self.report['error'] = traceback.format_exc()
        if self.system is not None:
            try:
                self.player.Stop()
            except Exception:
                self.report['cleanup_error'] = traceback.format_exc()
        self.report['elapsed_seconds'] = time.time() - self.report['started']
        self.write()

    def idle(self, sender, event):
        if self.started:
            return
        self.started = True
        try:
            self.system.OnUIIdle.Remove(self.idle)
            self.run()
            self.report['elapsed_seconds'] = time.time() - self.report['started']
            self.write()
        except Exception:
            self.fail()


_track_end_validation = TrackEndChecks()
try:
    _track_end_validation.arm()
except Exception:
    _track_end_validation.fail()
    raise
