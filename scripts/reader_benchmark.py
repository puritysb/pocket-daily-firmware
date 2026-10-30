"""Bounded reader telemetry codec and conservative, offline A/B assessment.

Times are firmware milliseconds, not optical response times. Opening a book is
reported separately. One walk is the statistical unit; correlated pages are
never counted as independent repeat trials.
"""
import math
import statistics
import struct

STAGES = ('input', 'section', 'page', 'prewarm', 'bwRender', 'status',
          'bwRefresh', 'grayRender', 'grayRefresh', 'graySync', 'save', 'total')
BW_STAGES = STAGES[:7]
COUNTERS = ('sdOpens', 'sdReads', 'sdKB', 'glyphs')
MAX_PAGES = 16
MAX_BYTES = 32 + MAX_PAGES * 48


def require(condition, message):
    if not condition:
        raise ValueError(message)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def decode(data, run, pages):
    require(type(run) is int and 0 < run <= 0xffffffff and type(pages) is int
            and 1 <= pages <= MAX_PAGES, 'Invalid performance run/pages')
    require(len(data) == 32 + pages * 48, 'Incomplete performance trace')
    fields = struct.unpack_from('<8I', data)
    require(fields == (0x4d424450, 1, run, pages, 48, 0, 0, checksum(data[:28])),
            'Foreign or corrupt performance header')
    records = []
    for index in range(pages):
        raw = data[32 + index * 48:32 + (index + 1) * 48]
        values = struct.unpack('<16HBBIIHI', raw)
        require(values[-1] == checksum(raw[:44]) and values[-2] == index,
                'Corrupt or reordered performance record')
        require(bool(values[17] & 128) == (index == 0), 'Open/turn sequence mismatch')
        # Saturated telemetry cannot establish a latency/memory improvement.
        require(all(x < 65535 for x in values[:16]) and values[16] < 255 and 0 < values[11]
                and values[18] >= values[19] > 0, 'Saturated or missing performance evidence')
        records.append(dict(index=index, ms=dict(zip(STAGES, values[:12])),
                            counters=dict(zip(COUNTERS, values[12:16])), strips=values[16],
                            flags=values[17], freeHeap=values[18], largestBlock=values[19]))
    return records


def percentile(values, fraction):
    require(bool(values), 'No timing samples')
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def summary(records):
    turns = records[1:]
    require(bool(turns), 'At least two pages are required for a turn benchmark')
    # Recorded work through BW refresh, not optical first-ink latency. Sum each
    # turn before taking percentiles: stage medians do not describe one turn.
    bw_work = [sum(x['ms'][key] for key in BW_STAGES) for x in turns]
    return {'pages': len(records), 'openMs': records[0]['ms']['total'],
            'turnP50Ms': statistics.median(x['ms']['total'] for x in turns),
            'turnP95Ms': percentile([x['ms']['total'] for x in turns], .95),
            'bwWorkP50Ms': statistics.median(bw_work),
            'bwWorkP95Ms': percentile(bw_work, .95),
            'minFreeHeap': min(x['freeHeap'] for x in records),
            'minLargestBlock': min(x['largestBlock'] for x in records),
            'counterP50': {key: statistics.median(x['counters'][key] for x in turns) for key in COUNTERS},
            'stageP50Ms': {key: statistics.median(x['ms'][key] for x in turns) for key in STAGES}}


def assess(pairs, minimum_gain=.10):
    """Pairs are verified same-fixture walks; reject each repeat's regressions.

    This screening is intentionally not automatic product acceptance: final BW
    captures cannot validate every page, grayscale waveform or panel ghosting.
    """
    require(.01 <= minimum_gain <= .9, 'Invalid minimum gain')
    require(len(pairs) >= 5, 'At least five paired walks per fixture required')
    reasons, gains, trials = [], [], []
    for baseline, candidate in pairs:
        a, b = summary(baseline), summary(candidate)
        require(a['pages'] == b['pages'], 'Page count changed')
        # Refresh/AA/image mode must not be silently sacrificed for speed.
        if any((x['flags'] & 0x3c, x['strips']) != (y['flags'] & 0x3c, y['strips'])
               for x, y in zip(baseline, candidate)):
            reasons.append('render-mode-changed')
        if b['minFreeHeap'] <= 50000 or b['minFreeHeap'] < a['minFreeHeap']:
            reasons.append('free-heap-regression')
        if b['minLargestBlock'] < a['minLargestBlock']:
            reasons.append('largest-block-regression')
        if b['openMs'] > a['openMs'] * 1.05:
            reasons.append('opening-regression')
        if b['turnP95Ms'] > a['turnP95Ms'] * 1.05:
            reasons.append('tail-latency-regression')
        gains.append(1 - b['turnP50Ms'] / a['turnP50Ms'])
        trials.append({'baseline': a, 'candidate': b, 'gain': gains[-1]})
    # All repeats must improve; do not cherry-pick a best run or pool pages.
    if min(gains) < minimum_gain:
        reasons.append('repeatable-gain-below-threshold')
    return {'timingScreenPassed': not reasons, 'reasons': sorted(set(reasons)),
            'pairedWalks': len(pairs), 'trials': trials, 'medianGain': statistics.median(gains),
            'worstPairGain': min(gains), 'minimumRequiredGain': minimum_gain,
            'automaticAcceptance': False, 'productionAcceptance': False,
            'remainingGates': ['all-page-output', 'optical-ghosting', 'battery-energy',
                               'physical-button-response', 'all-orientations', 'normal-progress-saves']}
