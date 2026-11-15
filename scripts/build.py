"""Shared native build entry point: Conan preparation -> CMake -> build -> test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--step', choices=('deps', 'configure', 'build', 'test', 'all'), default='all')
    parser.add_argument('--profile', choices=('full', 'server', 'client', 'core'), default='full')
    parser.add_argument('--config', choices=('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel'), default='Debug')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--target', nargs='+')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--refresh', action='store_true', help='Explicitly rerun dependency installation')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--generator')
    args = parser.parse_args()
    system = platform.system()
    if system not in ('Windows', 'Linux', 'Darwin'):
        parser.error('Supported native targets: Windows, Linux, macOS')
    os_name = {'Windows': 'windows', 'Linux': 'linux', 'Darwin': 'macos'}[system]
    directory = (ROOT / (args.build_dir or Path(f'build/{os_name}/{args.profile}/{args.config.lower()}'))).resolve()
    if directory == ROOT or not directory.is_relative_to(ROOT):
        parser.error('--build-dir must be a directory below the repository')
    generator = args.generator or ('Visual Studio 17 2022' if system == 'Windows' else 'Ninja')
    flavor = 'debug' if args.config == 'Debug' else 'release'
    compiler = {'Windows': 'windows-msvc', 'Linux': 'linux-gcc', 'Darwin': 'macos-clang'}[system]
    host = ROOT / f'conan/profiles/host/{compiler}-{flavor}'
    build = ROOT / 'conan/profiles/build/default'
    feature = {'full': 'windows-dev', 'client': 'windows-client', 'server': 'server', 'core': 'core'}[args.profile]
    identity = hashlib.sha256()
    for path in (ROOT/'conanfile.py', ROOT/'conan.lock', ROOT/'conan/requirements.txt', host, build):
        identity.update(path.read_bytes())
    identity.update(json.dumps([system, platform.machine(), args.config, feature, generator,
                               os.environ.get('CONAN_HOME', str(Path.home()/'.conan2'))]).encode())
    stamp = directory/'conan/dependencies.sha256'
    valid = stamp.exists() and stamp.read_text() == identity.hexdigest()
    toolchains = list((directory/'conan').rglob('conan_toolchain.cmake'))
    valid = valid and len(toolchains) == 1 and (directory/'conan/graph.json').exists()
    if args.step in ('deps', 'all') and (not valid or args.refresh):
        run([sys.executable, ROOT/'scripts/conan_cli.py', 'install', ROOT,
             '--output-folder', directory/'conan', '--build=missing',
             '--profile:host', host, '--profile:build', build,
             '--options:host', f'&:build_profile={feature}', '--lockfile', ROOT/'conan.lock',
             '--conf', f'tools.cmake.cmaketoolchain:generator={generator}',
             '--conf', f'tools.build:jobs={args.jobs}',
             '--format=json', '--out-file', directory/'conan/graph.json'])
        stamp.write_text(identity.hexdigest())
        toolchains = list((directory/'conan').rglob('conan_toolchain.cmake'))
        valid = len(toolchains) == 1
    if not valid:
        parser.error('Dependencies are missing or changed; run this command with --step deps first')
    if args.step == 'deps':
        print('Dependencies ready; repeated builds do not invoke Conan.')
        return
    if args.step in ('configure', 'all'):
        apps = args.profile in ('full', 'server')
        extensions = args.profile != 'core'
        flags = {'BUILD_CORE_EXTENSIONS': extensions, 'BUILD_SERVER_STACK': apps,
                 'BUILD_GATEWAY_APP': apps, 'BUILD_WRITE_BEHIND_TOOLS': apps,
                 'BUILD_MIGRATIONS_RUNNER': apps, 'BUILD_CLIENT_GUI': args.profile in ('full', 'client'),
                 'BUILD_SERVER_TESTS': args.profile != 'client', 'ENABLE_SANITIZERS': args.sanitize}
        command = ['cmake', '-S', ROOT, '-B', directory, '-G', generator,
                   f'-DCMAKE_TOOLCHAIN_FILE={toolchains[0]}', f'-DCMAKE_BUILD_TYPE={args.config}',
                   f'-DCMAKE_CONFIGURATION_TYPES={args.config}',
                   '-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release',
                   '-DCMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL=Release']
        if generator.startswith('Visual Studio'):
            command += ['-A', 'x64']
        run(command + [f'-D{key}={"ON" if value else "OFF"}' for key, value in flags.items()])
    if args.step in ('build', 'all'):
        command = ['cmake', '--build', directory, '--config', args.config, '--parallel', args.jobs]
        if args.target:
            command += ['--target', *args.target]
        run(command)
    if args.step == 'test':
        run(['ctest', '--test-dir', directory, '-C', args.config, '--output-on-failure', '--no-tests=error', '-j', args.jobs])


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
