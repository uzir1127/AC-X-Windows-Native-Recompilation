#!/usr/bin/env python3
"""Build an allowlisted portable Windows folder/ZIP from an existing SDL3 build.

Does not read the ISO, generated/image.bin, saves, or the user's settings.
Run from a maintainer's completed build; end users only need the resulting ZIP.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, zipfile, os

ROOT = Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--exe', type=Path, default=ROOT/'build/pspreco-sdl3.exe')
    ap.add_argument('--sdl', type=Path, required=True)
    ap.add_argument('--sdl-license', type=Path, required=True)
    ap.add_argument('--zip', action='store_true')
    ap.add_argument('--refresh', action='store_true', help='Refresh an existing clean package; refuses saves, ISOs and unexpected files')
    ap.add_argument('--with-source', action='store_true', default=True, help='Include matching source snapshot under licenses/source.zip')
    ap.add_argument('--validation', type=Path, help='Human-readable release validation report')
    ap.add_argument('--texture-pack', type=Path, help='Optional ULUS10176 texture pack directory; copied unchanged with credits')
    a=ap.parse_args()
    dest=a.out.resolve()
    # Never merge a fresh release into an existing installation with saves.
    if dest.exists() and not a.refresh:
        ap.error('Output already exists. Choose a new folder; existing installations are never overwritten.')
    archive=dest.parent/(dest.name+'.zip')
    if a.zip and archive.exists() and not a.refresh: ap.error(f'ZIP already exists: {archive}')
    files={
        'pspreco-sdl3.exe':a.exe,
        'SDL3.dll':a.sdl,
        'Play.bat':ROOT/'packaging/windows/Play.bat',
        'README.txt':ROOT/'packaging/windows/README.txt',
        'CREDITS.txt':ROOT/'packaging/windows/CREDITS.txt',
        'pspreco.toml':ROOT/'packaging/windows/defaults.toml',
        'gamecontrollerdb.txt':ROOT/'packaging/windows/gamecontrollerdb.txt',
        'LICENSE.txt':ROOT/'LICENSE',
        'NOTICE.md':ROOT/'NOTICE.md',
        'licenses/SDL3/LICENSE.txt':a.sdl_license,
        'licenses/SDL_GameControllerDB/LICENSE.txt':ROOT/'packaging/licenses/SDL_GameControllerDB-LICENSE.txt',
        'licenses/LLVM/LICENSE.txt':ROOT/'packaging/licenses/LLVM-LICENSE.txt',
        'licenses/MinGW-w64/COPYING.runtime.txt':ROOT/'packaging/licenses/MinGW-w64-runtime.txt',
        'licenses/MinGW-w64/COPYING.winpthreads.txt':ROOT/'packaging/licenses/winpthreads.txt',
        'licenses/imgui/LICENSE.txt':ROOT/'third_party/imgui/LICENSE.txt',
        'licenses/lua/README':ROOT/'third_party/lua/README',
        'licenses/basis_universal/LICENSE':ROOT/'third_party/basis_universal/LICENSE',
        'licenses/zstd/LICENSE':ROOT/'third_party/basis_universal/zstd/LICENSE',
        'licenses/atrac/README.txt':ROOT/'third_party/ppsspp/at3_standalone/README.txt',
        'licenses/atrac/COPYING.LGPLv2.1':ROOT/'packaging/licenses/COPYING.LGPLv2.1',
        'font/ltn0.pgf':ROOT/'third_party/ppsspp/assets/flash0/font/ltn0.pgf',
        'font/jpn0.pgf':ROOT/'third_party/ppsspp/assets/flash0/font/jpn0.pgf',
    }
    if a.validation: files['VALIDATION.txt']=a.validation
    if a.texture_pack:
        pack=a.texture_pack.resolve()
        if not (pack/'textures.ini').is_file(): ap.error('Texture pack must contain textures.ini.')
        allowed={'.png','.ktx2','.dds','.jpg','.jpeg','.bmp','.tga','.ini','.txt','.md'}
        for asset in sorted(pack.rglob('*')):
            if asset.is_symlink(): ap.error(f'Texture pack must not contain symlinks: {asset}')
            if asset.is_file():
                if asset.suffix.lower() not in allowed: ap.error(f'Unexpected texture pack file: {asset}')
                files['textures/ULUS10176/'+asset.relative_to(pack).as_posix()]=asset
    for source in files.values():
        if not source.is_file(): ap.error(f'Missing required runtime file: {source}')
    if a.exe.read_bytes()[:2]!=b'MZ' or a.sdl.read_bytes()[:2]!=b'MZ':
        ap.error('Executable and SDL DLL must be Windows PE files.')
    if dest.exists():
        permitted=set(files)|{'manifest.json','licenses/source.zip'}
        existing={x.relative_to(dest).as_posix() for x in dest.rglob('*') if x.is_file()}
        unexpected=existing-permitted
        if unexpected: ap.error('Refusing to refresh a folder with personal or unexpected files: '+', '.join(sorted(unexpected)[:5]))
        for item in dest.rglob('*'):
            if item.is_symlink(): ap.error('Output contains a symbolic link: '+str(item))
    dest.mkdir(parents=True,exist_ok=True)
    hashes={}
    for rel,source in files.items():
        target=dest/rel; target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,target)
        hashes[rel]=hashlib.sha256(target.read_bytes()).hexdigest()
    if a.with_source:
        source=dest/'licenses/source.zip'
        suffixes={'.c','.cpp','.h','.hpp','.inl','.inc','.py','.ps1','.bat','.txt','.md','.json','.toml','.cmake','.vert','.frag','.glsl','.yaml','.yml'}
        entries={}
        for directory in ('runtime','generated','recomp','tools','third_party','data','packaging','tests'):
            for item in (ROOT/directory).rglob('*'):
                if not item.is_file() or item.is_symlink() or '__pycache__' in item.parts: continue
                if item.suffix.lower() in suffixes or item.name.startswith(('LICENSE','COPYING')):
                    entries[item.relative_to(ROOT).as_posix()]=item
        for name in ('build.py','README.md','README.txt','CREDITS.md','NOTICE.md','LICENSE'):
            entries[name]=ROOT/name
        entries['SOURCE-BUILD.txt']=ROOT/'packaging/windows/SOURCE-BUILD.txt'
        with zipfile.ZipFile(source,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            for rel,item in sorted(entries.items()): z.write(item,rel)
        hashes['licenses/source.zip']=hashlib.sha256(source.read_bytes()).hexdigest()
    (dest/'manifest.json').write_text(json.dumps(hashes,indent=2)+'\n')
    if a.zip:
        temp=archive.with_name(archive.name+'.tmp')
        with zipfile.ZipFile(temp,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
            for rel in sorted([*hashes,'manifest.json']):
                z.write(dest/rel,Path(dest.name)/rel)
        os.replace(temp,archive)
        print(f'ZIP: {archive} ({archive.stat().st_size:,} bytes)')
    print(f'Portable package: {dest} ({len(hashes)} files; no ISO, extracted executable, or personal saves)')

if __name__=='__main__': main()
