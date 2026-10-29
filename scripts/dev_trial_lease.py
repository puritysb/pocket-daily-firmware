#!/usr/bin/env python3
"""Explicit developer-provisioned production trial. Plan is offline; mutations never retry."""
import argparse
from contextlib import redirect_stdout
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
import traceback
import urllib.request
import urllib.parse
import uuid

import dev_reader_walk as walk
import pocket_put as put

API = '/api/pocket/v1/dev/trial-lease'
SLOT_BYTES = 0x640000
LAYOUTS = {'X3': (0x10000, 0x780000, 0x770000), 'X4': (0x10000, 0x650000, 0x640000)}
CAPABILITY = b'POCKET_TRIAL_LEASE_V4\0'
HERE = Path(__file__).resolve()


def need(value, reason):
    if not value:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def save(path, value):
    walk.save_json(path, value)


def one_shot(plan_path, name, p, output):
    marker = plan_path.parent/('.'+name+'.used')
    fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump({'run': p['run'], 'output': str(output.resolve()),
                   'imageSHA256': p['imageSHA256'], 'intentUnix': time.time()}, stream)
        stream.flush()
        os.fsync(stream.fileno())


def versions(data):
    return {x.decode('ascii') for x in re.findall(
        rb'(?:Starting CrossPoint version |CrossPoint version: |PocketDaily-ESP32-)'
        rb'([A-Za-z0-9._+\-]{1,160})(?:\r?\n)?\x00', data)}


def plan(args):
    need(not args.output.exists(), 'Fresh plan output required')
    cfg = walk.load_json(args.config)
    walk.dev_pipeline.private_host(cfg['host'])
    need(re.fullmatch(r'[A-Za-z0-9]{8}', cfg['deviceID']), 'Enrolled identity required')
    need(args.model in ('X3', 'X4') and '-dev-' in args.expected_version, 'Exact developer bridge required')
    data = args.firmware.read_bytes()
    v = versions(data)
    need(len(v) == 1 and '-dev-' not in next(iter(v)), 'One exact production version required')
    need(65536 <= len(data) <= LAYOUTS[args.model][2] and data[0] == 0xe9 and CAPABILITY in data,
         'Validated trial-capable production image required; old production is refused')
    app_acceptance = getattr(args, 'app_acceptance', False)
    need(30 <= args.seconds <= (900 if getattr(args, 'surface', 'sync') == 'file-transfer' or getattr(args, 'download_plan', None) or app_acceptance else 360 if getattr(args, 'reader_auto_wake', False) else 180), 'Trial duration exceeds explicit plan budget')
    surface = getattr(args, 'surface', 'sync')
    need(surface in ('sync', 'file-transfer', 'home', 'library', 'settings'), 'Unsupported trial surface')
    reader = getattr(args, 'reader_epub', None)
    need(surface == 'sync' or not reader and not getattr(args, 'reader_auto_wake', False), 'Reader and surface plans cannot mix')
    if reader:
        book = reader.read_bytes()
        need(0 < len(book) <= 4*1024*1024 and book.startswith(b'PK') and b'POCKET_TRIAL_READER_V1' in data,
             'Bounded EPUB and reader-capable production image required')
    auto_wake = getattr(args, 'reader_auto_wake', False)
    need(not app_acceptance or surface == 'sync' and not reader and not auto_wake and not getattr(args, 'download_plan', None), 'App acceptance requires standalone Sync')
    need(not auto_wake or reader and args.seconds == 360 and b'POCKET_TRIAL_AUTOWAKE_V1' in data,
         'Auto wake requires reader plan, 360 seconds and exact capability')
    args.output.mkdir(parents=True, mode=0o700)
    walk.save_bytes(args.output/'firmware.bin', data)
    run = secrets.randbelow(0xffffffff) + 1
    result = {'schema': 2, 'slotBytes': LAYOUTS[args.model][2], 'slotAddresses': list(LAYOUTS[args.model][:2]), 'host': cfg['host'], 'deviceID': cfg['deviceID'], 'model': args.model,
              'expectedVersion': args.expected_version, 'productionVersion': next(iter(v)),
              'run': run, 'seconds': args.seconds, 'imageBytes': len(data), 'imageSHA256': sha(data),
              'staging': '/.pocket-' + uuid.uuid4().hex + '.part',
              'fixture': '/Pocket trial ' + str(run) + '.txt',
              'sourceSHA256': sha(HERE.read_bytes()),
              'clientSHA256': sha(Path(walk.__file__).read_bytes()),
              'senderSHA256': sha(Path(put.__file__).read_bytes()),
              'scope': 'Firmware HTTP trial and bounded slot return; no physical-button, optical or full-SD claim.'}
    if app_acceptance:
        result.update(surface='sync', appAcceptance=True,
                      scope='Actual Mac app public protocols against an image-bound production Sync lease; no Store, physical iOS or radio handoff claim.')
    download = getattr(args, 'download_plan', None)
    if download:
        need(surface == 'sync' and not reader and not auto_wake, 'Download plan requires plain Sync')
        owner, _ = checked(download)
        need(owner.get('surface') == 'file-transfer' and owner['deviceID'] == result['deviceID'] and owner['model'] == result['model'] and owner['imageSHA256'] == result['imageSHA256'], 'Owned download source differs')
        import dev_trial_transfer as ft
        result.update(surface='sync', downloadPlan=str(download.resolve()), downloadPlanSHA256=sha(download.read_bytes()), transferSourceSHA256=sha(Path(ft.__file__).read_bytes()))
    if surface != 'sync':
        result['surface'] = surface
        result['scope'] = 'Image-bound production '+surface+' and bounded slot return; no physical-power or optical claim.'
    if surface == 'file-transfer':
        import dev_trial_transfer as ft
        result['transferSourceSHA256'] = sha(Path(ft.__file__).read_bytes())
        result['fixtures'] = ft.freeze(args.output, run)
    if reader:
        result.update(readerPages=20, readerBytes=len(book), readerSHA256=sha(book),
                      fixture='/Pocket trial '+str(run)+'.epub',
                      scope='Normal production reader 20 pages, close/reopen continuity, owned-file absence/recovery, normal sleep and timer slot return; no physical SD removal or power-button claim.')
        if auto_wake:
            result['readerAutoWake'] = True
            result['scope'] = 'Natural one-minute inactivity, native production timer-wake reader position and next page; no mechanical/battery claim.'
        walk.save_bytes(args.output/'reader.epub', book)
    save(args.output/'plan.private.json', result)
    print('Offline trial plan created; no device I/O.')


def checked(path):
    p = walk.load_json(path)
    need(p.get('schema') == 2 and p['sourceSHA256'] == sha(HERE.read_bytes()) and
         p['clientSHA256'] == sha(Path(walk.__file__).read_bytes()) and
         p['senderSHA256'] == sha(Path(put.__file__).read_bytes()), 'Plan/source binding changed')
    data = (path.parent/'firmware.bin').read_bytes()
    need(len(data) == p['imageBytes'] and sha(data) == p['imageSHA256'], 'Pinned production bytes changed')
    need(p['model'] in LAYOUTS and p.get('slotBytes') == LAYOUTS[p['model']][2] and
         p.get('slotAddresses') == list(LAYOUTS[p['model']][:2]) and type(p['seconds']) is int and 30 <= p['seconds'] <= (900 if p.get('surface') == 'file-transfer' or p.get('downloadPlan') or p.get('appAcceptance') is True else 360 if p.get('readerAutoWake') else 180) and
         type(p['run']) is int and 0 < p['run'] <= 0xffffffff, 'Invalid trial plan')
    walk.dev_pipeline.private_host(p['host'])
    need(type(p.get('deviceID')) is str and re.fullmatch(r'[A-Za-z0-9]{8}', p['deviceID']) and
         type(p.get('expectedVersion')) is str and '-dev-' in p['expectedVersion'], 'Invalid bridge identity')
    need('readerAutoWake' not in p or p['readerAutoWake'] is True and 'readerPages' in p and
         p['seconds'] == 360 and b'POCKET_TRIAL_AUTOWAKE_V1' in data, 'Invalid auto-wake capability/plan')
    surface = p.get('surface', 'sync')
    need(surface in ('sync', 'file-transfer', 'home', 'library', 'settings') and (surface == 'sync' or 'readerPages' not in p and not p.get('readerAutoWake')), 'Invalid surface plan')
    need('appAcceptance' not in p or p['appAcceptance'] is True and surface == 'sync' and 'readerPages' not in p and 'readerAutoWake' not in p and 'downloadPlan' not in p, 'Invalid standalone app acceptance plan')
    if surface == 'file-transfer':
        import dev_trial_transfer as ft
        need(p.get('transferSourceSHA256') == sha(Path(ft.__file__).read_bytes()), 'Transfer runner source binding changed')
        ft.checked_fixtures(path.parent, p)
    elif p.get('downloadPlan'):
        need(surface == 'sync' and 'fixtures' not in p and 'readerPages' not in p and not p.get('readerAutoWake'), 'Invalid download-only Sync plan')
        other_path = Path(p['downloadPlan'])
        raw = walk.load_json(other_path)
        need(raw.get('surface') == 'file-transfer' and 'downloadPlan' not in raw and sha(other_path.read_bytes()) == p.get('downloadPlanSHA256'), 'Download ownership binding changed')
        other, _ = checked(other_path)
        need(all(other[k] == p[k] for k in ('deviceID','model','imageSHA256','expectedVersion')), 'Download owner identity or image differs')
        import dev_trial_transfer as ft
        need(p.get('transferSourceSHA256') == sha(Path(ft.__file__).read_bytes()), 'Download runner source changed')
    else:
        need('fixtures' not in p and 'transferSourceSHA256' not in p, 'Unexpected transfer fixtures')
    reader = 'readerPages' in p
    if reader:
        book = (path.parent/'reader.epub').read_bytes()
        need(p['readerPages'] == 20 and len(book) == p.get('readerBytes') and sha(book) == p.get('readerSHA256') and
             0 < len(book) <= 4*1024*1024 and b'POCKET_TRIAL_READER_V1' in data,
             'Pinned reader fixture/capability differs')
    need(p.get('fixture') == '/Pocket trial '+str(p['run'])+('.epub' if reader else '.txt') and
         type(p.get('staging')) is str and re.fullmatch(r'/\.pocket-[a-f0-9]{32}\.part', p['staging']),
         'Only fresh plan-owned transfer paths are allowed')
    need(versions(data) == {p['productionVersion']} and '-dev-' not in p['productionVersion'] and
         65536 <= len(data) <= p['slotBytes'] and data[0] == 0xe9 and CAPABILITY in data,
         'Pinned target is not a trial-capable production image')
    return p, data


def status(client, p, version=None):
    s = client.get_json('/api/status')
    need(s.get('deviceID') == p['deviceID'] and s.get('device') == p['model'] and s.get('mode') == 'STA',
         'Reader identity/model/mode differs')
    if version is not None:
        need(s.get('version') == version, 'Exact firmware version differs')
    return s


def backup_image(client, p, out, source):
    before = status(client, p, p['expectedVersion'])
    layout = client.get_json(API)
    need(layout.get('schema') == 4 and layout.get('capability') == CAPABILITY[:-1].decode() and
         sorted([layout.get('runningAddress', -1), layout.get('nextAddress', -1)]) == p['slotAddresses'] and
         layout.get('runningBytes') == p['slotBytes'] and layout.get('nextBytes') == p['slotBytes'],
         'Actual device layout differs; never reuse another model backup')
    source_bytes = source.read_bytes()
    need(65536 <= len(source_bytes) <= p['slotBytes'] and
         put.developer_version(source_bytes) == p['expectedVersion'], 'Return artifact version/size differs')
    rows = []
    digest = hashlib.sha256()
    with (out/'return-slot.private.bin').open('xb') as f:
        for offset in range(0, p['slotBytes'], 4096):
            b, _ = client.request('GET', API, {'imageOffset': offset}, limit=4096)
            need(len(b) == min(4096, p['slotBytes']-offset), 'Incomplete current-slot piece')
            if offset < len(source_bytes):
                need(b[:min(len(b), len(source_bytes)-offset)] == source_bytes[offset:offset+len(b)],
                     'Actual return firmware prefix differs from reviewed artifact')
            f.write(b)
            digest.update(b)
            rows.append({'offset': offset, 'bytes': len(b), 'sha256': sha(b)})
        f.flush()
        os.fsync(f.fileno())
    after = status(client, p, p['expectedVersion'])
    need(before['deviceID'] == after['deviceID'] and after.get('uptime', 0) >= before.get('uptime', 0),
         'Reader changed/restarted during image backup')
    receipt = {'completed': True, 'readOnly': True, 'bytes': p['slotBytes'], 'sha256': digest.hexdigest(),
               'reviewedArtifactSHA256': sha(source_bytes), 'prefixExact': True,
               'identitySHA256': sha(p['deviceID'].encode()), 'expectedVersion': p['expectedVersion'],
               'scope': 'Current app slot only; no NVS, bootloader or fullflash backup', 'pieces': rows}
    save(out/'receipt.json', receipt)
    return receipt


def command(args):
    p, data = checked(args.plan)
    need(args.execute, 'Explicit --execute required')
    out = args.output or args.plan.parent/args.command
    need(not out.exists(), 'Fresh output required; never replay a mutation')
    out.mkdir(parents=True, mode=0o700)
    client = walk.Client(p)
    record = {'completed': False, 'command': args.command, 'run': p['run'], 'startedUnix': time.time(),
              'networkMutationIntents': 0, 'noAutomaticMutationRetry': True}
    save(out/'intent.json', record)
    try:
        if args.command == 'observe':
            record['networkMutationIntents'] = 2 if args.transfer else 0
            save(out/'intent.json', record)
            observe(client, p, out, args.transfer, args.plan.parent)
        else:
            status(client, p, p['expectedVersion'])
            if args.command == 'image-backup':
                need(args.return_image is not None, 'Reviewed return artifact required')
                backup_image(client, p, out, args.return_image)
            elif args.command == 'mode':
                record['networkMutationIntents'] = 1
                save(out/'intent.json', record)
                client.request('POST', '/api/pocket/v1/dev/transfer-mode',
                               body={'mode': args.mode, 'restartAcknowledged': True}, expected=202)
            elif args.command == 'stage':
                need(args.backup_receipt and args.backup_receipt.is_file(), 'Known-state backup receipt required')
                backup = walk.load_json(args.backup_receipt)
                need(backup.get('completed') is True and backup.get('identitySHA256') == sha(p['deviceID'].encode()),
                     'Backup must bind this enrolled reader')
                save(out/'backup-binding.private.json', {'path': str(args.backup_receipt.resolve()),
                                                        'sha256': sha(args.backup_receipt.read_bytes())})
                s = status(client, p, p['expectedVersion'])
                record['networkMutationIntents'] = 2+2*len(p['fixtures']) if p.get('surface') == 'file-transfer' else 4 if 'readerPages' in p else 2
                save(out/'intent.json', record)
                one_shot(args.plan, 'stage', p, out)
                with (out/'transfer.private.log').open('x') as log, redirect_stdout(log):
                    if p.get('surface') == 'file-transfer':
                        import dev_trial_transfer as ft
                        ft.stage(client, p, args.plan.parent, s, out)
                    if 'readerPages' in p:
                        need(all('/'+r['name'] != p['fixture'] for r in walk.inventory(client, p['deviceID'])),
                             'Owned reader fixture already exists')
                        book = (args.plan.parent/'reader.epub').read_bytes()
                        staging = '/.pocket-'+uuid.uuid4().hex+'.part'
                        put.upload_once(p['host'], s, book, staging, False, 0)
                        put.commit(p['host'], staging, p['fixture'], book)
                    put.upload_once(p['host'], s, data, p['staging'], False, 0)
                    put.commit(p['host'], p['staging'], '/update.bin', data)
            elif args.command == 'start':
                need(args.return_backup and args.return_backup.is_file(), 'Whole return-slot backup required')
                b = walk.load_json(args.return_backup)
                need(b.get('completed') is True and b.get('prefixExact') is True and b.get('bytes') == p['slotBytes'] and
                     b.get('identitySHA256') == sha(p['deviceID'].encode()) and
                     b.get('expectedVersion') == p['expectedVersion'],
                     'Complete return-slot backup required')
                need(client.get_json(API).get('present') is False, 'Existing trial must be inspected and closed')
                body = {'run': p['run'], 'seconds': p['seconds'], 'imageSHA256': p['imageSHA256'],
                        'returnSHA256': b['sha256'],
                        'backupAcknowledged': True, 'installAcknowledged': True}
                if p.get('surface'):
                    body['surface'] = p['surface']
                if 'readerPages' in p:
                    body.update({k:p[k] for k in ('readerPages','readerBytes','readerSHA256')})
                    if p.get('readerAutoWake'): body['readerAutoWake'] = True
                save(out/'return-backup-binding.private.json' , {'path': str(args.return_backup.resolve()),
                                                               'sha256': sha(args.return_backup.read_bytes()),
                                                               'returnSHA256': b['sha256']})
                record['networkMutationIntents'] = 1
                save(out/'intent.json', record)
                # Validation/hashing happens before acknowledgement. A timeout
                # is an unknown outcome, never grounds for a repeated POST.
                req = urllib.request.Request('http://'+p['host']+API+'?'+urllib.parse.urlencode({'deviceID': p['deviceID']}),
                                             data=json.dumps(body).encode(), method='POST',
                                             headers={'Content-Type': 'application/json', 'Connection': 'close'})
                one_shot(args.plan, 'start', p, out)
                with client.opener.open(req, timeout=90) as reply:
                    need(reply.status == 202, 'Trial was not accepted')
                    walk.save_bytes(out/'response.private.bin', reply.read(1024))
            elif args.command == 'cleanup':
                need(p.get('surface') == 'file-transfer' or p.get('downloadPlan'), 'Only explicit transfer-fixture cleanup is supported')
                import dev_trial_transfer as ft
                owner = walk.load_json(Path(p['downloadPlan'])) if p.get('downloadPlan') else p
                owner_directory = Path(p['downloadPlan']).parent if p.get('downloadPlan') else args.plan.parent
                record['networkMutationIntents'] = len(owner['fixtures'])
                save(out/'intent.json', record)
                one_shot(args.plan, 'cleanup', p, out)
                ft.cleanup(client, owner, owner_directory, out, terminal_plan=p)
            elif args.command == 'clear':
                r = client.get_json(API)
                need(r.get('present') is True and r.get('run') == p['run'] and r.get('phase') in (4, 5),
                     'Matching terminal record required')
                save(out/'terminal.private.json', r)
                record['networkMutationIntents'] = 1
                save(out/'intent.json', record)
                client.request('DELETE', API, body={'run': p['run'], 'backupAcknowledged': True})
                need(client.get_json(API).get('present') is False, 'Trial record remains')
        record.update(completed=True, finishedUnix=time.time())
        save(out/'command-receipt.json', record)
        print('Explicit trial step completed; inspect saved receipt.')
    except BaseException as error:
        record.update(outcome='unknown-or-refused; inspect evidence; no retry', errorType=type(error).__name__)
        save(out/'command-receipt.json', record)
        walk.save_bytes(out/'failure.private.txt', traceback.format_exc().encode())
        raise


def observe(client, p, out, transfer, plan_directory=None):
    end = time.monotonic()+p['seconds']*(2 if p.get('readerAutoWake') else 1)+180
    observations = []
    production = False
    transferred = False
    content_checked = False
    while time.monotonic() < end:
        try:
            s = status(client, p)
        except (OSError, TimeoutError):
            observations.append({'transportUnavailable': True})
            save(out/'observations.private.json', observations)
            time.sleep(2)
            continue
        observations.append({k: s.get(k) for k in ('version', 'uptime', 'trialLease', 'lastResetReason')})
        save(out/'observations.private.json', observations)
        if s.get('version') == p['productionVersion']:
            lease = s.get('trialLease', {})
            need(lease.get('version') == 4 and lease.get('run') == p['run'] and lease.get('imageSHA256') == p['imageSHA256'] and
                 lease.get('seconds') == p['seconds'] and lease.get('slot') in p['slotAddresses'],
                 'Production boot lacks matching verified-image witness')
            surface = p.get('surface', 'sync')
            need(lease.get('surface', 'sync') == surface, 'Wrong production boot surface')
            need(surface != 'file-transfer' or s.get('sessionEnd') is False, 'Expected production File Transfer profile')
            if not production:
                save(out/'production-status.private.json', s)
                for route in (() if surface != 'sync' else ('reading', 'profile', 'preferences', 'content/state')):
                    save(out/('production-'+route.replace('/', '-')+'.private.json'),
                         client.get_json('/api/pocket/v1/'+route))
                production = True
            if p.get('surface') == 'file-transfer' and not transferred:
                import dev_trial_transfer as ft
                need(plan_directory is not None, 'Pinned fixture directory required')
                ft.exercise(client, p, out, plan_directory)
                transferred = True
            if transfer and not transferred:
                inventory = walk.inventory(client, p['deviceID'])
                need(all('/'+r['name'] != p['fixture'] for r in inventory), 'Owned transfer target already exists')
                payload = ('Pocket production transfer '+str(p['run'])+'\n').encode()*32
                staging = '/.pocket-'+uuid.uuid4().hex+'.part'
                save(out/'transfer-intent.private.json', {'target': p['fixture'], 'staging': staging,
                                                         'bytes': len(payload), 'sha256': sha(payload)})
                with (out/'transfer.private.log').open('x') as log, redirect_stdout(log):
                    put.upload_once(p['host'], s, payload, staging, False, 0)
                    put.commit(p['host'], staging, p['fixture'], payload)
                actual, _ = client.request('GET', '/api/pocket/v1/files/content',
                                           {'path': p['fixture'], 'offset': 0, 'size': len(payload)}, limit=4096)
                need(actual == payload, 'Production transferred bytes differ')
                save(out/'transfer-receipt.private.json', {'completed': True, 'target': p['fixture'],
                                                         'bytes': len(payload), 'sha256': sha(payload)})
                transferred = True
            if p.get('downloadPlan') and not content_checked:
                import dev_trial_transfer as ft
                owner = walk.load_json(Path(p['downloadPlan']))
                ft.exercise_content(client, p, owner, out)
                content_checked = True
        elif s.get('version') == p['expectedVersion']:
            r = client.get_json(API)
            if r.get('present') and r.get('run') == p['run'] and r.get('phase') in (4, 5):
                save(out/'returned.private.json', r)
                need(r.get('surface', 'sync') == p.get('surface', 'sync') and r['phase'] == 4 and r.get('failure') == 0 and r.get('trialSHA256') == p['imageSHA256'] and
                         (r.get('resetReason') == 3 or (r.get('resetReason') == 8 and r.get('wakeCause') == 4)),
                     'Trial returned with failure evidence')
                if p.get('surface') in ('home','library','settings'):
                    need(r.get('surfaceFree', 0) > 0 and r.get('surfaceLargest', 0) > 0, 'UI capture or memory checkpoint missing')
                    if p['surface'] == 'home':
                        need(r['surfaceFree'] > 50*1024 and r['surfaceLargest'] >= 8192, 'Home memory gate failed')
                    production = True
                if 'readerPages' in p:
                    evidence = r.get('reader', {})
                    need(evidence.get('schema') == 1 and evidence.get('run') == p['run'] and evidence.get('stage') == 7 and
                         evidence.get('requested') == 20 and evidence.get('rendered') == 20 and
                         all(evidence.get(k) == 1 for k in ('reopened','missingObserved','recovered')) and
                         ((r.get('resetReason') == 3) if p.get('readerAutoWake') else
                          (r.get('resetReason') == 8 and r.get('wakeCause') == 4)),
                         'Production reader, continuity, bounded file recovery or timer sleep evidence failed')
                    if p.get('readerAutoWake'):
                        wake = evidence.get('autoWake', {})
                        need(wake.get('schema') == 1 and wake.get('stage') == 6 and wake.get('timeoutMs') == 60000 and
                             type(wake.get('idleMs')) is int and 60000 <= wake['idleMs'] < 180000 and
                             wake.get('resetReason') == 8 and wake.get('wakeCause') == 4 and
                             wake.get('positionMatched') == 1 and wake.get('pageAdvanced') == 1,
                             'Natural idle/native production resume/next page evidence missing')
                    need(evidence.get('memorySamples', 0) >= 20 and evidence.get('renderMemorySamples', 0) >= 19 and 50*1024 < evidence.get('minFreeHeap', 0) < 0xffffffff and 0 < evidence.get('minLargestBlock', 0) < 0xffffffff, 'Reading memory evidence missing or below its gate')
                    production = True  # Verified on-device lease/image witness, not an HTTP observation while reading.
                need(production and (not p.get('downloadPlan') or content_checked) and (not transfer or transferred) and (p.get('surface') != 'file-transfer' or transferred), 'Required production evidence missing')
                save(out/'receipt.json', {'completed': True, 'productionObserved': production,
                                         'transferVerified': transferred, 'returnedRun': p['run'],
                                         'scope': p['scope'], 'cleanupPending': transferred or 'readerPages' in p})
                return
        else:
            raise ValueError('Unexpected firmware version; preserve state')
        time.sleep(2)
    raise TimeoutError('Bounded observation ended; no mutation was replayed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('plan')
    p.add_argument('--config', type=Path, required=True)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--model', choices=('X3', 'X4'), required=True)
    p.add_argument('--expected-version', required=True)
    p.add_argument('--seconds', type=int, default=90)
    p.add_argument('--surface', choices=('sync','file-transfer','home','library','settings'), default='sync')
    p.add_argument('--download-plan', type=Path, help='Existing same-device/image File Transfer fixture plan; download only')
    p.add_argument('--app-acceptance', action='store_true', help='Explicit bounded Sync lease for an externally owned actual-app public-protocol test; no reader/download fixture scenarios')
    p.add_argument('--reader-epub', type=Path)
    p.add_argument('--reader-auto-wake', action='store_true')
    p.add_argument('--output', type=Path, required=True)
    for name in ('image-backup', 'mode', 'stage', 'start', 'observe', 'cleanup', 'clear'):
        p = sub.add_parser(name)
        p.add_argument('--plan', type=Path, required=True)
        p.add_argument('--output', type=Path)
        p.add_argument('--execute', action='store_true')
        p.add_argument('--mode', choices=('sync', 'file-transfer'), default='sync')
        p.add_argument('--return-image', type=Path)
        p.add_argument('--return-backup', type=Path)
        p.add_argument('--backup-receipt', type=Path)
        p.add_argument('--transfer', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    plan(args) if args.command == 'plan' else command(args)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('Trial step stopped: '+type(error).__name__+'; inspect private evidence; no automatic retry.', file=sys.stderr)
        sys.exit(1)
