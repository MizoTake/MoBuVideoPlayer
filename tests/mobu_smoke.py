"""Run in a new, dedicated MotionBuilder process, not in an artist's session."""

import ctypes
import json
import os
from pathlib import Path
import time
import traceback

from pyfbsdk import *


output = Path(os.environ['MOBU_VIDEO_TEST_OUTPUT'])
fixtures = Path(os.environ['MOBU_VIDEO_TEST_FIXTURES'])
output.mkdir(parents=True, exist_ok=True)
report = {'checks': [], 'status': 'starting', 'started': time.time(),
          'run_id': os.environ.get('MOBU_VIDEO_TEST_RUN_ID')}
state = {'phase': 0, 'ticks': 0, 'capture': None, 'texture': None, 'target': 0.0}
system = FBSystem()
application = FBApplication()
player = FBPlayerControl()
manager = FBEvaluateManager()
gl = ctypes.WinDLL('opengl32.dll')
gl.wglGetCurrentContext.restype = ctypes.c_void_p
gl.glGetTexImage.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p]
gl.glGetIntegerv.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
gl.glBindTexture.argtypes = [ctypes.c_uint, ctypes.c_uint]
gl.glIsTexture.argtypes = [ctypes.c_uint]
gl.glIsTexture.restype = ctypes.c_ubyte
native_plugin = ctypes.CDLL(os.environ['MOBU_VIDEO_TEST_PLUGIN'])


def save_report():
    (output / 'host-results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


def check(name, passed, detail=None):
    report['checks'].append({'name': name, 'passed': bool(passed), 'detail': detail})
    save_report()
    if not passed:
        raise AssertionError(name + ': ' + repr(detail))


def prop(texture, name, kind, data_type, value):
    item = texture.PropertyCreate(name, kind, data_type, False, True, None)
    item.Data = value


def goto(seconds):
    state['target'] = seconds
    state['capture'] = None
    state['ticks'] = 0
    target = FBTime()
    target.SetSecondDouble(seconds)
    player.Goto(target)
    system.Scene.Evaluate()


def apply_target(index):
    seconds, expected, overrides = targets[index]
    texture = state['texture']
    defaults = {'StartFrame': 0, 'StopFrame': -1, 'FrameRate': 0.0,
                'PlaySpeed': 1.0, 'Loop': False, 'TimeOffset': 0.0}
    defaults.update(overrides)
    for name, value in defaults.items():
        item = texture.PropertyList.Find('NativeVideo.' + name)
        if item:
            if name == 'TimeOffset':
                offset = FBTime()
                offset.SetSecondDouble(value)
                value = offset
            item.Data = value
    goto(seconds)


def finish(error=None):
    system.OnUIIdle.Remove(idle)
    manager.OnRenderingPipelineEvent.Remove(render)
    player.Stop()
    report['status'] = 'failed' if error else 'passed'
    if error:
        report['error'] = error
    report['elapsed_seconds'] = time.time() - report['started']
    save_report()
    application.FileExit(False, 1 if error else 0)


def render(sender, event):
    try:
        report['render_calls'] = report.get('render_calls', 0) + 1
        texture = state['texture']
        if texture is None or not gl.wglGetCurrentContext():
            return
        report['render_with_context'] = report.get('render_with_context', 0) + 1
        video = texture.Video
        report['video_type'] = str(type(video))
        if not video or not hasattr(video, 'TextureOGLId'):
            return
        texture_id = video.TextureOGLId
        if not texture_id or not gl.glIsTexture(texture_id):
            return
        width = texture.PropertyList.Find('NativeVideo.Width')
        height = texture.PropertyList.Find('NativeVideo.Height')
        if width and height and width.Data and height.Data:
            width, height = int(width.Data), int(height.Data)
            state['dimensions'] = (width, height)
        elif state.get('dimensions'):
            width, height = state['dimensions']
        else:
            return
        previous = ctypes.c_int()
        gl.glGetIntegerv(0x8069, ctypes.byref(previous))
        gl.glBindTexture(0x0DE1, texture_id)
        pixels = (ctypes.c_ubyte * (width * height * 4))()
        gl.glGetTexImage(0x0DE1, 0, 0x80E1, 0x1401, pixels)
        gl.glBindTexture(0x0DE1, previous.value)
        center = (height // 2 * width + width // 2) * 4
        state['capture'] = {'time': system.LocalTime.GetSecondDouble(),
                            'bgra': list(pixels[center:center + 4]),
                            'upper_left_bgra': list(pixels[((height - 1 - 3) * width + 3) * 4:
                                                          ((height - 1 - 3) * width + 3) * 4 + 4]),
                            'lower_left_bgra': list(pixels[(3 * width + 3) * 4:(3 * width + 3) * 4 + 4]),
                            'width': width, 'height': height, 'gl_id': texture_id}
    except Exception:
        report['render_error'] = traceback.format_exc()
        save_report()


targets = [(0.0, (16, 16, 240), {}), (0.5, (20, 210, 235), {}),
           (1.2, (205, 30, 125), {}), (1 / 6, (24, 220, 16), {}),
           (2.5, (20, 210, 235), {'Loop': True}),
           (0.5, (210, 130, 40), {'PlaySpeed': -1.0}),
           (0.5, (16, 16, 240), {'PlaySpeed': 0.0}),
           (0.5, (20, 210, 235), {'StartFrame': 3, 'StopFrame': 5, 'TimeOffset': 1.0}),
           (1.2, (220, 20, 210), {'StartFrame': 3, 'StopFrame': 5, 'TimeOffset': 1.0})]


def idle(sender, event):
    try:
        application.UpdateAllWidgets()
        state['ticks'] += 1
        if state['ticks'] % 20 == 0:
            texture = state['texture']
            report['progress'] = {'phase': state['phase'], 'ticks': state['ticks'],
                                  'capture': state['capture'],
                                  'properties': {p.Name: str(p.Data) for p in texture.PropertyList
                                                 if p.Name.startswith('NativeVideo.')} if texture else {}}
            save_report()
        if time.time() - report['started'] > 100:
            raise TimeoutError('MotionBuilder smoke test timed out: ' + repr(state['capture']))
        phase = state['phase']
        if phase == 0:
            names = list(system.GetLoadedPluginItemsName())
            check('native_plugin_loaded', 'Native Video' in names, names)
            native_plugin.MoBuNativeVideoImport.argtypes = [ctypes.c_char_p, ctypes.c_int]
            native_plugin.MoBuNativeVideoImport.restype = ctypes.c_int
            native_plugin.MoBuNativeVideoLastError.restype = ctypes.c_char_p
            report['plugin_info'] = list(system.GetLoadedPluginItemInfo('Native Video'))
            camera = FBCamera('NativeVideoSmokeCamera')
            camera.Show = True
            pane = system.Renderer.GetSelectedPaneIndex()
            system.Renderer.SetCameraSwitcherInPane(pane, False)
            system.Renderer.SetCameraInPane(camera, pane)
            check('selected_viewport_camera', system.Renderer.GetCameraInPane(pane) == camera,
                  {'pane': pane, 'camera': system.Renderer.GetCameraInPane(pane).Name})
            check('import_api_background', native_plugin.MoBuNativeVideoImport(str(fixtures / 'cfr.mp4').encode('utf-8'), 0) == 1,
                  str(native_plugin.MoBuNativeVideoLastError()))
            texture = camera.BackGroundTexture
            check('import_api_connects_normal_camera', texture is not None and
                  texture.PropertyList.Find('NativeVideo.Source') is not None)
            texture.Name = 'NativeVideoSmoke'
            state['texture'] = texture
            switcher = FBCameraSwitcher()
            system.Renderer.SetCameraSwitcherInPane(pane, True)
            switcher.CurrentCamera = camera
            switcher_camera = system.Renderer.GetCameraInPane(pane)
            check('selected_switcher_camera', system.Renderer.IsCameraSwitcherInPane(pane) and
                  switcher_camera is not None, switcher_camera.Name if switcher_camera else None)
            previous_background = switcher_camera.BackGroundTexture
            check('import_api_camera_switcher', native_plugin.MoBuNativeVideoImport(
                  str(fixtures / 'cfr.mp4').encode('utf-8'), 0) == 1 and
                  switcher_camera.BackGroundTexture is not None and
                  switcher_camera.BackGroundTexture != previous_background)
            extra_switcher = switcher_camera.BackGroundTexture
            if previous_background is not None:
                switcher_camera.BackGroundTexture = previous_background
            extra_switcher.FBDelete()
            system.Renderer.SetCameraSwitcherInPane(pane, False)
            system.Renderer.SetCameraInPane(camera, pane)
            check('import_api_foreground', native_plugin.MoBuNativeVideoImport(str(fixtures / 'cfr.mp4').encode('utf-8'), 1) == 1)
            extra_foreground = camera.ForeGroundTexture
            check('import_api_connects_foreground', extra_foreground is not None and extra_foreground != texture)
            extra_foreground.FBDelete()
            before = list(system.Scene.Textures)
            check('import_api_texture_only', native_plugin.MoBuNativeVideoImport(
                  str(fixtures / '動画_テスト_ñ_日本語.mp4').encode('utf-8'), 2) == 1)
            extras = [item for item in system.Scene.Textures if item not in before]
            check('import_api_unicode_path', len(extras) == 1 and
                  '日本語' in extras[0].PropertyList.Find('NativeVideo.Source').Data)
            extras[0].FBDelete()
            check('import_api_rejects_invalid_destination', native_plugin.MoBuNativeVideoImport(
                  str(fixtures / 'cfr.mp4').encode('utf-8'), 99) == 0)
            check('import_api_rejects_missing_file', native_plugin.MoBuNativeVideoImport(
                  str(output / 'does-not-exist.mp4').encode('utf-8'), 2) == 0 and
                  bool(native_plugin.MoBuNativeVideoLastError()))
            camera.ForeGroundTexture = texture
            camera.ViewForeGroundPlaneMode = FBCameraViewPlaneMode.kFBViewPlaneAlways
            material = FBMaterial('NativeVideoSmokeMaterial')
            material.SetTexture(texture, FBMaterialTextureType.kFBMaterialTextureDiffuse)
            check('standard_foreground_and_material_connections', camera.ForeGroundTexture == texture and
                  material.GetTexture(FBMaterialTextureType.kFBMaterialTextureDiffuse) == texture)
            state['phase'] = 1
            apply_target(0)
        elif 1 <= phase <= len(targets):
            capture = state['capture']
            if state['ticks'] < 3 or capture is None:
                return
            seconds, expected, overrides = targets[phase - 1]
            if abs(capture['time'] - seconds) > 0.001:
                return
            check('timeline_texture_pixels_' + str(seconds),
                  all(abs(a - b) <= 15 for a, b in zip(capture['bgra'][:3], expected)), capture)
            if phase == 1:
                check('gpu_texture_top_left_orientation',
                      min(capture['upper_left_bgra'][:3]) > 220 and
                      min(capture['lower_left_bgra'][:3]) < 100, capture)
            state['phase'] += 1
            if phase < len(targets):
                apply_target(phase)
            else:
                scene_file = output / 'native-video-smoke.fbx'
                state['texture'].PropertyList.Find('NativeVideo.Source').Data = os.path.relpath(fixtures / 'cfr.mp4', output)
                check('fbx_save', application.FileSave(str(scene_file)))
                state['texture'] = None
                application.FileNew(False)
                check('fbx_reopen', application.FileOpen(str(scene_file), False))
                matches = [texture for texture in system.Scene.Textures if texture.Name == 'NativeVideoSmoke']
                check('metadata_persisted', len(matches) == 1 and
                      matches[0].PropertyList.Find('NativeVideo.Source').Data.endswith('cfr.mp4'))
                state['texture'] = matches[0]
                goto(0.5)
        elif phase == len(targets) + 1:
            capture = state['capture']
            if state['ticks'] < 4 or capture is None or abs(capture['time'] - 0.5) > 0.001:
                return
            check('reopened_texture_pixels',
                  all(abs(a - b) <= 15 for a, b in zip(capture['bgra'][:3], targets[1][1])), capture)
            check('camera_background_connection_persisted',
                  any(camera.BackGroundTexture == state['texture'] for camera in system.Scene.Cameras))
            check('foreground_material_connections_persisted',
                  any(camera.ForeGroundTexture == state['texture'] for camera in system.Scene.Cameras) and
                  any(material.GetTexture(FBMaterialTextureType.kFBMaterialTextureDiffuse) == state['texture']
                      for material in system.Scene.Materials))
            state['phase'] += 1
            apply_target(0)
            player.Play()
        elif phase == len(targets) + 2:
            if system.LocalTime.GetSecondDouble() < 0.5:
                return
            player.Stop()
            check('transport_play_advances_time', system.LocalTime.GetSecondDouble() >= 0.5)
            state['stopped_at'] = system.LocalTime.GetSecondDouble()
            state['stop_settle_deadline'] = time.time() + 2.0
            state['ticks'] = 0
            state['capture'] = None
            state['phase'] += 1
        elif phase == len(targets) + 3:
            current = system.LocalTime.GetSecondDouble()
            if abs(current - state['stopped_at']) > 0.001:
                if time.time() > state['stop_settle_deadline']:
                    raise AssertionError('Transport kept advancing after Stop')
                state['stopped_at'] = current
                state['ticks'] = 0
                state['capture'] = None
                return
            if state['ticks'] < 4 or state['capture'] is None:
                return
            check('transport_stop_holds_time', abs(system.LocalTime.GetSecondDouble() - state['stopped_at']) < 0.001)
            palette = [(16, 16, 240), (24, 220, 16), (240, 32, 20), (20, 210, 235),
                       (220, 20, 210), (210, 215, 20), (30, 115, 210), (205, 30, 125),
                       (210, 130, 40), (110, 60, 210), (40, 210, 100), (180, 180, 180)]
            expected = palette[min(11, int(state['stopped_at'] * 6))]
            check('playing_texture_matches_transport', all(abs(a - b) <= 15 for a, b in
                  zip(state['capture']['bgra'][:3], expected)), state['capture'])
            state['source'] = state['texture'].PropertyList.Find('NativeVideo.Source').Data
            state['texture'].PropertyList.Find('NativeVideo.Source').Data = str(output / 'missing-input.mp4')
            state['phase'] += 1
        elif phase == len(targets) + 4:
            status = state['texture'].PropertyList.Find('NativeVideo.Status').Data
            if not status.startswith('Error:'):
                return
            check('missing_source_reports_error', True, status)
            state['texture'].PropertyList.Find('NativeVideo.Source').Data = state['source']
            state['texture'].PropertyList.Find('NativeVideo.Retry').Data = True
            state['capture'] = None
            state['ticks'] = 0
            state['phase'] += 1
        elif phase == len(targets) + 5:
            status = state['texture'].PropertyList.Find('NativeVideo.Status').Data
            if state['ticks'] < 4 or not status.startswith('Ready') or state['capture'] is None:
                return
            check('restoring_source_recovers', True, status)
            shared = FBTexture('NativeVideoSharedTexture')
            shared.Video = state['texture'].Video
            state['texture'].FBDelete()
            state['texture'] = shared
            state['phase'] += 1
            goto(1.0)
        elif phase == len(targets) + 6:
            if state['ticks'] < 4 or state['capture'] is None or abs(state['capture']['time'] - 1.0) > 0.001:
                return
            check('shared_video_keeps_playing_after_owner_deleted', all(abs(a - b) <= 15 for a, b in
                  zip(state['capture']['bgra'][:3], (30, 115, 210))), state['capture'])
            state['texture'].FBDelete()
            state['texture'] = None
            state['phase'] += 1
            state['ticks'] = 0
        elif phase == len(targets) + 7:
            if state['ticks'] < 4:
                return
            runtimes = []
            for video in system.Scene.VideoClips:
                if video.PropertyList.Find('NativeVideo.Runtime'):
                    destinations = []
                    for index in range(video.GetDstCount()):
                        destination = video.GetDst(index)
                        owner = destination.GetOwner() if hasattr(destination, 'GetOwner') else None
                        destinations.append({'class': destination.ClassName(), 'name': str(getattr(destination, 'Name', '')),
                                             'owner_class': owner.ClassName() if owner else None})
                    runtimes.append({'name': video.Name, 'destinations': destinations})
            check('deleting_texture_releases_runtime_video', len(runtimes) == 0, runtimes)
            bad_source = FBTexture('NativeVideoBadSource')
            prop(bad_source, 'NativeVideo.Version', FBPropertyType.kFBPT_int, 'Integer', 1)
            prop(bad_source, 'NativeVideo.Source', FBPropertyType.kFBPT_int, 'Integer', 123)
            bad_version = FBTexture('NativeVideoBadVersion')
            prop(bad_version, 'NativeVideo.Version', FBPropertyType.kFBPT_charptr, 'String', '1')
            prop(bad_version, 'NativeVideo.Source', FBPropertyType.kFBPT_charptr, 'String', str(fixtures / 'cfr.mp4'))
            state['bad_source'] = bad_source
            state['bad_version'] = bad_version
            state['phase'] += 1
            state['ticks'] = 0
        elif phase == len(targets) + 8:
            if state['ticks'] < 4:
                return
            status = state['bad_source'].PropertyList.Find('NativeVideo.Status')
            check('wrong_source_type_reports_error', status is not None and 'String property' in status.Data,
                  status.Data if status else None)
            check('wrong_version_type_is_ignored', state['bad_version'].PropertyList.Find('NativeVideo.Status') is None)
            state['bad_source'].FBDelete()
            state['bad_version'].FBDelete()
            finish()
    except Exception:
        finish(traceback.format_exc())


save_report()
manager.OnRenderingPipelineEvent.Add(render)
system.OnUIIdle.Add(idle)
