"""Run command-driven Story checks inside an existing, stopped MotionBuilder scene."""

import builtins
import hashlib
import json
from pathlib import Path
import re
import time
import traceback

from pyfbsdk import *


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'artifacts' / 'reuse-host'
OUTPUT.mkdir(parents=True, exist_ok=True)
COMMAND = OUTPUT / 'command.json'
RESULT = OUTPUT / 'reuse-results.json'
NAMES = ('stereo', 'pcm24', 'float')
ACTIONS = ('play', 'mute', 'save', 'append', 'finish')
MARKER = '_native_video_reuse_checks_active'


def seconds(value):
    result = FBTime()
    result.SetSecondDouble(value)
    return result


def copy_time(value):
    return FBTime(value.Get())


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def current_cache_revision(source_root=ROOT):
    source = source_root / 'src' / 'media' / 'MediaCache.cpp'
    revisions = re.findall(r'^\s*constexpr\s+unsigned\s+CacheRevision\s*=\s*(\d+)\s*;',
                           source.read_text(encoding='utf-8'), re.MULTILINE)
    if len(revisions) != 1 or int(revisions[0]) <= 0:
        raise RuntimeError('Cannot determine the unique media cache revision from MediaCache.cpp')
    return int(revisions[0])


def prepared_audio_cache(cache_root, source, revision):
    source_hash = digest(source)
    directory = cache_root / ('v' + str(revision) + '-' + source_hash + '-a')
    audio = directory / ('a' + str(revision) + '-' + source_hash + '-raw.wav')
    try:
        records = [line.split() for line in (directory / 'complete.txt').read_text(encoding='utf-8').splitlines()]
        if (len(records) != 6 or records[0] != ['NativeMediaCache', str(revision), source_hash]
                or records[1] != ['0', '1', '0', '0', '0']
                or len(records[2]) != 6 or len(records[3]) != 5
                or len(records[4]) != 9 or len(records[5]) != 2):
            raise ValueError('Cache revision, source hash, or raw audio options do not match')
        source_info = [int(value) for value in records[2]]
        video_info = [int(value) for value in records[3]]
        audio_info = [int(value) for value in records[4]]
        if (any(value < 0 for value in source_info) or video_info[2] != 0
                or audio_info[0] != 1 or audio_info[5] <= 0 or audio_info[7] <= 0
                or audio_info[8] not in (0, 1)):
            raise ValueError('Cache does not contain complete audio-only media')
        size, audio_hash = records[5]
        if (not audio.is_file() or audio.stat().st_size != int(size)
                or not re.fullmatch(r'[0-9a-f]{64}', audio_hash) or digest(audio) != audio_hash):
            raise ValueError('Cached WAV size or content does not match its manifest')
    except (OSError, ValueError) as error:
        raise RuntimeError('Prepare the current standard audio cache before reuse validation: '
                           + source.name + ' (revision ' + str(revision) + '): ' + str(error)) from error
    return audio


class ExistingHostChecks:
    def __init__(self):
        self.system = FBSystem()
        self.app = FBApplication()
        self.player = FBPlayerControl()
        self.story = FBStory()
        self.folder = None
        self.tracks = {}
        self.clips = {}
        self.owned_tracks = []
        self.expected_hashes = {}
        self.seen = set()
        self.busy = False
        self.callback_active = False
        self.saved = False
        self.appended = False
        self.snapshot = None
        self.report = {'status': 'starting', 'started': time.time(), 'checks': [],
                       'last_command': None, 'last_command_id': None,
                       'last_command_action': None, 'scene_preserved': False}
        self.write()

    def write(self):
        temporary = RESULT.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(self.report, indent=2, ensure_ascii=False), encoding='utf-8')
        temporary.replace(RESULT)

    def check(self, name, passed, detail=None):
        self.report['checks'].append({'name': name, 'passed': bool(passed), 'detail': detail})
        self.write()
        if not passed:
            raise AssertionError(name + ': ' + repr(detail))

    def marker(self, component, name):
        prop = component.PropertyCreate('NVReuse.Kind', FBPropertyType.kFBPT_charptr, 'String', False, True, None)
        if prop is None:
            raise RuntimeError('Cannot create validation ownership metadata')
        prop.Data = name

    def kind(self, component):
        prop = component.PropertyList.Find('NVReuse.Kind')
        if prop is None:
            return None
        value = prop.Data
        return value if value in NAMES else None

    def preserved(self, baseline=None):
        baseline = self.snapshot['components'] if baseline is None else baseline
        current = list(self.system.Scene.Components)
        return all(component in current for component in baseline)

    def stop_owned(self):
        self.player.Stop()
        for track in self.owned_tracks:
            track.Mute = True
        if self.folder is not None:
            self.folder.Solo = False

    def restore(self):
        if self.snapshot is None:
            return
        self.stop_owned()
        self.system.CurrentTake = self.snapshot['take']
        self.player.LoopMode = self.snapshot['loop_mode']
        self.player.LoopStart = self.snapshot['loop_start']
        self.player.LoopStop = self.snapshot['loop_stop']
        self.story.Mute = self.snapshot['story_mute']
        self.player.Goto(self.snapshot['time'])
        self.app.FBXFileName = self.snapshot['filename']
        for component in list(self.system.Scene.Components):
            if hasattr(component, 'Selected'):
                component.Selected = False
        for component, selected in self.snapshot['selection']:
            component.Selected = selected
        self.system.Scene.Evaluate()

    def start(self):
        if bool(self.player.IsPlaying):
            raise RuntimeError('The existing scene is playing; stop it before starting validation')
        components = list(self.system.Scene.Components)
        self.snapshot = {
            'filename': str(self.app.FBXFileName), 'take': self.system.CurrentTake,
            'time': copy_time(self.system.LocalTime), 'loop_mode': self.player.LoopMode,
            'loop_start': copy_time(self.player.LoopStart), 'loop_stop': copy_time(self.player.LoopStop),
            'story_mute': bool(self.story.Mute),
            'components': components,
            'selection': [(component, bool(component.Selected)) for component in components if hasattr(component, 'Selected')],
        }
        self.report['original_scene'] = {
            'filename': self.snapshot['filename'], 'component_count': len(components),
            'take': self.snapshot['take'].Name, 'local_time_ticks': self.snapshot['time'].Get(),
            'loop_mode': str(self.snapshot['loop_mode']),
            'loop_start_ticks': self.snapshot['loop_start'].Get(), 'loop_stop_ticks': self.snapshot['loop_stop'].Get(),
            'story_mute': self.snapshot['story_mute'],
        }
        media = {}
        cache_root = ROOT / 'artifacts' / 'story-host-smoke' / 'NativeVideoMedia'
        revision = current_cache_revision()
        self.report['cache_revision'] = revision
        for name in NAMES:
            source = ROOT / 'build' / 'audio-fixtures' / (name + '.wav')
            cache = prepared_audio_cache(cache_root, source, revision)
            media[name] = cache
            self.expected_hashes[name] = digest(cache)
        self.folder = FBStoryFolder(self.story.RootFolder)
        self.folder.Name = 'NVReuse_folder'
        self.folder.Label = 'Native Video validation'
        self.folder.Mute = False
        self.folder.Solo = False
        for name in NAMES:
            audio = FBAudioClip(str(media[name]))
            audio.Name = 'NVReuse_' + name + ' Audio'
            self.check(name + '_standard_audio_ready', audio.IsMediaReady(), str(media[name]))
            track = FBStoryTrack(FBStoryTrackType.kFBStoryTrackAudio, self.folder)
            track.Name = 'NVReuse_' + name
            track.Mute = True
            self.owned_tracks.append(track)
            self.marker(track, name)
            clip = FBStoryClip(audio, track, seconds(0))
            clip.Name = 'NVReuse_' + name
            self.marker(clip, name)
            clip.FirstLoopMarkIn = seconds(0)
            clip.LastLoopMarkOut = seconds(1)
            clip.Start = seconds(0)
            clip.Stop = seconds(1)
            clip.Speed = 1.0
            self.tracks[name] = track
            self.clips[name] = clip
            expected_bits = 16 if name == 'stereo' else 24
            self.check(name + '_audio_format', audio.Bits == expected_bits and audio.Rate == 48000,
                       {'bits': audio.Bits, 'rate': audio.Rate, 'channels': audio.Channels})
        self.check('startup_preserves_original_components', self.preserved())
        self.report['cache_files'] = {name: str(path) for name, path in media.items()}
        self.report['cache_hashes'] = self.expected_hashes
        if COMMAND.is_file():
            try:
                previous = json.loads(COMMAND.read_text(encoding='utf-8'))
                if isinstance(previous, dict) and isinstance(previous.get('id'), str):
                    self.seen.add(previous['id'])
            except (ValueError, OSError):
                pass
        self.report['status'] = 'ready'
        self.write()
        self.system.OnUIIdle.Add(self.idle)
        self.callback_active = True

    def options(self, loading):
        path = str(OUTPUT / 'reuse-only.fbx')
        options = FBFbxOptions(True, path) if loading else FBFbxOptions(False)
        options.SetAll(FBElementAction.kFBElementActionDiscard, False)
        action = FBElementAction.kFBElementActionAppend if loading else FBElementAction.kFBElementActionSave
        options.Story = action
        options.Audio = action
        options.Video = action
        options.ShowOptionsDialog = False
        options.ShowFileDialog = False
        options.EmbedMedia = True
        for index in range(options.GetTakeCount()):
            options.SetTakeSelect(index, False)
        return options

    def save_media(self):
        self.stop_owned()
        before = list(self.system.Scene.Components)
        options = self.options(False)
        try:
            ok = self.app.FileSave(str(OUTPUT / 'reuse-only.fbx'), options)
        finally:
            self.app.FBXFileName = self.snapshot['filename']
        self.check('save_only_story_audio_video', bool(ok))
        self.check('save_preserves_original_components', self.preserved(before))
        self.check('save_restores_original_filename', str(self.app.FBXFileName) == self.snapshot['filename'])
        self.saved = True

    def append_media(self):
        if not self.saved:
            raise RuntimeError('Save validation media before appending it')
        self.stop_owned()
        before = list(self.system.Scene.Components)
        options = self.options(True)
        try:
            ok = self.app.FileAppend(str(OUTPUT / 'reuse-only.fbx'), False, options)
        finally:
            self.app.FBXFileName = self.snapshot['filename']
            self.system.CurrentTake = self.snapshot['take']
        self.check('append_standard_media', bool(ok))
        self.check('append_preserves_all_existing_components', self.preserved(before))
        added = [component for component in self.system.Scene.Components if component not in before]
        for component in added:
            if isinstance(component, FBStoryTrack) and self.kind(component) is not None:
                component.Mute = True
                self.owned_tracks.append(component)
        appended = {}
        for component in added:
            if isinstance(component, FBStoryClip) and self.kind(component) is not None:
                name = self.kind(component)
                if component.AudioClip is not None:
                    appended[name] = component
        self.check('append_restores_three_validation_audio_clips', set(appended) == set(NAMES), list(appended))
        paths = []
        for name in NAMES:
            audio = appended[name].AudioClip
            path = Path(str(audio.Path))
            paths.append(str(path.resolve()).casefold())
            self.check(name + '_appended_audio_content', audio.IsMediaReady() and path.is_file()
                       and digest(path) == self.expected_hashes[name],
                       {'path': str(path), 'ready': audio.IsMediaReady(), 'bits': audio.Bits})
        self.check('appended_audio_paths_are_distinct', len(set(paths)) == len(NAMES), paths)
        self.appended = True

    def finish(self):
        self.restore()
        if self.callback_active:
            self.system.OnUIIdle.Remove(self.idle)
            self.callback_active = False
        preserved = self.preserved()
        selection = all(bool(component.Selected) == selected for component, selected in self.snapshot['selection'])
        restored = (self.system.CurrentTake == self.snapshot['take']
                    and self.system.LocalTime.Get() == self.snapshot['time'].Get()
                    and str(self.app.FBXFileName) == self.snapshot['filename']
                    and self.player.LoopMode == self.snapshot['loop_mode']
                    and self.player.LoopStart.Get() == self.snapshot['loop_start'].Get()
                    and self.player.LoopStop.Get() == self.snapshot['loop_stop'].Get()
                    and bool(self.story.Mute) == self.snapshot['story_mute'] and selection)
        self.check('finish_preserves_original_scene_objects', preserved)
        self.check('finish_restores_original_scene_state', restored)
        self.check('finish_leaves_validation_tracks_muted', all(bool(track.Mute) for track in self.owned_tracks))
        self.check('save_and_append_were_verified', self.saved and self.appended)
        self.report['scene_preserved'] = preserved and restored
        self.report['status'] = 'passed'
        self.report['elapsed_seconds'] = time.time() - self.report['started']
        setattr(builtins, MARKER, None)

    def dispatch(self, command):
        action = command['action']
        if action == 'play':
            self.stop_owned()
            self.system.CurrentTake = self.snapshot['take']
            self.story.Mute = False
            self.folder.Solo = True
            self.tracks[command['name']].Mute = False
            self.player.LoopMode = FBTransportLoopMode.kFBTransportNoLoop
            self.player.LoopStart = seconds(0)
            self.player.LoopStop = seconds(2)
            self.player.Goto(seconds(0))
            self.system.Scene.Evaluate()
            self.player.Play()
        elif action == 'mute':
            self.stop_owned()
        elif action == 'save':
            self.save_media()
        elif action == 'append':
            self.append_media()
        elif action == 'finish':
            self.finish()

    def idle(self, sender, event):
        if self.busy or not COMMAND.is_file():
            return
        try:
            command = json.loads(COMMAND.read_text(encoding='utf-8'))
        except (ValueError, OSError):
            return
        if not isinstance(command, dict):
            return
        identifier = command.get('id')
        if not isinstance(identifier, str) or not identifier or len(identifier) > 128 or identifier in self.seen:
            return
        self.seen.add(identifier)
        self.report['last_command'] = {'id': identifier, 'action': command.get('action'), 'status': 'running'}
        self.report['status'] = 'running'
        self.write()
        if (set(command) - {'id', 'action', 'name'} or command.get('action') not in ACTIONS
                or ('name' in command and command['name'] not in NAMES)
                or (command.get('action') == 'play' and command.get('name') not in NAMES)):
            self.report['last_command']['status'] = 'rejected'
            self.report['last_command_id'] = identifier
            self.report['last_command_action'] = command.get('action')
            self.report['status'] = 'ready'
            self.write()
            return
        self.busy = True
        try:
            self.dispatch(command)
            self.report['last_command']['status'] = 'complete'
            self.check('command_' + command['action'] + '_preserves_original_components', self.preserved())
            if command['action'] != 'finish':
                self.report['status'] = 'ready'
        except Exception:
            self.report['error'] = traceback.format_exc()
            self.report['status'] = 'failed'
            self.report['last_command']['status'] = 'failed'
            try:
                self.restore()
                self.report['scene_preserved'] = self.preserved()
            except Exception:
                self.report['restore_error'] = traceback.format_exc()
            if self.callback_active:
                self.system.OnUIIdle.Remove(self.idle)
                self.callback_active = False
            setattr(builtins, MARKER, None)
        finally:
            self.report['last_command_id'] = identifier
            self.report['last_command_action'] = command.get('action')
            self.busy = False
            self.write()


if getattr(builtins, MARKER, None) is not None:
    raise RuntimeError('Existing validation is active; send its finish command first')
validation = ExistingHostChecks()
setattr(builtins, MARKER, validation)
try:
    validation.start()
except Exception:
    validation.report['status'] = 'failed'
    validation.report['error'] = traceback.format_exc()
    try:
        validation.restore()
    except Exception:
        validation.report['restore_error'] = traceback.format_exc()
    validation.write()
    setattr(builtins, MARKER, None)
    raise
