#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Public SDK command acceptance in a caller-owned firmware emulator.

Transport/normal-loader trust and PID/birth ownership belong to the supplied
callbacks. No SSH target, physical device, or private native entry is selected
by this module. Synthetic parser tests are not firmware acceptance.
"""
from sdk_timestamps import compare_timestamps
import math
import re
import time

from sdk_emulator_controls import MissingEvidence

FIELDS = set('v seq event id deck op target_load rc outcome reason revision receipt_ns query_ns load_valid native_load load_unit load_ns loop_unit loop_rev loop_ns engaged_valid engaged bounds_valid in out'.split())
EVENTS = {'submit', 'poll', 'cancel', 'release', 'snapshot', 'refused', 'terminal', 'acknowledge', 'released'}
UINT = re.compile(r'0|[1-9][0-9]*')
INT = re.compile(r'0|[1-9][0-9]*|-[1-9][0-9]*')
DECIMAL = re.compile(r'-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?')
ALL_COMMANDS = (1 << 13) - 2


def records(text, *, sample_rate=None):
    if not isinstance(text, str) or len(text.encode()) > 2097152:
        raise ValueError('bounded complete command transcript required')
    result = []
    previous_query = 0
    for line in text.splitlines():
        if not line.startswith('SDK_LOOP '):
            continue
        pairs = [p.split('=', 1) for p in line.split()[1:]]
        if len(line) > 2048 or any(len(p) != 2 for p in pairs) or len(pairs) != len(FIELDS) or {p[0] for p in pairs} != FIELDS:
            raise ValueError('exact loop receipt fields required')
        raw = dict(pairs)
        if raw['event'] not in EVENTS:
            raise ValueError('unknown loop receipt event')
        row = {'event': raw['event']}
        for key, value in raw.items():
            if key == 'event':
                continue
            if key in ('in', 'out'):
                if not DECIMAL.fullmatch(value) or not math.isfinite(float(value)):
                    raise ValueError('finite source coordinates required')
                row[key] = float(value)
            else:
                if not INT.fullmatch(value):
                    raise ValueError('canonical integer loop receipts required')
                row[key] = int(value)
                signed = key in ('rc', 'outcome', 'reason', 'engaged')
                if not (-(2**31) <= row[key] < 2**31 if signed else 0 <= row[key] < 2**64):
                    raise ValueError('loop receipt integer out of range')
        if row['v'] != 1 or row['seq'] != len(result) + 1 or row['seq'] > 4096 or row['deck'] not in range(4) or row['op'] not in range(13):
            raise ValueError('loop version/sequence/deck/operation mismatch')
        if not row['query_ns'] or compare_timestamps(row['query_ns'], previous_query) < 0 or compare_timestamps(row['receipt_ns'], row['query_ns']) > 0:
            raise ValueError('backward/future loop receipt clock')
        previous_query = row['query_ns']
        for flag in ('load_valid', 'engaged_valid', 'bounds_valid'):
            if row[flag] not in (0, 1):
                raise ValueError('boolean snapshot availability required')
        if row['load_valid'] and (not all(row[k] for k in ('native_load', 'load_unit', 'load_ns')) or compare_timestamps(row['load_ns'], row['query_ns']) > 0):
            raise ValueError('unqualified native load receipt')
        if (row['engaged_valid'] or row['bounds_valid']) and (not all(row[k] for k in ('loop_unit', 'loop_rev', 'loop_ns')) or compare_timestamps(row['loop_ns'], row['query_ns']) > 0):
            raise ValueError('unqualified loop observer receipt')
        if row['engaged_valid'] and row['engaged'] not in (0, 1):
            raise ValueError('invalid loop engagement')
        if row['bounds_valid']:
            if type(sample_rate) is not int or sample_rate <= 0:
                raise MissingEvidence('bound source sample rate required for loop coordinates')
            if compare_timestamps(row['in'], 0., units_per_second=1, sample_rate=sample_rate) < 0 or compare_timestamps(row['in'], row['out'], units_per_second=1, sample_rate=sample_rate) >= 0:
                raise ValueError('invalid working loop coordinates')
        if row['outcome'] not in (-1, 0, 1, 2, 3, 4):
            raise ValueError('unknown command outcome')
        if row['outcome'] == 1 and (row['reason'] != 0 or not row['revision'] or not row['receipt_ns']):
            raise ValueError('unqualified APPLIED command receipt')
        if row['event'] == 'snapshot' and any(row[k] for k in ('id', 'op', 'target_load', 'revision', 'receipt_ns')):
            raise ValueError('snapshot cannot impersonate a command receipt')
        result.append(row)
    return result


def ready(text, pid):
    found = []
    for line in text.splitlines():
        if not line.startswith('SDK_LOOP_READY '):
            continue
        pairs = [p.split('=', 1) for p in line.split()[1:]]
        if len(pairs) != 4 or {p[0] for p in pairs if len(p) == 2} != {'v', 'pid', 'commands', 'data'}:
            raise ValueError('exact command-probe readiness required')
        row = dict(pairs)
        if row['v'] != '1' or row['pid'] != str(pid) or not UINT.fullmatch(row['commands']):
            raise ValueError('command probe process/version mismatch')
        if not re.fullmatch(r'/[A-Za-z0-9_./-]+/sdk-loop-command-probe/data', row['data']) or '..' in row['data'].split('/'):
            raise ValueError('owned mod data directory required')
        row['commands'] = int(row['commands'])
        if not 0 <= row['commands'] < 2**64:
            raise ValueError('command capability range exceeded')
        found.append(row)
    if not found:
        raise MissingEvidence('normal-loaded command probe readiness record required')
    if len(found) > 32 or any(row['data'] != found[0]['data'] for row in found):
        raise ValueError('command-probe readiness identity changed')
    return found[-1]


def request(pid, action, request_id=0, deck=0, operation=0, load=0, deadline_ns=0, position=0, end=0, beats=0):
    if not re.fullmatch('[1-9][0-9]*', str(pid)) or action not in ('submit', 'cancel', 'snapshot'):
        raise ValueError('pinned native PID and reviewed probe action required')
    if any(type(v) is not int or not 0 <= v < 2**64 for v in (request_id, deck, operation, load, deadline_ns)) or deck not in range(4):
        raise ValueError('bounded scalar request required')
    if any(type(v) not in (int, float) or not math.isfinite(v) for v in (position, end, beats)):
        raise ValueError('finite command arguments required')
    if action == 'submit' and (not request_id or operation not in range(1, 13) or not load or not deadline_ns):
        raise ValueError('complete SDK submit identity required')
    if action != 'submit' and (operation or load or deadline_ns or position or end or beats or (request_id if action == 'snapshot' else not request_id)):
        raise ValueError('unused control arguments must be zero')
    text = (f'v=1 pid={pid} action={action} id={request_id} deck={deck} op={operation} load={load} '
            f'deadline_ns={deadline_ns} position={position:.17g} end={end:.17g} beats={beats:.17g}\n')
    if len(text.encode()) > 511:
        raise ValueError('probe request byte cap exceeded')
    return text


def applied(before, after, operation, target_load, observed=None, *, sample_rate=None):
    """Check public receipts plus independently available working-loop fields.

    v1 has no retained-point or beat-rational reader. IN/CLEAR and beat ratios
    therefore retain the backend's correlated receipt as their exact witness.
    """
    if after['rc'] != 0 or after['outcome'] != 1 or after['reason'] != 0 or not after['revision']:
        raise ValueError('command did not reach APPLIED')
    if not after['load_valid'] or after['native_load'] != target_load or after['target_load'] != target_load or after['load_unit'] != before['load_unit']:
        raise ValueError('command receipt belongs to a different native load')
    if observed is not None:
        if not observed['load_valid'] or observed['native_load'] != target_load or observed['load_unit'] != before['load_unit']:
            raise ValueError('post-command observation belongs to a different load/unit')
        if before['loop_unit'] and observed['loop_unit'] and observed['loop_unit'] != before['loop_unit']:
            raise ValueError('post-command loop observer unit changed')
        after = observed
    if operation in (2, 3, 5) and (not after['engaged_valid'] or after['engaged'] != 1):
        raise ValueError('loop entry has no engaged snapshot witness')
    if operation == 4 and (not after['engaged_valid'] or after['engaged'] != 0):
        raise ValueError('EXIT has no inactive snapshot witness')
    if operation in (7, 8, 9, 10, 11, 12):
        if type(sample_rate) is not int or sample_rate <= 0:
            raise MissingEvidence('bound source sample rate required for loop timestamps')
        if not before['bounds_valid'] or not after['bounds_valid']:
            raise MissingEvidence('working bounds unavailable for adjustment verification')
        same_in = compare_timestamps(after['in'], before['in'], units_per_second=1, sample_rate=sample_rate) == 0
        same_out = compare_timestamps(after['out'], before['out'], units_per_second=1, sample_rate=sample_rate) == 0
        if operation in (7, 8, 9, 12) and (not same_in or same_out):
            raise ValueError('resize failed its independent boundary witness')
        if operation == 11 and (same_in or not same_out):
            raise ValueError('IN adjustment failed its independent boundary witness')
        if operation == 10 and (same_in or same_out or compare_timestamps(after['out'] - after['in'], before['out'] - before['in'], units_per_second=1, sample_rate=sample_rate) != 0):
            raise ValueError('MOVE failed its independent boundary witness')
    return True


class LoopClient:
    def __init__(self, read_log, send_request, health, pid, *, sleep=time.sleep, clock=time.monotonic, timeout=15, sample_rate=None):
        if not all(callable(f) for f in (read_log, send_request, health, sleep, clock)) or not 1 <= timeout <= 30:
            raise ValueError('bounded owned transport callbacks required')
        self.read_log, self.send_request, self.health = read_log, send_request, health
        self.pid, self.sleep, self.clock, self.timeout = pid, sleep, clock, timeout
        self.sample_rate = sample_rate
        self.next_id = 0
        self.evidence = []
        self.last_rows = []
        self.last_text = ""

    def read(self):
        if self.health() is not True:
            raise ValueError('owned native process identity/health changed')
        text = self.read_log()
        ready(text, self.pid)
        self.last_text = text
        self.last_rows = records(text, sample_rate=self.sample_rate)
        return self.last_rows

    def wait(self, predicate):
        deadline = self.clock() + self.timeout
        while True:
            rows = self.read()
            value = next((row for row in rows if predicate(row)), None)
            if value is not None:
                return value
            if compare_timestamps(self.clock(), deadline, units_per_second=1) >= 0:
                raise MissingEvidence('bounded command receipt wait expired')
            self.sleep(0.1)

    def snapshot(self, deck):
        rows = self.read()
        fence = rows[-1]['seq'] if rows else 0
        self.send_request(request(self.pid, 'snapshot', deck=deck))
        return self.wait(lambda r: r['seq'] > fence and r['event'] == 'snapshot' and r['deck'] == deck)

    def submit(self, deck, operation, **arguments):
        if operation in (7, 8, 9, 10, 11, 12) and (type(self.sample_rate) is not int or self.sample_rate <= 0):
            raise MissingEvidence('bound source sample rate required before loop adjustment')
        before = self.snapshot(deck)
        if not before['load_valid']:
            raise MissingEvidence('valid native load required before submitting a command')
        self.next_id = max(self.next_id, max((r['id'] for r in self.last_rows), default=0)) + 1
        if self.next_id >= 2**64:
            raise ValueError('probe request ID space exhausted')
        identity = self.next_id
        self.send_request(request(self.pid, 'submit', identity, deck, operation, before['native_load'], before['query_ns'] + 10_000_000_000, **arguments))
        terminal = self.wait(lambda r: r['id'] == identity and r['event'] in ('submit', 'poll', 'terminal', 'refused') and r['outcome'] != 0)
        if terminal['deck'] != deck or terminal['op'] != operation:
            raise ValueError('cross-request command receipt')
        # Validate result identity before permitting any state-observer retry.
        if terminal['rc'] != 0 or terminal['outcome'] != 1 or terminal['reason'] != 0 or not terminal['revision']:
            raise ValueError('command did not reach APPLIED')
        if terminal['target_load'] != before['native_load'] or not terminal['load_valid'] or terminal['native_load'] != before['native_load']:
            raise ValueError('command receipt belongs to a different native load')
        released = self.wait(lambda r: r['id'] == identity and r['event'] in ('release', 'released') and r['rc'] == 0)
        observed = terminal
        if operation not in (1, 6):
            deadline = self.clock() + self.timeout
            while True:
                observed = self.snapshot(deck)
                newer = operation == 4 or (compare_timestamps(observed['loop_ns'], before['loop_ns']) > 0 and observed['loop_rev'] > before['loop_rev'])
                available = observed['engaged_valid'] if operation in (2, 3, 4, 5) else observed['bounds_valid']
                if available and newer:
                    break
                if compare_timestamps(self.clock(), deadline, units_per_second=1) >= 0:
                    raise MissingEvidence('fresh post-command working-loop observation unavailable')
                self.sleep(.1)
        applied(before, terminal, operation, before['native_load'], observed, sample_rate=self.sample_rate)
        self.evidence.append(dict(deck=deck, operation=operation, request_id=identity, before=before, terminal=terminal, observed=observed, release=released))
        return terminal


def run_commands(read_log, send_request, health, pid, *, decks=(0, 1, 2, 3), sleep=time.sleep, clock=time.monotonic, setup_deck=None, sample_rate=None):
    """Run all twelve commands after the caller establishes actual loaded playback.

    setup_deck may prepare the selected deck with actual panel inputs. It must
    never manufacture SDK load generations or modify native memory.
    """
    if not decks or len(set(decks)) != len(decks) or any(d not in range(4) for d in decks):
        raise ValueError('explicit unique logical decks required')
    if type(sample_rate) is not int or sample_rate <= 0:
        raise MissingEvidence('bound source sample rate required before loop commands')
    client = LoopClient(read_log, send_request, health, pid, sleep=sleep, clock=clock, sample_rate=sample_rate)
    capability = ready(read_log(), pid)['commands']
    report = dict(status='INCOMPLETE', scope='real public SDK loop-command receipts and available working-state witnesses; no audio/hardware/persistence acceptance', decks={}, evidence=client.evidence, errors=[], missing=[])
    if capability & ALL_COMMANDS != ALL_COMMANDS:
        report['missing'].append('all twelve loop-command capabilities required')
        return report
    for deck in decks:
        completed = []
        try:
            if setup_deck is not None:
                setup_deck(deck)
            client.submit(deck, 4)
            client.submit(deck, 1)
            sleep(1)
            client.submit(deck, 2)
            for op, arguments in ((7, {}), (8, {}), (9, {'beats': 8}), (11, {'position': 0.05}), (12, {'end': 0.05}), (10, {'position': 0.05}), (4, {}), (5, {}), (4, {}), (3, {}), (4, {}), (6, {})):
                client.submit(deck, op, **arguments)
            completed = sorted({e['operation'] for e in client.evidence if e['deck'] == deck})
            if completed != list(range(1, 13)):
                raise MissingEvidence('complete twelve-command matrix unavailable')
            report['decks'][str(deck)] = dict(status='PASS', operations=completed)
        except MissingEvidence as error:
            completed = sorted({e['operation'] for e in client.evidence if e['deck'] == deck})
            report['missing'].append(f'deck {deck + 1}: {error}')
            report['decks'][str(deck)] = dict(status='INCOMPLETE', operations=completed)
        except (ValueError, RuntimeError) as error:
            completed = sorted({e['operation'] for e in client.evidence if e['deck'] == deck})
            report['errors'].append(f'deck {deck + 1}: {error}')
            report['decks'][str(deck)] = dict(status='FAIL', operations=completed)
    report['status'] = 'FAIL' if report['errors'] else 'INCOMPLETE' if report['missing'] else 'PASS'
    return report


def run_refusals(read_log, send_request, health, pid, *, decks=(0, 1, 2, 3), sleep=time.sleep, clock=time.monotonic, sample_rate=None):
    """Real public admission checks with a deliberately impossible load generation.

    This tests rejection, never native action/application. Keep its verdict
    separate from loaded command acceptance, including when Browse is unavailable.
    """
    if not decks or len(set(decks)) != len(decks) or any(d not in range(4) for d in decks):
        raise ValueError('explicit unique logical decks required')
    client = LoopClient(read_log, send_request, health, pid, sleep=sleep, clock=clock, sample_rate=sample_rate)
    report = dict(status='INCOMPLETE', scope='public SDK stale-generation rejection only; no native loop application', evidence=[], errors=[], missing=[])
    if ready(read_log(), pid)['commands'] & ALL_COMMANDS != ALL_COMMANDS:
        report['missing'].append('all twelve loop command capabilities required')
        return report
    for deck in decks:
        try:
            for op in range(1, 13):
                before = client.snapshot(deck)
                client.next_id = max(client.next_id, max((r['id'] for r in client.last_rows), default=0)) + 1
                if client.next_id >= 2**64 or before['native_load'] == 2**64 - 1:
                    raise MissingEvidence('bounded stale test identity unavailable')
                identity = client.next_id
                arguments = {'beats': 8} if op == 9 else {'position': .05} if op in (10, 11) else {'end': .05} if op == 12 else {}
                send_request(request(pid, 'submit', identity, deck, op, 2**64 - 1, before['query_ns'] + 10_000_000_000, **arguments))
                row = client.wait(lambda r: r['id'] == identity and r['event'] in ('submit', 'refused') and r['outcome'] != 0)
                if row['event'] == 'refused':
                    raise MissingEvidence('probe refused the request before calling the SDK')
                if not (compare_timestamps(before['query_ns'], row['receipt_ns']) < 0 and compare_timestamps(row['receipt_ns'], before['query_ns'] + 10_000_000_000) < 0):
                    raise MissingEvidence('stale-generation test deadline cannot be qualified')
                if (row['event'], row['deck'], row['op'], row['target_load'], row['rc'], row['outcome'], row['reason'], row['revision']) != ('submit', deck, op, 2**64 - 1, -4, 2, -4, 0):
                    raise ValueError('public SDK did not reject the stale native load without dispatch')
                report['evidence'].append(row)
        except MissingEvidence as error:
            report['missing'].append(f'deck {deck + 1}: {error}')
        except (ValueError, RuntimeError) as error:
            report['errors'].append(f'deck {deck + 1}: {error}')
    report['status'] = 'FAIL' if report['errors'] else 'INCOMPLETE' if report['missing'] else 'PASS'
    return report
