"""Probe the standard Story image-sequence import and editing path."""

import ctypes
import json
import os
from pathlib import Path
import time
import traceback

from pyfbsdk import *


output = Path(os.environ['MOBU_STORY_PROBE_OUTPUT'])
output.mkdir(parents=True, exist_ok=True)
report = {'started': time.time(), 'checks': []}
system = FBSystem()
app = FBApplication()
player = FBPlayerControl()
manager = FBEvaluateManager()
state = {'phase': 0, 'ticks': 0}
gl = ctypes.WinDLL('opengl32.dll')
gl.wglGetCurrentContext.restype = ctypes.c_void_p
gl.glIsTexture.argtypes = [ctypes.c_uint]
gl.glIsTexture.restype = ctypes.c_ubyte
gl.glBindTexture.argtypes = [ctypes.c_uint, ctypes.c_uint]
gl.glGetTexImage.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p]
gl.glGetTexLevelParameteriv.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
gl.glGetIntegerv.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]


def save():
    (output / 'results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


def check(name, passed, detail=None):
    report['checks'].append({'name': name, 'passed': bool(passed), 'detail': detail})
    save()
    if not passed:
        raise AssertionError(name + ': ' + repr(detail))


def seconds(value):
    result = FBTime()
    result.SetSecondDouble(value)
    return result


def describe(video):
    if video is None:
        return None
    return {'type': str(type(video)), 'name': video.Name,
            'props': {p.Name: str(p.Data) for p in video.PropertyList}}


def pixels(video):
    texture_id = video.GetTextureID()
    result = {'gl_id': texture_id}
    if texture_id and gl.glIsTexture(texture_id):
        prior = ctypes.c_int()
        width, height = ctypes.c_int(), ctypes.c_int()
        gl.glGetIntegerv(0x8069, ctypes.byref(prior))
        gl.glBindTexture(0x0DE1, texture_id)
        gl.glGetTexLevelParameteriv(0x0DE1, 0, 0x1000, ctypes.byref(width))
        gl.glGetTexLevelParameteriv(0x0DE1, 0, 0x1001, ctypes.byref(height))
        result.update(width=width.value, height=height.value)
        if 0 < width.value < 4096 and 0 < height.value < 4096:
            data = (ctypes.c_ubyte * (width.value * height.value * 4))()
            gl.glGetTexImage(0x0DE1, 0, 0x80E1, 0x1401, data)
            center = (height.value // 2 * width.value + width.value // 2) * 4
            result['bgra'] = list(data[center:center + 4])
        gl.glBindTexture(0x0DE1, prior.value)
    return result


def finish(error=None):
    system.OnUIIdle.Remove(idle)
    manager.OnRenderingPipelineEvent.Remove(render)
    player.Stop()
    report['status'] = 'failed' if error else 'passed'
    report['error'] = error
    report['elapsed_seconds'] = time.time() - report['started']
    save()
    app.FileExit(False, int(error is not None))


def render(sender, event):
    if not gl.wglGetCurrentContext() or state['phase'] < 1:
        return
    try:
        state['pixels'] = {'source': pixels(state['source']),
                           'track': pixels(state['track'].TrackVideo),
                           'time': system.LocalTime.GetSecondDouble()}
    except Exception:
        report['render_error'] = traceback.format_exc()
        save()


def close_color(actual, expected):
    return actual and all(abs(a - b) < 20 for a, b in zip(actual[:3], expected))


def wait_for_pixels():
    return state['ticks'] > 12 and state.get('pixels', {}).get('track', {}).get('bgra')


def goto(value):
    player.Goto(seconds(value))
    system.Scene.Evaluate()
    state['ticks'] = 0
    state.pop('pixels', None)


def idle(sender, event):
    if state.get('busy'):
        return
    state['busy'] = True
    try:
        app.UpdateAllWidgets()
        state['ticks'] += 1
        if time.time() - report['started'] > 75:
            finish('timeout: ' + repr(state.get('pixels')))
            return
        phase = state['phase']
        if phase == 0:
            track = FBStoryTrack(FBStoryTrackType.kFBStoryTrackVideo)
            track.Name = 'SequenceVideoProbe'
            source = FBVideoClip(os.environ['MOBU_STORY_PROBE_SEQUENCE'])
            source.PropertyList.Find('UseSystemFrameRate').Data = False
            source.PropertyList.Find('FrameRate').Data = 6.0
            source.PropertyList.Find('Offset').Data = FBTime(0)
            clip = FBStoryClip(source, track, FBTime(0))
            clip.ImageSequence = True
            clip.UseSystemFrameRate = False
            clip.FrameRate = 6
            clip.FirstLoopMarkIn = FBTime(0)
            clip.LastLoopMarkOut = seconds(2.0)
            clip.Stop = seconds(2.0)
            texture = FBTexture('SequenceTrackOutput')
            texture.Video = track.TrackVideo
            camera = FBCamera('SequenceStoryCamera')
            camera.BackGroundTexture = texture
            camera.Show = True
            pane = system.Renderer.GetSelectedPaneIndex()
            system.Renderer.SetCameraSwitcherInPane(pane, False)
            system.Renderer.SetCameraInPane(camera, pane)
            state.update(track=track, source=source, clip=clip, texture=texture, camera=camera, phase=1)
            FBStory().Mute = False
            report['initial'] = {'source': describe(source), 'track': describe(track.TrackVideo),
                                 'clip': {p.Name: str(p.Data) for p in clip.PropertyList}}
            check('sequence_story_clip_created', len(track.Clips) == 1, len(track.Clips))
            goto(0.5)
        elif phase == 1 and wait_for_pixels():
            report['loaded'] = {'source': describe(state['source']), 'track': describe(state['track'].TrackVideo),
                                'clip': {p.Name: str(p.Data) for p in state['clip'].PropertyList}}
            check('sequence_story_pixels', close_color(state['pixels']['track']['bgra'], (20, 210, 235)), state['pixels'])
            clip = state['clip']
            clip.MoveTo(seconds(1.0), True)
            state['phase'] = 2
            goto(1.5)
        elif phase == 2 and wait_for_pixels():
            check('sequence_story_move', close_color(state['pixels']['track']['bgra'], (20, 210, 235)), state['pixels'])
            state['clip'].Speed = 2.0
            state['phase'] = 3
            goto(1.5)
        elif phase == 3 and wait_for_pixels():
            check('sequence_story_speed', close_color(state['pixels']['track']['bgra'], (30, 115, 210)), state['pixels'])
            state['track'].Mute = True
            check('sequence_story_mute_property', state['track'].Mute)
            state['track'].Mute = False
            state['clip'].Speed = 1.0
            path = str(output / 'sequence.fbx')
            options = FBFbxOptions(False)
            options.ShowOptionsDialog = False
            app.FileSave(path, options)
            check('sequence_story_save', Path(path).is_file())
            state['phase'] = -1
            report['progress'] = 'before_file_new'
            save()
            app.FileNew()
            options = FBFbxOptions(True)
            options.ShowOptionsDialog = False
            report['progress'] = 'before_file_open'
            save()
            app.FileOpen(path, False, options)
            report['progress'] = 'after_file_open'
            save()
            track = next(t for t in FBStory().RootFolder.Tracks if t.Name == 'SequenceVideoProbe')
            state.update(track=track, clip=track.Clips[0], phase=4)
            state['source'] = track.TrackVideo
            goto(1.5)
        elif phase == 4 and wait_for_pixels():
            check('sequence_story_reopen', close_color(state['pixels']['track']['bgra'], (20, 210, 235)), state['pixels'])
            clip = state['clip']
            clip.FirstLoopMarkIn = seconds(0.5)
            clip.Start = seconds(1.0)
            clip.Stop = seconds(2.5)
            state['phase'] = 5
            goto(1.0)
        elif phase == 5 and wait_for_pixels():
            check('sequence_story_trim', close_color(state['pixels']['track']['bgra'], (20, 210, 235)), state['pixels'])
            state['track'].Mute = True
            state['phase'] = 6
            goto(1.2)
        elif phase == 6 and state['ticks'] > 20:
            sample = state.get('pixels', {}).get('track', {})
            check('sequence_story_mute_pixels', not sample.get('gl_id') or
                  sample.get('bgra', [None] * 4)[3] == 0, state.get('pixels'))
            state['track'].Mute = False
            state['phase'] = 7
            goto(1.0)
        elif phase == 7 and wait_for_pixels():
            check('sequence_story_unmute_pixels', close_color(state['pixels']['track']['bgra'], (20, 210, 235)), state['pixels'])
            finish()
    except Exception:
        finish(traceback.format_exc())
    finally:
        state['busy'] = False


system.OnUIIdle.Add(idle)
manager.OnRenderingPipelineEvent.Add(render)
