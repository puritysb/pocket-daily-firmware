#!/usr/bin/env python3
"""Offline-only C3 diagnostic wake-stub build. Never provisions or touches a reader."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rtc_probe_policy as policy


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--command-database', type=Path, required=True)
    parser.add_argument('--baseline-elf', type=Path, required=True)
    parser.add_argument('--probe-elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = json.loads(args.command_database.read_text())
    rows = [row for row in rows if (Path(row.get('directory', args.command_database.resolve().parent))/row['file']).resolve().as_posix().endswith('/src/main.cpp')]
    if len(rows) != 1:
        raise ValueError('One exact C3 main translation unit is required')
    command = rows[0].get('arguments') or shlex.split(rows[0]['command'])
    compiler = Path(command[0])
    if compiler.name != 'riscv32-esp-elf-g++':
        raise ValueError('Expected the pinned C3 RISC-V toolchain')
    tool = lambda name: str(compiler.with_name('riscv32-esp-elf-'+name))
    symbols = lambda path: subprocess.check_output([tool('nm'), '-n', str(path.resolve())], text=True)
    layout = policy.validate_layout(symbols(args.baseline_elf), symbols(args.probe_elf))
    includes = []
    for index, item in enumerate(command):
        if item.startswith('-I'):
            includes.append(item)
        if item == '-isystem':
            includes.extend([item, command[index+1]])
    source = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(mode=0o700, parents=False, exist_ok=False)
    elf = out/'probe.elf'
    build = [tool('gcc'), '-march=rv32imc', '-mabi=ilp32', '-Oz', '-ffreestanding', '-fno-pic',
             '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-msmall-data-limit=0',
             '-mrelax', '-nostdlib', '-Wl,--relax,--build-id=none,-T'+str(source/'payload.ld')+',-e,entry']
    with (out/'compile.log').open('x') as log:
        subprocess.run(build+includes+[str(source/'probe.c'), '-o', str(elf)],
                       cwd=rows[0].get('directory', args.command_database.resolve().parent), stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', '-j', '.text', str(elf), str(out/'probe.bin')], check=True)
    data = (out/'probe.bin').read_bytes()
    if not 0 < len(data) <= policy.GPIO_TRACE_ADDRESS-policy.ARENA_START:
        raise ValueError('Payload overlaps the run-bound trace')
    with (out/'disassembly.txt').open('x') as log:
        subprocess.run([tool('objdump'), '-dr', str(elf)], stdout=log, check=True)
    receipt = dict(nonshippingDiagnostic=True, deviceIO=0, flashWrites=0, bytes=len(data),
                   payloadSHA256=digest(data), sourceSHA256=digest((source/'probe.c').read_bytes()),
                   entry=policy.ARENA_START, trace=policy.GPIO_TRACE_ADDRESS, layout=layout,
                   GPIOPins=[3], firstWakeMustBeTimer=True, independentGPIOWitnessRequired=True)
    (out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('Offline diagnostic payload built; reader provisioning is a separate reviewed operation.')


if __name__ == '__main__':
    main()
