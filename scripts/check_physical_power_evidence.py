#!/usr/bin/env python3
"""Offline consistency checks for external-fixture evidence; never hardware sign-off.

No reader I/O, GPIO drive, serial open, relay commands or flashing is performed.
A consistent receipt still needs independently reviewed physical measurements.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


class EvidenceError(ValueError):
    pass


def need(condition, reason):
    if not condition:
        raise EvidenceError(reason)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check(receipt, *, model, run, candidate_sha256, evidence_root):
    need(type(receipt) is dict, 'receipt must be an object')
    need(model in ('X3', 'X4'), 'unsupported model')
    need(type(run) is str and re.fullmatch(r'[a-f0-9]{32}', run), 'fresh run nonce required')
    need(type(candidate_sha256) is str and re.fullmatch(r'[a-f0-9]{64}', candidate_sha256),
         'exact candidate SHA required')
    need(type(receipt.get('schema')) is int and receipt['schema'] == 1 and receipt.get('model') == model and
         receipt.get('run') == run and receipt.get('candidateSHA256') == candidate_sha256,
         'receipt identity/run/image differs')
    case = receipt.get('case')
    need(case in ('mechanical_button_wake', 'battery_cold_on'), 'unknown physical scenario')
    raw = receipt.get('rawEvidence', {})
    need(type(raw) is dict, 'raw evidence must be an object')
    name = raw.get('path')
    need(type(name) is str and name and not Path(name).is_absolute() and '..' not in Path(name).parts,
         'raw evidence must be within the receipt directory')
    root = evidence_root.resolve()
    path = (root / name).resolve()
    need(path.is_relative_to(root) and path.is_file(), 'raw evidence file unavailable')
    need(0 < path.stat().st_size <= 16 * 1024 * 1024, 'raw evidence size exceeds budget')
    with path.open('rb') as stream:
        data = stream.read(16 * 1024 * 1024 + 1)
    need(0 < len(data) <= 16 * 1024 * 1024 and digest(data) == raw.get('sha256'),
         'raw evidence digest or bounded size differs')
    need(receipt.get('actuator') == 'external_mechanical' and
         receipt.get('pressFeedback') == 'independent_sensor', 'mechanical press was not observed')
    need(receipt.get('buttonReleased') is True, 'button release not observed')
    events = receipt.get('events')
    expected = (['sleep_committed', 'mechanical_press', 'native_reader_render', 'next_page_render', 'restored']
                if case == 'mechanical_button_wake' else
                ['vbus_absent', 'cpu_rail_off', 'mechanical_press', 'battery_boot',
                 'native_reader_render', 'next_page_render', 'restored'])
    need(type(events) is list and all(type(e) is dict for e in events) and [e.get('kind') for e in events] == expected, 'physical event order differs')
    times = [e.get('monotonicMs') for e in events]
    need(all(type(t) is int and t >= 0 for t in times) and
         all(a < b for a, b in zip(times, times[1:])), 'timestamps must strictly advance')
    if case == 'mechanical_button_wake':
        need(receipt.get('resetReason') == 'deep_sleep' and receipt.get('wakeCause') == 'gpio',
             'timer/software/power-on is not a GPIO-button wake')
    else:
        need(receipt.get('vbusPresent') is False and receipt.get('debugBackPower') is False,
             'battery-only power not established')
        need(receipt.get('cpuRailOffMeasured') is True and receipt.get('batterySupplyMeasured') is True,
             'CPU rail loss and battery supply require external measurements')
        need(receipt.get('resetReason') == 'power_on' and receipt.get('wakeCause') == 'undefined',
             'deep-sleep/software reset is not a cold power-on')
    need(receipt.get('samePosition') is True and receipt.get('nextRenderCompleted') is True,
         'native reader continuation incomplete')
    need(receipt.get('originalRestored') is True, 'restoration evidence missing')
    return {'consistent': True, 'hardwareSignoff': False, 'model': model, 'case': case,
            'scope': 'Offline consistency only; physical provenance, calibration, wiring and raw measurements require independent review.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--model', choices=('X3', 'X4'), required=True)
    parser.add_argument('--run', required=True)
    parser.add_argument('--candidate-sha256', required=True)
    args = parser.parse_args()
    try:
        result = check(json.loads(args.receipt.read_text()), model=args.model, run=args.run,
                       candidate_sha256=args.candidate_sha256, evidence_root=args.receipt.parent)
    except (EvidenceError, OSError, ValueError, TypeError, KeyError):
        parser.exit(2, 'Physical evidence refused; no hardware action was taken.\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
