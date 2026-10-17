"""Register an installed plugin directory from MotionBuilder's Python Editor."""

import hashlib
import json
import os
from pathlib import Path
import re
import socket
import time
import uuid


SECTION = 'AdditionnalPluginPath'
DLL_NAME = 'MoBuVideoPlayer.dll'
MAX_PATH_COUNT = 4096


def _digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def _normalize(path):
    expanded = os.path.expanduser(os.path.expandvars(str(path)))
    return os.path.normcase(os.path.normpath(os.path.abspath(expanded))).casefold()


def _config_path(user_config):
    machine = os.environ.get('COMPUTERNAME') or socket.gethostname()
    if not machine or Path(machine).name != machine:
        raise ValueError('The current machine name is invalid')
    expected = (machine + '.Application.txt').casefold()
    candidates = [path for path in user_config.iterdir()
                  if path.is_file() and path.name.casefold() == expected]
    if len(candidates) != 1:
        raise FileNotFoundError('Exactly one existing machine Application.txt is required')
    return candidates[0]


def _read_paths(config):
    missing = '__native_video_missing_' + uuid.uuid4().hex
    raw = config.Get(SECTION, 'Count', missing)
    if not isinstance(raw, str) or not re.fullmatch(r'[0-9]+', raw.strip()):
        raise ValueError('Additional plugin path Count must be a nonnegative integer')
    count = int(raw.strip())
    if count > MAX_PATH_COUNT:
        raise ValueError('Additional plugin path Count exceeds the safety bound')
    paths = []
    for index in range(count):
        value = config.Get(SECTION, 'PlugInPath' + str(index), missing)
        if not isinstance(value, str) or value == missing or not value.strip():
            raise ValueError('An existing additional plugin path is missing or empty')
        paths.append(value)
    return paths


def _write_report(report_path, report):
    if report_path is None:
        return
    report_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = report_path.with_name(report_path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        os.replace(temporary, report_path)
    finally:
        if temporary.exists():
            temporary.unlink()


def register_plugin_path(plugin_directory, report_path=None):
    """Append one existing plugin directory without changing other preferences."""
    report = {'status': 'starting', 'pid': os.getpid(), 'started': time.time(),
              'changed': False, 'paths_before': None, 'paths_after': None,
              'config_path': None, 'backup_path': None, 'plugin_sha256': None,
              'write_attempted': False, 'error': None}
    output = None
    config_path = None
    original = None
    try:
        from pyfbsdk import FBConfigFile, FBSystem

        plugin = Path(plugin_directory).expanduser().resolve(strict=True)
        if not plugin.is_dir():
            raise NotADirectoryError(str(plugin))
        dll = plugin / DLL_NAME
        if not dll.is_file():
            raise FileNotFoundError('The plugin directory must contain ' + DLL_NAME)
        report['plugin_directory'] = str(plugin)
        report['plugin_sha256'] = _digest(dll)
        user_config = Path(str(FBSystem().UserConfigPath)).resolve(strict=True)
        if not user_config.is_dir():
            raise NotADirectoryError(str(user_config))
        config_path = _config_path(user_config)
        report['config_path'] = str(config_path)
        if report_path is not None:
            candidate = Path(report_path).expanduser().resolve()
            if _normalize(candidate) in {_normalize(config_path), _normalize(dll)}:
                raise ValueError('The report must not overwrite configuration or plugin files')
            output = candidate
        original = config_path.read_bytes()
        config = FBConfigFile('@Application.txt', str(user_config), False, False)
        before = _read_paths(config)
        report['paths_before'] = before
        report['paths_after'] = list(before)
        duplicate = any(_normalize(path) == _normalize(plugin) for path in before)
        if not duplicate:
            if len(before) >= MAX_PATH_COUNT:
                raise ValueError('No additional plugin path slots remain within the safety bound')
            missing = '__native_video_missing_' + uuid.uuid4().hex
            key = 'PlugInPath' + str(len(before))
            if config.Get(SECTION, key, missing) != missing:
                raise ValueError('The next plugin path slot already exists outside Count')
            if config_path.read_bytes() != original:
                raise RuntimeError('The configuration changed before registration; retry explicitly')
            backup_root = output.parent if output is not None else user_config
            backup_root.mkdir(parents=True, exist_ok=True)
            backup = backup_root / (config_path.name + '.' + uuid.uuid4().hex + '.bak')
            with backup.open('xb') as stream:
                stream.write(original)
            report['backup_path'] = str(backup)
            report['backup_sha256'] = hashlib.sha256(original).hexdigest()
            # A byte backup is retained because the SDK may normalize file encoding or spacing.
            report['write_attempted'] = True
            if not config.Set(SECTION, key, str(plugin)):
                raise RuntimeError('The SDK could not save the additional plugin path')
            if not config.Set(SECTION, 'Count', str(len(before) + 1)):
                raise RuntimeError('The SDK could not save the additional plugin path Count')
            report['changed'] = True
        del config
        saved = FBConfigFile('@Application.txt', str(user_config), False, False)
        after = _read_paths(saved)
        del saved
        report['paths_after'] = after
        expected = before if duplicate else before + [str(plugin)]
        if after != expected:
            raise RuntimeError('Saved additional plugin paths do not match the expected values')
        if _digest(dll) != report['plugin_sha256']:
            raise RuntimeError('The plugin DLL changed during registration')
        report['config_sha256'] = _digest(config_path)
        report['status'] = 'passed'
        report['finished'] = time.time()
        _write_report(output, report)
        return report
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = type(error).__name__ + ': ' + str(error)
        report['finished'] = time.time()
        if report['write_attempted'] and config_path is not None and original is not None:
            try:
                report['changed'] = config_path.read_bytes() != original
            except OSError as read_error:
                report['configuration_read_error'] = str(read_error)
        if output is None and report_path is not None and report['config_path'] is None:
            output = Path(report_path).expanduser().resolve()
        try:
            _write_report(output, report)
        except Exception as report_error:
            report['report_error'] = type(report_error).__name__ + ': ' + str(report_error)
        raise


if __name__ == '__main__':
    local_app_data = os.environ.get('LOCALAPPDATA')
    if not local_app_data:
        raise RuntimeError('LOCALAPPDATA is required for the default installed plugin directory')
    root = Path(__file__).resolve().parents[1]
    result = register_plugin_path(
        Path(local_app_data) / 'MoBuVideoPlayer' / 'MotionBuilder2026' / 'plugins',
        root / 'artifacts' / 'installed-story-acceptance' / 'settings-results.json')
    print(json.dumps(result, ensure_ascii=False, indent=2))
