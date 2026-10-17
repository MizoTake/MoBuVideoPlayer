"""Probe FBVideoMemory consumption through a standard Story video track."""

import ctypes
import json
import os
from pathlib import Path
import time
import traceback

from pyfbsdk import *


output = Path(os.environ['MOBU_STORY_PROBE_OUTPUT'])
output.mkdir(parents=True, exist_ok=True)
report = {'started': time.time(), 'steps': []}
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
gl.glGetIntegerv.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
native = ctypes.CDLL(os.environ['MOBU_STORY_PROBE_PLUGIN'])
native.MoBuNativeVideoImport.argtypes = [ctypes.c_char_p, ctypes.c_int]
native.MoBuNativeVideoImport.restype = ctypes.c_int
native.MoBuNativeVideoLastError.restype = ctypes.c_char_p


def save(step, **values):
    report['steps'].append({'step': step, **values})
    (output / 'results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


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
    texture_id = video.TextureOGLId if hasattr(video, 'TextureOGLId') else video.GetTextureID()
    result = {'gl_id': texture_id}
    if texture_id and gl.glIsTexture(texture_id):
        prior = ctypes.c_int()
        gl.glGetIntegerv(0x8069, ctypes.byref(prior))
        gl.glBindTexture(0x0DE1, texture_id)
        data = (ctypes.c_ubyte * (194 * 130 * 4))()
        gl.glGetTexImage(0x0DE1, 0, 0x80E1, 0x1401, data)
        gl.glBindTexture(0x0DE1, prior.value)
        center = (65 * 194 + 97) * 4
        result['bgra'] = list(data[center:center + 4])
    return result


def finish(error=None):
    system.OnUIIdle.Remove(idle)
    manager.OnRenderingPipelineEvent.Remove(render)
    report['status'] = 'failed' if error else 'finished'
    save('finish', error=error)
    app.FileExit(False, int(error is not None))


def render(sender, event):
    if not gl.wglGetCurrentContext() or state['phase'] < 2:
        return
    try:
        state['pixels'] = {'source': pixels(state['source'].Video),
                           'track': pixels(state['track'].TrackVideo),
                           'time': system.LocalTime.GetSecondDouble()}
    except Exception:
        save('render_exception', error=traceback.format_exc())


def idle(sender, event):
    try:
        app.UpdateAllWidgets()
        state['ticks'] += 1
        if time.time() - report['started'] > 60:
            finish('timeout')
            return
        if state['phase'] == 0:
            imported = native.MoBuNativeVideoImport(os.environ['MOBU_STORY_PROBE_SOURCE'].encode('utf-8'), 2)
            if imported != 1:
                raise RuntimeError(native.MoBuNativeVideoLastError())
            state['source'] = next(t for t in system.Scene.Textures if t.PropertyList.Find('NativeVideo.Source'))
            player.Goto(seconds(0.5))
            state['phase'] = 1
            state['ticks'] = 0
        elif state['phase'] == 1 and state['ticks'] > 10 and state['source'].Video:
            video = state['source'].Video
            video.PropertyList.Find('FrameRate').Data = 6.0
            video.PropertyList.Find('StopFrame').Data = 11
            track = FBStoryTrack(FBStoryTrackType.kFBStoryTrackVideo)
            track.Name = 'NativeMemoryTrack'
            clip = FBStoryClip(video, track, seconds(1.0))
            clip.Stop = seconds(3.0)
            output_texture = FBTexture('NativeMemoryTrackOutput')
            output_texture.Video = track.TrackVideo
            camera = FBCamera('StoryMemoryCamera')
            camera.BackGroundTexture = output_texture
            camera.ViewShowGrid = False
            camera.Show = True
            pane = system.Renderer.GetSelectedPaneIndex()
            system.Renderer.SetCameraSwitcherInPane(pane, False)
            system.Renderer.SetCameraInPane(camera, pane)
            state.update(track=track, clip=clip, output=output_texture, camera=camera, phase=2, ticks=0)
            save('story_created', clips=len(track.Clips), source=describe(video), output=describe(track.TrackVideo),
                 clip_properties={p.Name: str(p.Data) for p in clip.PropertyList},
                 source_connections=[{'type': str(type(clip.GetSrc(i))), 'name': str(clip.GetSrc(i).Name)} for i in range(clip.GetSrcCount())])
            FBStory().Mute = False
            player.Goto(seconds(1.5))
        elif state['phase'] == 2 and state['ticks'] > 20:
            save('story_pixels', pixels=state.get('pixels'), output=describe(state['track'].TrackVideo),
                 source_time=state['clip'].GetSourceTimeFromDestinationTime(seconds(1.5))[0].GetSecondDouble())
            file_name = str(output / 'memory.fbx')
            options = FBFbxOptions(False)
            options.ShowOptionsDialog = False
            save('before_save')
            app.FileSave(file_name, options)
            save('saved', exists=Path(file_name).is_file())
            finish()
    except Exception:
        finish(traceback.format_exc())


system.OnUIIdle.Add(idle)
manager.OnRenderingPipelineEvent.Add(render)
