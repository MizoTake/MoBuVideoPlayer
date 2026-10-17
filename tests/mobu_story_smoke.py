"""Verify the shipped media import path and standard Story output in a dedicated host."""

import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import struct
import time
import traceback

from pyfbsdk import *


PCM24_TONE_FREQUENCY = 431


def pcm_wave(path):
    data = Path(path).read_bytes()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('Expected a RIFF WAVE file')
    position = 12
    info, samples = None, None
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
                raise ValueError('Expected byte-aligned PCM samples')
            info = {'channels': channels, 'rate': rate, 'bits': bits, 'block': block}
        elif tag == b'data':
            samples = chunk
        position += 8 + size + (size & 1)
    if info is None or samples is None or len(samples) % info['block']:
        raise ValueError('Incomplete PCM WAVE data')
    return info, samples


def pcm_tone_amplitude(info, samples, frequency):
    width = info['bits'] // 8
    scale = 1 << (info['bits'] - 1)
    mono = [sum(int.from_bytes(samples[frame + channel * width:frame + (channel + 1) * width],
                               'little', signed=True) for channel in range(info['channels']))
            / (scale * info['channels']) for frame in range(0, len(samples), info['block'])]
    sine = sum(value * math.sin(2 * math.pi * frequency * index / info['rate']) for index, value in enumerate(mono))
    cosine = sum(value * math.cos(2 * math.pi * frequency * index / info['rate']) for index, value in enumerate(mono))
    return 2 * math.hypot(sine, cosine) / len(mono) if mono else 0


output = Path(os.environ['MOBU_STORY_TEST_OUTPUT'])
fixtures = Path(os.environ['MOBU_VIDEO_TEST_FIXTURES'])
audio_fixtures = Path(os.environ['MOBU_AUDIO_TEST_FIXTURES'])
keep_open = os.environ.get('MOBU_STORY_TEST_KEEP_OPEN') == '1'
output.mkdir(parents=True, exist_ok=True)
report = {'checks': [], 'started': time.time(), 'status': 'starting',
          'run_id': os.environ['MOBU_VIDEO_TEST_RUN_ID'], 'keep_open': keep_open,
          'pid': os.getpid(),
          'plugin_sha256': hashlib.sha256(Path(os.environ['MOBU_VIDEO_TEST_PLUGIN']).read_bytes()).hexdigest()}
state = {'phase': 0, 'ticks': 0}
system, app, player = FBSystem(), FBApplication(), FBPlayerControl()
manager = FBEvaluateManager()
native = ctypes.CDLL(os.environ['MOBU_VIDEO_TEST_PLUGIN'])
native.MoBuNativeMediaImport.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
native.MoBuNativeMediaImport.restype = ctypes.c_int
native.MoBuNativeVideoLastError.restype = ctypes.c_char_p
native.MoBuNativeMediaLastNotice.restype = ctypes.c_char_p
gl = ctypes.WinDLL('opengl32.dll')
gl.wglGetCurrentContext.restype = ctypes.c_void_p
gl.glIsTexture.argtypes = [ctypes.c_uint]
gl.glIsTexture.restype = ctypes.c_ubyte
gl.glBindTexture.argtypes = [ctypes.c_uint, ctypes.c_uint]
gl.glGetTexImage.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p]
gl.glGetTexLevelParameteriv.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
gl.glGetIntegerv.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
TextureReader = runpy.run_path(str(Path(__file__).resolve().with_name('gl_readback.py')))['TextureReader']
texture_reader = TextureReader(gl)


def save():
    (output / 'story-results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


def check(name, passed, detail=None):
    report['checks'].append({'name': name, 'passed': bool(passed), 'detail': detail})
    save()
    if not passed:
        raise AssertionError(name + ': ' + repr(detail))


def seconds(value):
    result = FBTime()
    result.SetSecondDouble(value)
    return result


def goto(value):
    player.Goto(seconds(value))
    system.Scene.Evaluate()
    state['ticks'] = 0
    state.pop('pixels', None)


def import_media(path, destination=3, audio=True):
    ok = native.MoBuNativeMediaImport(str(path).encode('utf-8'), destination,
        str(output / 'NativeVideoMedia').encode('utf-8'), int(audio))
    check('import_' + str(destination) + '_' + path.name, ok == 1,
          native.MoBuNativeVideoLastError().decode('utf-8'))


def find_track(name, kind):
    return next(t for t in FBStory().RootFolder.Tracks if t.Name.startswith(name) and t.Type == kind)


def pixels(video):
    return texture_reader.read(video.GetTextureID())


def render(sender, event):
    if not gl.wglGetCurrentContext() or 'track' not in state:
        return
    try:
        video = state['track'].TrackVideo
        if video is not None:
            state['pixels'] = pixels(video)
    except Exception:
        report['render_error'] = traceback.format_exc()
        save()


def color(expected):
    actual = state.get('pixels', {}).get('bgra')
    return actual and all(abs(a - b) < 20 for a, b in zip(actual[:3], expected))


def ready():
    return state['ticks'] > 12 and state.get('pixels', {}).get('bgra')


def start_capture(name, frequency=440):
    result = output / (name + '.json')
    result.unlink(missing_ok=True)
    process = subprocess.Popen([os.environ['MOBU_AUDIO_LOOPBACK_PROBE'], str(result), '2', str(frequency)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    line = process.stdout.readline().decode('utf-8').strip()
    if line != 'READY':
        raise RuntimeError('Loopback probe failed: ' + process.stderr.read().decode('utf-8'))
    state.update(capture=process, capture_path=result)
    goto(0)
    player.Play()


def capture_done():
    process = state['capture']
    if process.poll() is None:
        return None
    player.Stop()
    check('loopback_probe_exit', process.returncode == 0, process.stderr.read().decode('utf-8'))
    result = json.loads(state['capture_path'].read_text(encoding='utf-8'))
    report[state['capture_path'].stem] = result
    return result


def prepare_demo():
    video_track = find_track('combined', FBStoryTrackType.kFBStoryTrackVideo)
    audio_track = find_track('combined', FBStoryTrackType.kFBStoryTrackAudio)
    for track in FBStory().RootFolder.Tracks:
        track.Mute = track != video_track and track != audio_track
    FBStory().Mute = False
    texture = FBTexture('NativeStoryDemoOutput')
    texture.Video = video_track.TrackVideo
    pane = system.Renderer.GetSelectedPaneIndex()
    camera = system.Renderer.GetCameraInPane(pane)
    camera.BackGroundTexture = texture
    camera.ViewBackGroundPlaneMode = FBCameraViewPlaneMode.kFBViewPlaneAlways
    player.LoopMode = FBTransportLoopMode.kFBTransportNoLoop
    player.LoopStart = seconds(0)
    player.LoopStop = seconds(1)
    goto(0)
    options = FBFbxOptions(False)
    options.ShowOptionsDialog = False
    demo = output / 'story-demo.fbx'
    saved = app.FileSave(str(demo), options)
    check('demo_scene_saved', saved and demo.is_file(), str(demo))
    report['demo'] = {'scene': str(demo), 'video_track': video_track.Name,
                      'audio_track': audio_track.Name, 'position_seconds': 0,
                      'other_tracks_muted': True}


def finish(error=None):
    if state.get('finished'):
        return
    state['finished'] = True
    cleanup_errors = []
    for event, callback in ((system.OnUIIdle, idle), (manager.OnRenderingPipelineEvent, render)):
        try:
            event.Remove(callback)
        except Exception:
            cleanup_errors.append(traceback.format_exc())
    try:
        player.Stop()
    except Exception:
        cleanup_errors.append(traceback.format_exc())
    process = state.pop('capture', None)
    if process and process.poll() is None:
        try:
            process.terminate()
            process.wait(timeout=5)
        except Exception:
            cleanup_errors.append(traceback.format_exc())
    try:
        source = output / '入力動画.mp4'
        offline = source.with_suffix('.offline')
        if offline.exists() and not source.exists():
            offline.rename(source)
        if keep_open and error is None:
            prepare_demo()
    except Exception:
        error = traceback.format_exc()
    if cleanup_errors:
        report['cleanup_errors'] = cleanup_errors
        error = error or '\n'.join(cleanup_errors)
    state['busy'] = False
    report.update(status='failed' if error else 'passed', error=error,
                  elapsed_seconds=time.time() - report['started'])
    save()
    if not keep_open:
        app.FileExit(False, int(error is not None))


def process_idle(sender, event):
    try:
        app.UpdateAllWidgets()
        state['ticks'] += 1
        if time.time() - report['started'] > 130:
            raise TimeoutError('Story smoke timed out: ' + repr(state.get('pixels')))
        phase = state['phase']
        if state['ticks'] % 20 == 0:
            report['progress'] = {'phase': phase, 'ticks': state['ticks'], 'pixels': state.get('pixels')}
            save()
        if phase == 0:
            for args in [(None, 3, b'', 1), (b'missing.mp4', -1, b'cache', 1),
                         (b'missing.mp4', 3, b'cache', 2)]:
                check('invalid_import_arguments', native.MoBuNativeMediaImport(*args) == 0)
            empty = FBFbxOptions(False)
            empty.ShowOptionsDialog = False
            app.FileSave(str(output / 'story.fbx'), empty)
            source = output / '入力動画.mp4'
            shutil.copyfile(fixtures / 'cfr.mp4', source)
            goto(0)
            import_media(source, audio=False)
            track = find_track('入力動画', FBStoryTrackType.kFBStoryTrackVideo)
            clip = track.Clips[0]
            check('standard_video_clip', isinstance(clip, FBStoryClip) and clip.ImageSequence,
                  {'class': str(type(clip)), 'image_sequence': clip.ImageSequence,
                   'properties': {p.Name: str(p.Data) for p in clip.PropertyList}})
            check('sequence_source_rate', abs(clip.FrameRate - 6) < 1e-8, clip.FrameRate)
            check('standard_source_duration', abs((clip.Stop - clip.Start).GetSecondDouble() - 2) < 1e-5)
            check('metadata_source_relative', not Path(clip.PropertyList.Find('NativeVideo.OriginalSource').Data).is_absolute())
            texture = FBTexture('StoryTrackOutput')
            texture.Video = track.TrackVideo
            camera = FBCamera('NativeStoryCamera')
            camera.Show = True
            camera.BackGroundTexture = texture
            camera.ViewBackGroundPlaneMode = FBCameraViewPlaneMode.kFBViewPlaneAlways
            pane = system.Renderer.GetSelectedPaneIndex()
            system.Renderer.SetCameraSwitcherInPane(pane, False)
            system.Renderer.SetCameraInPane(camera, pane)
            state.update(track=track, clip=clip, texture=texture, camera=camera, phase=1)
            FBStory().Mute = False
            goto(0.5)
        elif phase == 1 and ready():
            check('standard_video_pixels', color((20, 210, 235)), state['pixels'])
            state['clip'].MoveTo(seconds(1), True)
            state['phase'] = 2
            goto(1.5)
        elif phase == 2 and ready():
            check('standard_story_move', color((20, 210, 235)), state['pixels'])
            state['clip'].Speed = 2
            state['phase'] = 3
            goto(1.5)
        elif phase == 3 and ready():
            check('standard_story_speed', color((30, 115, 210)), state['pixels'])
            state['clip'].Speed = 1
            state['clip'].FirstLoopMarkIn = seconds(0.5)
            state['clip'].Stop = seconds(2.5)
            state['phase'] = 4
            goto(1)
        elif phase == 4 and ready():
            check('standard_story_trim', color((20, 210, 235)), state['pixels'])
            state['track'].Mute = True
            state['phase'] = 5
            goto(1.2)
        elif phase == 5 and state['ticks'] > 20:
            sample = state.get('pixels', {})
            check('standard_story_video_mute', not sample.get('gl_id') or sample.get('bgra', [0] * 4)[3] == 0, sample)
            state['track'].Mute = False
            state['phase'] = 6
            goto(1)
        elif phase == 6 and ready():
            check('standard_story_video_unmute', color((20, 210, 235)), state['pixels'])
            goto(0)
            import_media(audio_fixtures / 'stereo.wav', 4)
            audio_track = find_track('stereo', FBStoryTrackType.kFBStoryTrackAudio)
            audio_clip = audio_track.Clips[0]
            check('standard_audio_clip_ready', audio_clip.AudioClip is not None and audio_clip.AudioClip.IsMediaReady())
            check('standard_audio_format', audio_clip.AudioClip.Channels == 2 and audio_clip.AudioClip.Rate == 48000,
                  {'channels': audio_clip.AudioClip.Channels, 'rate': audio_clip.AudioClip.Rate})
            check('audio_wave_duration', abs((audio_clip.Stop - audio_clip.Start).GetSecondDouble() - 1) < 1e-5)
            state.update(audio_track=audio_track, audio_clip=audio_clip)
            state['audio_hashes'] = {'stereo': hashlib.sha256(Path(audio_clip.AudioClip.Path).read_bytes()).hexdigest()}
            for name, bits, reduced in [('pcm24', 24, False), ('float', 24, True)]:
                import_media(audio_fixtures / (name + '.wav'), 4)
                extra = find_track(name, FBStoryTrackType.kFBStoryTrackAudio)
                extra.Mute = True
                clip = extra.Clips[0]
                check(name + '_standard_audio_precision', clip.AudioClip.IsMediaReady() and clip.AudioClip.Bits == bits,
                      {'ready': clip.AudioClip.IsMediaReady(), 'bits': clip.AudioClip.Bits})
                check(name + '_precision_conversion_metadata', clip.PropertyList.Find('NativeVideo.AudioPrecisionReduced').Data == reduced)
                if name == 'pcm24':
                    source_info, source_pcm = pcm_wave(audio_fixtures / 'pcm24.wav')
                    cache_info, cache_pcm = pcm_wave(clip.AudioClip.Path)
                    check('pcm24_cached_samples_unchanged', source_info == cache_info and source_pcm == cache_pcm,
                          {'source': source_info, 'cache': cache_info, 'pcm_sha256': hashlib.sha256(source_pcm).hexdigest()})
                    primary = pcm_tone_amplitude(source_info, source_pcm, PCM24_TONE_FREQUENCY)
                    secondary = pcm_tone_amplitude(source_info, source_pcm, 1733)
                    check('pcm24_source_tone_matches_capture_frequency', 0.12 < primary < 0.13 and 0.00001 < secondary < 0.000015,
                          {'frequency': PCM24_TONE_FREQUENCY, 'amplitude': primary, 'secondary1733': secondary})
                if reduced:
                    check('precision_conversion_notice', '24-bit' in native.MoBuNativeMediaLastNotice().decode('utf-8'))
                state['audio_hashes'][name] = hashlib.sha256(Path(clip.AudioClip.Path).read_bytes()).hexdigest()
            player.LoopMode = FBTransportLoopMode.kFBTransportNoLoop
            player.LoopStop = seconds(5)
            state['phase'] = 7
            start_capture('audio-active')
        elif phase == 7:
            result = capture_done()
            if result is not None:
                check('standard_story_audio_reaches_output', result['tone440'] > 0.003, result)
                state['audio_track'].Mute = True
                find_track('pcm24', FBStoryTrackType.kFBStoryTrackAudio).Mute = False
                state['phase'] = 14
                start_capture('audio-pcm24', PCM24_TONE_FREQUENCY)
        elif phase == 14:
            result = capture_done()
            if result is not None:
                check('standard_story_pcm24_reaches_output', result['toneAmplitude'] > 0.003, result)
                find_track('pcm24', FBStoryTrackType.kFBStoryTrackAudio).Mute = True
                state['phase'] = 8
                start_capture('audio-muted')
        elif phase == 8:
            result = capture_done()
            if result is not None:
                check('standard_story_audio_mute_output', result['tone440'] < report['audio-active']['tone440'] * 0.15 + 0.0001, result)
                state['audio_track'].Mute = False
                state['audio_clip'].MoveTo(seconds(1), True)
                check('standard_audio_move', abs(state['audio_clip'].Start.GetSecondDouble() - 1) < 1e-5)
                state['audio_clip'].Speed = 2
                check('standard_audio_speed', abs(state['audio_clip'].Speed - 2) < 1e-5)
                state['audio_clip'].Speed = 1
                state['audio_clip'].FirstLoopMarkIn = seconds(0.25)
                state['audio_clip'].Stop = seconds(1.75)
                check('standard_audio_trim', abs(state['audio_clip'].FirstLoopMarkIn.GetSecondDouble() - 0.25) < 1e-5)
                state['phase'] = 10
                state['ticks'] = 0
        elif phase == 10 and state['ticks'] > 10:
                report['save_stage'] = 'before save'
                save()
                options = FBFbxOptions(False)
                options.ShowOptionsDialog = False
                app.FileSave(str(output / 'story.fbx'), options)
                report['save_stage'] = 'saved'
                save()
                state['phase'] = 11
                state['ticks'] = 0
        elif phase == 11 and state['ticks'] > 10:
                source = output / '入力動画.mp4'
                source.rename(source.with_suffix('.offline'))
                state.pop('track', None)
                state['phase'] = -1
                app.FileNew()
                report['save_stage'] = 'file new'
                save()
                state['phase'] = 12
                state['ticks'] = 0
        elif phase == 12 and state['ticks'] > 10:
                options = FBFbxOptions(True)
                options.ShowOptionsDialog = False
                state['phase'] = -1
                app.FileOpen(str(output / 'story.fbx'), False, options)
                report['save_stage'] = 'file opened'
                save()
                state['phase'] = 13
                state['ticks'] = 0
        elif phase == 13 and state['ticks'] > 10:
                track = find_track('入力動画', FBStoryTrackType.kFBStoryTrackVideo)
                state.update(track=track, clip=track.Clips[0], phase=9)
                audio_track = find_track('stereo', FBStoryTrackType.kFBStoryTrackAudio)
                check('audio_fbx_reopen', audio_track.Clips[0].AudioClip.IsMediaReady())
                check('audio_fbx_edit_retained', abs(audio_track.Clips[0].FirstLoopMarkIn.GetSecondDouble() - 0.25) < 1e-5)
                audio_paths = []
                for name, digest in state['audio_hashes'].items():
                    clip = find_track(name, FBStoryTrackType.kFBStoryTrackAudio).Clips[0]
                    path = Path(clip.AudioClip.Path)
                    audio_paths.append(str(path))
                    check(name + '_fbx_audio_content', clip.AudioClip.IsMediaReady() and
                          hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path))
                check('fbx_audio_paths_remain_distinct', len(set(audio_paths)) == len(audio_paths), audio_paths)
                goto(1)
        elif phase == 9 and ready():
            check('video_fbx_reopen_without_original', color((20, 210, 235)), state['pixels'])
            goto(0)
            import_media(audio_fixtures / 'combined.mp4', 3)
            video_track = find_track('combined', FBStoryTrackType.kFBStoryTrackVideo)
            audio_track = find_track('combined', FBStoryTrackType.kFBStoryTrackAudio)
            check('embedded_audio_video_same_start', abs((video_track.Clips[0].Start - audio_track.Clips[0].Start).GetSecondDouble()) < 1e-8)
            check('embedded_audio_priming_trimmed', abs((audio_track.Clips[0].Stop - audio_track.Clips[0].Start).GetSecondDouble() - 1) < 1e-5)
            check('embedded_audio_standard_ready', audio_track.Clips[0].AudioClip.IsMediaReady())
            check('video_ends_before_audio_padding', abs((video_track.Clips[0].Stop - video_track.Clips[0].Start).GetSecondDouble() - 1) < 1e-5)
            import_media(audio_fixtures / 'video-delayed.mp4', 3)
            video_track = find_track('video-delayed', FBStoryTrackType.kFBStoryTrackVideo)
            audio_track = find_track('video-delayed', FBStoryTrackType.kFBStoryTrackAudio)
            check('movie_preserves_video_delay', abs(video_track.Clips[0].Start.GetSecondDouble() - 0.5) < 1e-5)
            check('movie_preserves_early_audio', abs(audio_track.Clips[0].Start.GetSecondDouble()) < 1e-5 and
                  abs((audio_track.Clips[0].Stop - audio_track.Clips[0].Start).GetSecondDouble() - 1) < 1e-5)
            check('delayed_video_own_duration', abs((video_track.Clips[0].Stop - video_track.Clips[0].Start).GetSecondDouble() - 1) < 1e-5)
            tracks_before = len(FBStory().RootFolder.Tracks)
            check('audio_only_rejects_silent_video', native.MoBuNativeMediaImport(
                str(fixtures / 'cfr.mp4').encode('utf-8'), 4,
                str(output / 'NativeVideoMedia').encode('utf-8'), 1) == 0)
            check('failed_audio_import_leaves_no_tracks', len(FBStory().RootFolder.Tracks) == tracks_before)
            camera = system.Renderer.GetCameraInPane(system.Renderer.GetSelectedPaneIndex())
            previous = camera.BackGroundTexture
            previous_mode = camera.ViewBackGroundPlaneMode
            tracks_before = len(FBStory().RootFolder.Tracks)
            check('unsupported_story_audio_import_fails', native.MoBuNativeMediaImport(
                str(audio_fixtures / 'surround-video.mp4').encode('utf-8'), 0,
                str(output / 'NativeVideoMedia').encode('utf-8'), 1) == 0)
            check('failed_av_import_restores_camera', camera.BackGroundTexture == previous and
                  camera.ViewBackGroundPlaneMode == previous_mode)
            check('failed_av_import_removes_created_tracks', len(FBStory().RootFolder.Tracks) == tracks_before)
            finish()
    except Exception:
        finish(traceback.format_exc())


def idle(sender, event):
    if state.get('busy') or state.get('finished'):
        return
    state['busy'] = True
    try:
        process_idle(sender, event)
    finally:
        state['busy'] = False


system.OnUIIdle.Add(idle)
manager.OnRenderingPipelineEvent.Add(render)
