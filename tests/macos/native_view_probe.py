#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Probe native view prerequisites on macOS; does not enable a fyai backend."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


PATH_LISTS = {'PATH', 'CPATH', 'CMAKE_PREFIX_PATH', 'LIBRARY_PATH',
              'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'PKG_CONFIG_PATH'}


def project_path(value, logical, physical):
    if value == logical or value.startswith(logical + '/'):
        return physical + value[len(logical):]
    return value


def execution_environment(environment, logical, physical, scratch):
    result = {}
    for key, value in environment.items():
        if key in PATH_LISTS:
            result[key] = ':'.join(project_path(p, logical, physical)
                                   for p in value.split(':'))
        elif value.startswith(logical) and os.path.isabs(value):
            result[key] = project_path(value, logical, physical)
        else:
            result[key] = value
    result.update(PWD=physical, TMPDIR=scratch + '/')
    if 'OLDPWD' in result and not result['OLDPWD'].startswith(physical + '/'):
        result.pop('OLDPWD')
    return result


def profile_string(value):
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError('control character in profile path')
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def self_check():
    env = execution_environment({
        'ROOT': '/host/project', 'CHILD': '/host/project/include',
        'OTHER': '/host/project-other', 'URL': 'https://host/project',
        'CPATH': '/host/project/include:/opt/homebrew/include',
        'CFLAGS': '-I/host/project/include', 'OLDPWD': '/elsewhere',
    }, '/host/project', '/view/project', '/scratch')
    assert env['ROOT'] == '/view/project'
    assert env['CHILD'] == '/view/project/include'
    assert env['OTHER'] == '/host/project-other'
    assert env['URL'] == 'https://host/project'
    assert env['CPATH'] == '/view/project/include:/opt/homebrew/include'
    assert env['CFLAGS'] == '-I/host/project/include'
    assert 'OLDPWD' not in env
    assert profile_string('a"b\\c') == '"a\\"b\\\\c"'
    try:
        profile_string('a\nb')
    except ValueError:
        pass
    else:
        raise AssertionError('profile newline accepted')


def main():
    self_check()
    if sys.argv[1:] == ['--self-check']:
        print('environment and profile encoder checks passed')
        return
    if sys.platform != 'darwin':
        raise SystemExit('native probe requires macOS; use --self-check elsewhere')
    env = dict(os.environ)
    env['PATH'] = '/opt/homebrew/bin:/usr/local/bin:' + env.get('PATH', '')
    cmake = shutil.which('cmake', path=env['PATH'])
    ninja = shutil.which('ninja', path=env['PATH'])
    if not cmake or not ninja:
        raise SystemExit('CMake and Ninja are required')
    results = {'platform': subprocess.check_output(['sw_vers'], text=True).strip(),
               'profile': 'compatibility only: broad file reads and Mach lookups'}
    with tempfile.TemporaryDirectory(prefix='fyai-native-probe-', dir='/private/tmp') as temp:
        root = Path(temp)
        original, view, scratch = (root / p for p in ('original', 'view', 'scratch'))
        original.mkdir()
        scratch.mkdir()
        (original / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(probe C)\n'
            'add_executable(probe main.c)\n')
        (original / 'project.h').write_text('#define MESSAGE "native build passed"\n')
        (original / 'main.c').write_text(
            '#include <stdio.h>\n#include <project.h>\n'
            'int main(void) { puts(MESSAGE); return 0; }\n')
        initial = dict(env, CPATH=str(original))
        subprocess.run([cmake, '-S', str(original), '-B', str(original / 'build'),
                        '-G', 'Ninja'], env=initial, check=True, capture_output=True)
        subprocess.run(['/bin/cp', '-cR', str(original), str(view)], check=True)
        with (view / 'project.h').open('a') as stream:
            stream.write('#define VIEW_ONLY 1\n')
        assert 'VIEW_ONLY' not in (original / 'project.h').read_text()
        results['independent_clone_write'] = True
        profile = root / 'profile.sb'
        profile.write_text(
            '(version 1)\n(deny default)\n(allow process*)\n'
            '(allow sysctl-read)\n(allow mach-lookup)\n(allow file-read*)\n'
            '(allow file-write* (subpath ' + profile_string(str(view)) + ')'
            ' (subpath ' + profile_string(str(scratch)) + ') (literal "/dev/null"))\n'
            '(deny file-read* file-write* (subpath ' + profile_string(str(original)) + '))\n')
        mapped = execution_environment(dict(env, CPATH=str(original), PROJECT_ROOT=str(original)),
                                       str(original), str(view), str(scratch))

        def sandbox(command):
            return subprocess.run(['/usr/bin/sandbox-exec', '-f', str(profile)] + command,
                                  cwd=view, env=mapped, capture_output=True, text=True, timeout=90)

        read = sandbox(['/bin/cat', str(original / 'project.h')])
        assert read.returncode != 0 and 'permitted' in read.stderr
        write = sandbox(['/bin/sh', '-c', 'echo bad > "$1/new"', 'probe', str(original)])
        assert write.returncode != 0 and not (original / 'new').exists()
        results['original_read_write_denied'] = True
        stale = sandbox([cmake, '-S', '.', '-B', 'build', '-G', 'Ninja'])
        assert stale.returncode != 0 and 'CMakeCache.txt' in stale.stderr
        results['copied_cache_rejected'] = True
        for command in ([cmake, '-S', '.', '-B', 'native-build', '-G', 'Ninja'],
                        [cmake, '--build', 'native-build'], ['./native-build/probe'],
                        [cmake, '--build', 'native-build']):
            run = sandbox(command)
            if run.returncode:
                raise RuntimeError(run.stdout + run.stderr)
        results['mapped_native_build_and_incremental_build'] = True
        nested = sandbox(['/usr/bin/sandbox-exec', '-p', '(version 1)(allow default)', '/usr/bin/true'])
        results['nested_sandbox_supported'] = nested.returncode == 0
        results['nested_sandbox_diagnostic'] = nested.stderr.strip()
        (view / 'detach.c').write_text(
            '#include <unistd.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
            'int main(void) { int p[2]; char c; pid_t child;\n'
            'if (pipe(p)) return 1; child=fork(); if (child<0) return 2;\n'
            'if (!child) { close(p[0]); if (setsid()<0) _exit(3);\n'
            'close(0); close(1); close(2); if (write(p[1], "x", 1)!=1) _exit(4);\n'
            'close(p[1]); for (;;) pause(); }\n'
            'close(p[1]); if (read(p[0], &c, 1)!=1) return 5;\n'
            'printf("%ld\\n", (long)child); return 0; }\n')
        compile_detach = sandbox(['/usr/bin/cc', 'detach.c', '-o', 'detach'])
        if compile_detach.returncode:
            raise RuntimeError(compile_detach.stdout + compile_detach.stderr)
        detached = sandbox(['./detach'])
        if detached.returncode:
            raise RuntimeError(detached.stdout + detached.stderr)
        detached_pid = int(detached.stdout.strip())
        try:
            os.kill(detached_pid, 0)
            results['detached_child_survived_launcher_exit'] = True
        finally:
            os.kill(detached_pid, signal.SIGKILL)
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
