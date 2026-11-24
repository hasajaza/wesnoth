#!/usr/bin/env python3
"""
Wesnoth Access packaging script.

Runs inside the upstream MinGW cross-compilation image
(wesnoth/wesnoth:mingw-master) after wesnoth.exe has been built.

It produces:
  * WesnothAccess-<version>-win64-overlay.zip
      Compact package: wesnoth.exe, every MinGW runtime DLL that wesnoth.exe
      (transitively) imports, UniversalSpeech.dll, nvdaControllerClient.dll,
      launcher/installer scripts and plain-text documentation. It is meant to
      be used together with an existing, unmodified Wesnoth 1.18.8
      installation (its data is used through --data-dir).
  * optionally WesnothAccess-<version>-win64-standalone.zip
      The above plus the complete game data (data, fonts, images, sounds,
      translations if built), so no prior installation is needed.

Usage:
  make_package.py --exe <wesnoth.exe> --source <repo root> --out <dir>
                  --speech-dir <dir with UniversalSpeech.dll etc.>
                  [--standalone] [--revision <git sha>]
"""

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import sys
import zipfile

# The last entry holds the compiler runtime (libssp-0.dll), which upstream's
# Dockerfile copies separately.
DLL_SEARCH_PATHS = [pathlib.Path('/windows/mingw64/bin'), pathlib.Path('/msys64/mingw64/bin'),
                    pathlib.Path('/usr/x86_64-w64-mingw32/bin')]


def find_dll(name):
    for base in DLL_SEARCH_PATHS:
        p = base / name
        if p.exists():
            return p
        # case-insensitive lookup
        lname = name.lower()
        for cand in base.iterdir():
            if cand.name.lower() == lname:
                return cand
    return None


def collect_dlls(exe):
    import pefile  # available in the upstream image
    found = {}
    queue = [pathlib.Path(exe)]
    seen = set()
    while queue:
        mod = queue.pop()
        if str(mod) in seen:
            continue
        seen.add(str(mod))
        pe = pefile.PE(str(mod), fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            name = entry.dll.decode()
            if name.lower() in found:
                continue
            path = find_dll(name)
            if path is not None:  # system DLLs (KERNEL32 etc.) are not found and skipped
                found[name.lower()] = path
                queue.append(path)
        pe.close()
    return sorted(found.values(), key=lambda p: p.name.lower())


# DLLs that are part of every Windows 10/11 x64 installation.
SYSTEM_DLLS = {
    'advapi32.dll', 'crypt32.dll', 'kernel32.dll', 'msvcrt.dll', 'ole32.dll', 'oleaut32.dll', 'shell32.dll',
    'shlwapi.dll', 'user32.dll', 'winmm.dll', 'ws2_32.dll', 'gdi32.dll', 'imm32.dll', 'setupapi.dll',
    'version.dll', 'psapi.dll', 'rpcrt4.dll', 'dwrite.dll', 'usp10.dll', 'msimg32.dll', 'bcrypt.dll',
    'iphlpapi.dll', 'wldap32.dll', 'normaliz.dll', 'secur32.dll', 'dnsapi.dll', 'comdlg32.dll',
    'comctl32.dll', 'userenv.dll', 'cfgmgr32.dll', 'hid.dll', 'dbghelp.dll', 'mswsock.dll', 'ncrypt.dll',
    'uxtheme.dll', 'dwmapi.dll', 'winspool.drv', 'gdiplus.dll', 'opengl32.dll', 'd3d9.dll', 'dxgi.dll',
    'mfplat.dll', 'mfreadwrite.dll', 'avrt.dll', 'xinput1_4.dll', 'uuid.dll', 'wininet.dll', 'urlmon.dll',
    'dinput8.dll', 'mmdevapi.dll', 'wtsapi32.dll', 'netapi32.dll', 'credui.dll',
}


def verify_imports(exe, dlls, speech_dir):
    """Every import of every shipped module must be shipped or be a system DLL."""
    import pefile
    shipped = {d.name.lower() for d in dlls} | {'wesnoth.exe'}
    modules = [pathlib.Path(exe)] + list(dlls)
    for extra in ('UniversalSpeech.dll', 'nvdaControllerClient.dll'):
        p = pathlib.Path(speech_dir) / extra
        if p.exists():
            modules.append(p)
            shipped.add(extra.lower())
    missing = set()
    for mod in modules:
        pe = pefile.PE(str(mod), fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            name = entry.dll.decode().lower()
            if name not in shipped and name not in SYSTEM_DLLS and not name.startswith('api-ms-win-'):
                missing.add(f'{name} (needed by {mod.name})')
        pe.close()
    return missing


def read_version(source):
    txt = (source / 'src' / 'wesconfig.h').read_text(encoding='utf-8')
    m = re.search(r'#define VERSION "([^"]+)"', txt)
    return m.group(1) if m else 'unknown'


def read_access_version(source):
    vfile = source / 'src' / 'accessibility' / 'version.hpp'
    if not vfile.exists():
        return 'baseline'
    txt = vfile.read_text(encoding='utf-8')
    m = re.search(r'ACCESS_VERSION_STRING "([^"]+)"', txt)
    return m.group(1) if m else 'unknown'


def to_crlf(data):
    return data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


def add_file(zf, src, arc, text=False, subst=None):
    data = pathlib.Path(src).read_bytes()
    if subst:
        for k, v in subst.items():
            data = data.replace(k.encode(), v.encode())
    if text:
        data = to_crlf(data)
    zf.writestr(arc, data)


def add_tree(zf, src_root, arc_root):
    src_root = pathlib.Path(src_root)
    for path in sorted(src_root.rglob('*')):
        if path.is_file():
            rel = path.relative_to(src_root).as_posix()
            if '/.git' in '/' + rel:
                continue
            zf.write(path, f'{arc_root}/{rel}')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--source', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--speech-dir', required=True)
    ap.add_argument('--revision', default='unknown')
    ap.add_argument('--standalone', action='store_true')
    args = ap.parse_args()

    source = pathlib.Path(args.source)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    speech = pathlib.Path(args.speech_dir)
    pkgsrc = source / 'packaging' / 'access'
    docsrc = source / 'doc' / 'accessibility'

    version = read_version(source)
    aversion = read_access_version(source)
    base = f'WesnothAccess-{aversion}-wesnoth{version}-win64'
    root = 'WesnothAccess'

    dlls = collect_dlls(args.exe)
    missing = verify_imports(args.exe, dlls, speech)
    if missing:
        print('ERROR: unresolved non-system DLL imports: ' + ', '.join(sorted(missing)), file=sys.stderr)
        sys.exit(1)
    print(f'Collected {len(dlls)} runtime DLLs:')
    for d in dlls:
        print('  ', d.name)

    subst = {'@ACCESS_VERSION@': aversion, '@GAME_VERSION@': version, '@REVISION@': args.revision}

    def write_common(zf):
        zf.write(args.exe, f'{root}/wesnoth.exe')
        for d in dlls:
            zf.write(d, f'{root}/{d.name}')
        for f in ('UniversalSpeech.dll', 'nvdaControllerClient.dll'):
            p = speech / f
            if p.exists():
                zf.write(p, f'{root}/{f}')
            else:
                print(f'WARNING: {f} missing from speech dir', file=sys.stderr)
        for f in ('UniversalSpeech-LICENSE.txt', 'nvdaControllerClient-LICENSE.txt', 'SOURCES.txt'):
            p = speech / f
            if p.exists():
                add_file(zf, p, f'{root}/licenses/{f}', text=True)
        add_file(zf, source / 'COPYING', f'{root}/licenses/COPYING-Wesnoth-GPL2.txt', text=True)
        add_file(zf, source / 'copyright', f'{root}/licenses/copyright-Wesnoth.txt', text=True)
        for f in sorted(pkgsrc.glob('*.cmd')) + sorted(pkgsrc.glob('*.ps1')):
            add_file(zf, f, f'{root}/{f.name}', text=True, subst=subst)
        for f in sorted(docsrc.glob('*.txt')):
            add_file(zf, f, f'{root}/docs/{f.name}', text=True, subst=subst)
        info = (f'Wesnoth Access {aversion}\n'
                f'Base game version: {version}\n'
                f'Build commit: {args.revision}\n'
                f'Source: https://github.com/hasajaza/wesnoth/tree/{args.revision}\n')
        zf.writestr(f'{root}/BUILD-INFO.txt', to_crlf(info.encode()))

    overlay = out / f'{base}-overlay.zip'
    with zipfile.ZipFile(overlay, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        write_common(zf)
        zf.writestr(f'{root}/PACKAGE-TYPE.txt', b'overlay\r\n')
    print(f'Wrote {overlay} ({overlay.stat().st_size // 1024} KiB) sha256={sha256(overlay)}')

    if args.standalone:
        standalone = out / f'{base}-standalone.zip'
        with zipfile.ZipFile(standalone, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            write_common(zf)
            zf.writestr(f'{root}/PACKAGE-TYPE.txt', b'standalone\r\n')
            for d in ('data', 'fonts', 'images', 'sounds'):
                add_tree(zf, source / d, f'{root}/{d}')
            trans = pathlib.Path(args.exe).parent / 'translations'
            if trans.exists():
                add_tree(zf, trans, f'{root}/translations')
            for f in ('README.md', 'changelog.md', 'copyright', 'COPYING'):
                zf.write(source / f, f'{root}/{f}')
        print(f'Wrote {standalone} ({standalone.stat().st_size // (1024*1024)} MiB) sha256={sha256(standalone)}')


if __name__ == '__main__':
    main()
