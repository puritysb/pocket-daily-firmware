"""Run-bound production File Transfer acceptance; no mutation retries."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import socket
import struct
import time
import urllib.parse
import uuid
import zipfile

import dev_reader_walk as walk
import pocket_put as put

MIB = 1024 * 1024
SPECS = (('one.epub', 'one.epub', MIB), ('twenty.epub', 'twenty.epub', 20*MIB),
         ('boundary.txt', 'boundary.txt', 4097), ('unicode.txt', '경계.txt', 4099))


def need(value, reason):
    if not value:
        raise ValueError(reason)


def epub(size):
    files = [('mimetype', b'application/epub+zip'),
             ('META-INF/container.xml', b'<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'),
             ('book.opf', b'<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">pocket-transfer-fixture</dc:identifier><dc:title>Pocket transfer fixture</dc:title><dc:language>en</dc:language><meta property="dcterms:modified">2026-10-28T00:00:00Z</meta></metadata><manifest><item id="text" href="book.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest><spine><itemref idref="text"/></spine></package>'),
             ('nav.xhtml', b'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc"><ol><li><a href="book.xhtml">Transfer</a></li></ol></nav></body></html>')]
    prefix = b'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Transfer</title></head><body><p>'
    suffix = b'</p></body></html>'

    def pack(text):
        out = io.BytesIO()
        with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_STORED) as archive:
            for name, data in files + [('book.xhtml', prefix + text + suffix)]:
                archive.writestr(zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0)), data)
        return out.getvalue()
    overhead = len(pack(b''))
    pattern = b''.join(('Checkpoint %06d: Pocket transfer integrity.\n' % i).encode() for i in range(997))
    n = size - overhead
    need(n > 0, 'EPUB fixture too small')
    data = pack((pattern * ((n + len(pattern)-1)//len(pattern)))[:n])
    need(len(data) == size, 'Fixture size mismatch')
    return data


def freeze(directory, run):
    rows = []
    for local, name, size in SPECS:
        data = epub(size) if name.endswith('.epub') else bytes(32 + (i*37+i//31) % 95 for i in range(size))
        walk.save_bytes(directory/local, data)
        rows.append(dict(local=local, path=f'/Pocket trial {run} {name}', bytes=size, sha256=walk.digest(data)))
    return rows


def checked_fixtures(directory, plan):
    rows = plan.get('fixtures', [])
    need(len(rows) == len(SPECS), 'Exact owned fixture set required')
    for row, (local, name, size) in zip(rows, SPECS):
        need(row.get('local') == local and row.get('path') == f"/Pocket trial {plan['run']} {name}" and
             row.get('bytes') == size, 'Fixture path or size changed')
        data = (directory/local).read_bytes()
        need(len(data) == size and walk.digest(data) == row.get('sha256'), 'Fixture bytes changed')
    return rows


def stage(client, plan, directory, status, out):
    rows = checked_fixtures(directory, plan)
    existing = {('/'+row['name']).casefold() for row in walk.inventory(client, plan['deviceID'])}
    need(all(row['path'].casefold() not in existing for row in rows), 'Owned fixture already exists')
    for index, row in enumerate(rows):
        staging = '/.pocket-'+uuid.uuid4().hex+'.part'
        walk.save_json(out/f'fixture-{index}-intent.private.json', {**row, 'staging':staging, 'noRetry':True})
        with (out/f'fixture-{index}-transport.private.log').open('x') as log, redirect_stdout(log):
            data = (directory/row['local']).read_bytes()
            put.upload_once(plan['host'], status, data, staging, False, 0)
            put.commit(plan['host'], staging, row['path'], data)


def receive(sock, expected, http='HTTP/1.1', mode='normal', budget=600):
    started = time.monotonic()
    raw = bytearray()
    while b'\r\n\r\n' not in raw:
        part = sock.recv(512)
        need(part and len(raw)+len(part) < 8192, 'Missing or unbounded HTTP header')
        raw.extend(part)
    head, pending = bytes(raw).split(b'\r\n\r\n', 1)
    lines = head.decode('iso-8859-1').split('\r\n')
    need(lines[0] == http+' 200 OK', 'Unexpected download status/version')
    headers = {}
    for line in lines[1:]:
        key, value = line.split(':', 1)
        key = key.lower()
        need(key not in headers, 'Duplicate HTTP header')
        headers[key] = value.strip()
    need(headers.get('content-length') == str(len(expected)) and headers.get('connection') == 'close' and
         'transfer-encoding' not in headers, 'Download framing mismatch')
    body = bytearray(pending)
    if mode == 'stall':
        time.sleep(7)
    while len(body) < len(expected):
        need(time.monotonic()-started < budget, 'Download total budget expired')
        try:
            part = sock.recv(1024 if mode == 'slow' else 4096)
        except ConnectionResetError:
            need(mode == 'stall', 'Unexpected connection reset')
            break
        if not part:
            break
        body.extend(part)
        if mode == 'rst' and len(body) >= 8192:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            break
        if mode == 'slow':
            time.sleep(.01)
    need(bytes(body) == expected[:len(body)], 'Wrong or extra response bytes')
    if mode == 'rst':
        need(8192 <= len(body) < len(expected), 'RST did not interrupt a partial body')
    elif mode == 'stall':
        need(len(body) < len(expected), 'Stalled peer did not enforce idle bound')
    else:
        need(bytes(body) == expected and sock.recv(1) == b'', 'Early EOF or trailing bytes')
    return {'bytes':len(body), 'expectedBytes':len(expected), 'seconds':round(time.monotonic()-started, 3),
            'kind':mode, 'http':http, 'complete':mode not in ('rst','stall'), 'automaticRetries':0}


def exercise(client, plan, out, directory):
    rows = checked_fixtures(directory, plan)
    facts = []
    before = client.get_json('/api/status')
    need(before.get('sessionEnd') is False and before.get('version') == plan['productionVersion'], 'Wrong transfer profile')
    last_uptime = before['uptime']

    def download(row, mode='normal', http='HTTP/1.1', payload=None):
        nonlocal last_uptime
        data = (directory/row['local']).read_bytes() if payload is None else payload
        query = urllib.parse.urlencode({'path':row['path'], 'deviceID':plan['deviceID']})
        with socket.socket() as sock:
            sock.settimeout(10)
            if mode in ('slow','stall'):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            sock.connect((plan['host'],80))
            sock.sendall(('GET /download?'+query+' '+http+'\r\nHost: '+plan['host']+'\r\nConnection: close\r\n\r\n').encode())
            fact = receive(sock, data, http, mode)
        after = client.get_json('/api/status')
        need(after.get('deviceID') == plan['deviceID'] and after.get('version') == plan['productionVersion'] and
             after.get('sessionEnd') is False and after['uptime'] >= last_uptime, 'Reader changed or restarted')
        need(all(type(after.get(k)) is int for k in ('freeHeap','largestFreeBlock','minimumFreeHeap')), 'Memory fields missing')
        last_uptime = after['uptime']
        fact.update(freeHeap=after['freeHeap'], largestBlock=after['largestFreeBlock'], minimumEver=after['minimumFreeHeap'])
        facts.append(fact)
        walk.save_json(out/'file-transfer-progress.json', {'facts':facts})

    listing = json.loads(client.request('GET','/api/files',{'path':'/'})[0])
    need(all(any('/'+e['name']==r['path'] and e['size']==r['bytes'] for e in listing) for r in rows), 'Fixture inventory mismatch')
    download({'path':'/.crosspoint/library.dirty'}, payload=b'')
    for _ in range(3):
        for row in (rows[0], rows[2], rows[3]):
            download(row)
    download(rows[1])
    image = (directory/'firmware.bin').read_bytes()
    download({'path':'/update.bin'}, payload=image)
    download(rows[2], http='HTTP/1.0')
    download(rows[0], mode='slow')
    for _ in range(3):
        download(rows[0], mode='rst')
        download(rows[2])
    for _ in range(2):
        download(rows[0], mode='stall')
        download(rows[2])
    walk.save_json(out/'file-transfer-receipt.json', {'completed':True,'exactProductionImage':True,
        'requests':len(facts),'fullDownloads':sum(x['complete'] for x in facts),'intentionalInterruptions':5,
        'twentyMiBWholeSHA256Verified':True,'oneMiBWholeSHA256Verified':True,'sameUptime':True,
        'automaticRetries':0,'heapBefore':before['freeHeap'],'heapMinBetweenRequests':min(x['freeHeap'] for x in facts),
        'largestMinBetweenRequests':min(x['largestBlock'] for x in facts),'facts':facts,'cleanupPending':True})


def cleanup(client, plan, directory, out, terminal_plan=None):
    rows = checked_fixtures(directory, plan)
    terminal_plan = terminal_plan or plan
    terminal = client.get_json('/api/pocket/v1/dev/trial-lease')
    need(terminal.get('run') == terminal_plan['run'] and terminal.get('phase') == 4 and terminal.get('failure') == 0 and
         terminal.get('trialSHA256') == terminal_plan['imageSHA256'] and terminal.get('surface') == terminal_plan.get('surface', 'sync'),
         'Exact successful terminal return required')
    facts = []
    for index, row in enumerate(rows):
        offset = 0
        digest = hashlib.sha256()
        while offset < row['bytes']:
            piece, _ = client.request('GET',walk.API+'files/content',{'path':row['path'],'size':row['bytes'],'offset':offset},limit=4096)
            need(0 < len(piece) <= min(4096,row['bytes']-offset), 'Incomplete owned-file cleanup check')
            offset += len(piece)
            digest.update(piece)
        need(digest.hexdigest() == row['sha256'], 'Owned-file changed; refusing delete')
        walk.save_json(out/f'delete-{index}-intent.private.json', {**row,'noRetry':True})
        client.request('DELETE',walk.API+'files',{'path':row['path'],'size':row['bytes'],'cursor':0})
        facts.append({'bytes':offset,'sha256':row['sha256']})
    names = {'/'+row['name'] for row in walk.inventory(client, plan['deviceID'])}
    need(all(row['path'] not in names for row in rows), 'Owned fixture remains')
    walk.save_json(out/'receipt.json', {'completed':True,'ownedFixtureCount':len(rows),'facts':facts,'wholeSHA256BeforeDelete':True})


def exercise_content(client, plan, owner, out):
    """Same-Wi-Fi production piece route; only complete known503 bodies back off."""
    rows = owner['fixtures']
    facts = []
    def health():
        status = client.get_json('/api/status')
        witness = status.get('trialLease', {})
        need(status.get('deviceID') == plan['deviceID'] and status.get('version') == plan['productionVersion'] and
             status.get('sessionEnd') is True and status.get('readerFiles') == 2 and witness.get('run') == plan['run'] and
             witness.get('imageSHA256') == plan['imageSHA256'], 'Piece reads left the exact production Sync trial')
        return status
    health()
    for row in rows:
        offset = 0
        digest = hashlib.sha256()
        retries = 0
        started = time.monotonic()
        while offset < row['bytes']:
            if offset % (128*4096) == 0:
                health()
            for attempt in range(8):
                try:
                    piece, headers = client.request('GET',walk.API+'files/content',
                        {'path':row['path'],'size':row['bytes'],'offset':offset},limit=4096)
                    break
                except walk.HTTPFailure as error:
                    if not (error.code == 503 and error.body == 'Reader memory is too low for the download right now' and attempt < 7):
                        raise
                    retries += 1
                    time.sleep(min(.5*(2**attempt),4))
            need(0 < len(piece) <= min(4096,row['bytes']-offset), 'Wrong piece bound')
            h = {k.lower():v for k,v in headers.items()}
            need(h.get('content-length') == str(len(piece)) and h.get('cache-control') == 'no-store' and
                 'transfer-encoding' not in h, 'Wrong piece response framing')
            digest.update(piece);offset+=len(piece)
            need(time.monotonic()-started < plan['seconds']-30, 'Piece download exceeded total budget')
        need(digest.hexdigest() == row['sha256'], 'Whole piece-assembled SHA256 mismatch')
        status = health()
        facts.append({'bytes':offset,'sha256':row['sha256'],'seconds':round(time.monotonic()-started,3),
                      'lowHeap503Backoffs':retries,'freeHeap':status['freeHeap'],'largestBlock':status['largestFreeBlock']})
        walk.save_json(out/'content-download-progress.json',{'facts':facts})
    row=rows[0]
    base={'path':row['path'],'size':row['bytes'],'offset':0}
    cases=[('changed-size',{**base,'size':row['bytes']+1},409),
           ('eof-offset',{**base,'offset':row['bytes']},416),
           ('malformed-offset',{**base,'offset':'no'},400),
           ('missing-file',{**base,'path':f"/Pocket trial {owner['run']} missing.epub"},404),
           ('firmware-type',{**base,'path':'/update.bin'},403),
           ('forbidden-path',{**base,'path':'/.crosspoint/state.json'},403)]
    refusals=[]
    for name, query, expected in cases:
        try:
            client.request('GET',walk.API+'files/content',query,limit=4096)
            raise ValueError('Invalid download was accepted')
        except walk.HTTPFailure as error:
            need(error.code == expected,'Wrong negative download response')
        refusals.append({'case':name,'status':expected})
    wrong={**plan,'deviceID':'00000000' if plan['deviceID']!='00000000' else 'FFFFFFFF'}
    try:
        walk.Client(wrong).request('GET',walk.API+'files/content',base,limit=4096)
        raise ValueError('Wrong identity accepted')
    except walk.HTTPFailure as error:
        need(error.code==409,'Wrong identity response')
    health()
    walk.save_json(out/'content-download-receipt.json',{'completed':True,'bearer':'Same Wi-Fi',
        'twentyMiBWholeSHA256Verified':True,'oneMiBWholeSHA256Verified':True,'pieceCap':4096,
        'facts':facts,'negativeCases':refusals+[{'case':'wrong-identity','status':409}],
        'busyAndForcedLowHeapInjected':False,'transportRetries':0})
