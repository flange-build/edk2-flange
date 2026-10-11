#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the display DT fixup against the repository's libfdt on the host."""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


def extract_function(source, name):
    # Ignore braces in comments and literals while retaining source offsets.
    code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda match: ' ' * len(match[0]), source, flags=re.S)
    declaration = re.search(r'STATIC\s+EFI_STATUS\s+(?:EFIAPI\s+)?' + name + r'\s*\(', code)
    if declaration is None:
        raise RuntimeError(f'firmware function not found: {name}')
    start = code.index('{', declaration.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (code[end] == '{') - (code[end] == '}')
        end += 1
    return source[declaration.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true', help='enable AddressSanitizer and UBSan')
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    repo = tests.parents[1]
    source = (tests.parent / 'Silicon/Qualcomm/QCS6490/Drivers/DspPreloadDxe/DeviceTree.c').read_text()
    libfdt = repo / 'edk2/MdePkg/Library/BaseFdtLib/libfdt/libfdt'
    if not (libfdt / 'libfdt.h').is_file():
        parser.error('initialize the edk2 and libfdt submodules first (see tests/README.md)')
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if not compiler or shutil.which(compiler[0]) is None:
        parser.error('a host C compiler is required; install cc or set CC')
    functions = '\n\n'.join(extract_function(source, name) for name in (
        'DtSetDisplayFixupVersion', 'DtFixupDisplay', 'DtFixup'))
    with tempfile.TemporaryDirectory(prefix='edk2-display-fixup-') as directory:
        temporary = Path(directory)
        (temporary / 'display_fixup_functions.c').write_text(functions)
        binary = temporary / 'test_display_fixup'
        command = compiler + ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                              '-Wno-unused-parameter', '-g']
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        command += ['-I', str(libfdt), '-I', str(temporary),
                    str(tests / 'display_fixup_host.c')]
        command += [str(path) for path in sorted(libfdt.glob('*.c'))]
        subprocess.run(command + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
