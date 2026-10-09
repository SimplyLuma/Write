#!/usr/bin/env python3
"""Assemble the Linux binary, qualified toolkit and complete Writer runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--kit', type=Path, required=True)
parser.add_argument('--staged', type=Path, required=True)
parser.add_argument('--office', type=Path, default=Path('/usr/lib64/libreoffice'))
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
if out.exists():
    raise SystemExit('Output must be a new directory')
out.mkdir(parents=True)
lib = out / 'lib'
lib.mkdir()
origins = set()
elf_origins = {}

def copy_file(source, destination):
    source = Path(source)
    resolved = source.resolve(strict=True)
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if destination.read_bytes() != resolved.read_bytes():
            raise RuntimeError(f'Conflicting library name: {destination.name}')
        return
    # Keep aliases of a shared object as one loader instance. Materializing
    # each symlink as a separate ELF can duplicate static state (NSS/p11-kit).
    is_elf = resolved.open('rb').read(4) == b'\x7fELF'
    previous = elf_origins.get(str(resolved)) if is_elf else None
    if previous is not None:
        destination.symlink_to(os.path.relpath(previous, destination.parent))
    else:
        shutil.copy2(resolved, destination)
        if is_elf: elf_origins[str(resolved)] = destination
    origins.add(str(source))
    origins.add(str(resolved))

def copy_tree(source, destination):
    source, destination = Path(source), Path(destination)
    for path in sorted(source.rglob('*')):
        if path.is_file():
            copy_file(path, destination / path.relative_to(source))

copy_file(args.binary, out / 'bin/luma-write')
copy_tree(args.kit / 'share', out / 'share')
for folder in ['lib', 'lib64']:
    if (args.kit / folder).exists():
        for path in (args.kit / folder).glob('*.so*'):
            copy_file(path, lib / path.name)
copy_tree(args.staged / 'usr/share', out / 'share')
copy_tree(args.office, out / 'lib/office')
# Only packaged runtime data and redistributable fonts, never user profiles.
for folder in ['liblangtag', 'libexttextcat', 'libnumbertext', 'hunspell', 'hyphen']:
    source = Path('/usr/share') / folder
    if source.exists(): copy_tree(source, out / 'share' / folder)
for path in Path('/usr/share/fonts').rglob('*'):
    if path.is_file() and any(word in str(path).lower() for word in ['figtree', 'liberation', 'opensymbol']):
        copy_file(path, out / 'share/fonts' / path.relative_to('/usr/share/fonts'))

# NSS loads its trust provider dynamically, so ldd does not report this alias.
for name in ['libnssckbi.so', 'p11-kit-proxy.so']:
    source = Path('/usr/lib64') / name
    if source.exists(): copy_file(source, lib / name)

# Resolve every ELF in the engine, including dynamically loaded Writer filters.
env = dict(os.environ)
env['LD_LIBRARY_PATH'] = ':'.join([str(args.kit / 'lib'), str(args.kit / 'lib64'),
    str(args.office / 'program'), '/usr/lib64/libreoffice/ure/lib'])
queue = [p for p in out.rglob('*') if p.is_file() and p.open('rb').read(4) == b'\x7fELF']
seen = set()
while queue:
    path = queue.pop()
    if path in seen: continue
    seen.add(path)
    result = subprocess.run(['ldd', str(path)], env=env, text=True, capture_output=True)
    for line in result.stdout.splitlines():
        if 'not found' in line:
            raise RuntimeError(f'Unresolved dependency in {path.name}: {line.strip()}')
        words = line.strip().split()
        candidate = words[2] if len(words) >= 3 and words[1] == '=>' else words[0] if words else ''
        if candidate.startswith('/') and Path(candidate).is_file():
            target = lib / Path(candidate).name
            if not target.exists():
                copy_file(candidate, target)
                queue.append(target)

packages = {}
for origin in sorted(origins):
    result = subprocess.run(['rpm', '-qf', '--qf', '%{NAME}\t%{VERSION}-%{RELEASE}\t%{ARCH}\t%{LICENSE}\t%{SOURCERPM}\n', origin],
                            text=True, capture_output=True)
    if result.returncode: continue
    for row in result.stdout.splitlines():
        fields = row.split('\t')
        if len(fields) == 5:
            name, version, arch, license_id, source = fields
            packages[name] = dict(name=name, version=version, arch=arch, license=license_id,
                source_rpm=source, source_repository='https://src.fedoraproject.org/rpms/' + source.removesuffix('.src.rpm').rsplit('-', 2)[0],
                source_build='https://koji.fedoraproject.org/koji/search?type=build&terms=' + source.removesuffix('.src.rpm'))
for name in sorted(packages):
    paths = subprocess.check_output(['rpm', '-ql', name], text=True).splitlines()
    for value in paths:
        path = Path(value)
        if path.is_file() and str(path).startswith('/usr/share/licenses/'):
            copy_file(path, out / 'share/licenses/runtime' / name / path.relative_to('/usr/share/licenses'))
copy_tree(root / 'LICENSES', out / 'share/licenses/write')
for name in ['LICENSE.md', 'THIRD_PARTY_NOTICES.md', 'README.md']:
    copy_file(root / name, out / 'share/licenses/write' / name)
(out / 'share/licenses/runtime-packages.json').write_text(json.dumps(list(packages.values()), indent=2) + '\n')
(out / 'share/licenses/CORRESPONDING_SOURCE.md').write_text('''# Corresponding source\n\nRuntime package versions, source RPM names and Fedora build/source locations\nare recorded in runtime-packages.json. Preserve this inventory and all\ncomponent licenses. A distributor must supply the exact corresponding source\nfor components whose licenses require it alongside the binary release.\nWrite and the qualified shared toolkit source are in the public Write repository.\n''')
(out / 'etc/fonts').mkdir(parents=True)
(out / 'etc/fonts/fonts.conf').write_text('''<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n<fontconfig><dir prefix="relative">../../share/fonts</dir><dir>/usr/share/fonts</dir>\n<cachedir prefix="xdg">fontconfig</cachedir></fontconfig>\n''')
subprocess.run(['glib-compile-schemas', str(out / 'share/glib-2.0/schemas')], check=True)
launcher = out / 'Write'
launcher.write_text('''#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexport LUMA_LIBREOFFICE_PROGRAM="$root/lib/office/program"\nexport XDG_DATA_DIRS="$root/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}:/usr/local/share:/usr/share"\nexport GSETTINGS_SCHEMA_DIR="$root/share/glib-2.0/schemas"\nexport FONTCONFIG_FILE="$root/etc/fonts/fonts.conf"\nexport SAL_USE_VCLPLUGIN=svp\nexport LD_LIBRARY_PATH="$root/lib/office/program:$root/lib"\nexec "$root/lib/ld-linux-x86-64.so.2" --library-path "$LD_LIBRARY_PATH" "$root/bin/luma-write" "$@"\n''')
launcher.chmod(0o755)
files = {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(out.rglob('*')) if p.is_file()}
(out / 'manifest.json').write_text(json.dumps({'architecture': 'x86_64',
    'engine': 'LibreOfficeKit 26.2.6.3', 'files': files}, indent=2) + '\n')
print(f'Assembled {len(files)} files and {len(packages)} runtime packages in {out}')
