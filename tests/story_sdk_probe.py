"""Run only in a dedicated MotionBuilder process to probe Story media support."""

import json
import os
from pathlib import Path
import time
import traceback

from pyfbsdk import *


output = Path(os.environ.get('MOBU_STORY_PROBE_OUTPUT', 'artifacts/story-sdk-probe'))
output.mkdir(parents=True, exist_ok=True)
report = {'started': time.time(), 'steps': []}
system = FBSystem()
application = FBApplication()
state = {'started': False}


def save(step, **values):
    report['steps'].append({'step': step, **values})
    (output / 'results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


def idle(sender, event):
    if state['started']:
        return
    state['started'] = True
    system.OnUIIdle.Remove(idle)
    try:
        story = FBStory()
        story.Mute = False
        video = FBVideoMemory('StoryMemoryProbe')
        video.SetObjectImageSize(194, 130)
        texture = FBTexture('StoryMemoryTexture')
        texture.Video = video
        save('memory_created', type=str(type(video)), properties=[p.Name for p in video.PropertyList])
        track = FBStoryTrack(FBStoryTrackType.kFBStoryTrackVideo)
        track.Name = 'Memory Video Probe'
        save('before_memory_clip', tracks=len(story.RootFolder.Tracks))
        try:
            clip = FBStoryClip(video, track, FBTime(0))
            save('after_memory_clip', clip=bool(clip), clips=len(track.Clips),
                 start=clip.Start.GetSecondDouble(), stop=clip.Stop.GetSecondDouble(),
                 path=clip.ClipVideoPath, track_video=str(type(track.TrackVideo)),
                 sources=[str(type(clip.GetSrc(i))) for i in range(clip.GetSrcCount())])
            clip.Stop = FBTime(0, 0, 2)
            save('memory_set_duration', start=clip.Start.GetSecondDouble(), stop=clip.Stop.GetSecondDouble())
        except Exception:
            save('memory_exception', error=traceback.format_exc(), clips=len(track.Clips))
        if os.environ.get('MOBU_STORY_PROBE_AVI'):
            path = os.environ['MOBU_STORY_PROBE_AVI']
            media = FBVideoClip(path)
            save('avi_loaded', valid=media.IsValid(), width=media.Width, height=media.Height,
                 start=media.StartFrame, stop=media.StopFrame, rate=media.FrameRate)
            file_track = FBStoryTrack(FBStoryTrackType.kFBStoryTrackVideo)
            file_track.Name = 'AVI Video Probe'
            file_clip = FBStoryClip(path, file_track, FBTime(0))
            save('avi_story_created', clips=len(file_track.Clips), start=file_clip.Start.GetSecondDouble(),
                 stop=file_clip.Stop.GetSecondDouble(), path=file_clip.ClipVideoPath,
                 frame_rate=file_clip.FrameRate, source_start=file_clip.SourceMarkIn.GetSecondDouble(),
                 source_stop=file_clip.SourceMarkOut.GetSecondDouble())
        report['status'] = 'passed'
        save('finished')
    except Exception:
        report['status'] = 'failed'
        save('failure', error=traceback.format_exc())
    application.FileExit(False, 0 if report.get('status') == 'passed' else 1)


system.OnUIIdle.Add(idle)
