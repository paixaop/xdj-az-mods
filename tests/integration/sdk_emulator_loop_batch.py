#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate explicitly requested, locally clocked public SDK diagnostic plans.

The approved client performs only the documented fixed plan, through the public
SDK. Host code independently validates its per-command receipts and observations;
a PLAN PASS marker is never sufficient by itself.
"""
from sdk_timestamps import compare_timestamps
import re
import time
from sdk_emulator_loop import LoopClient, records, ready, applied, ALL_COMMANDS
from sdk_emulator_controls import MissingEvidence

PLAN_FIELDS = set('v event kind id deck load status completed query_ns deadline_ns'.split())
OPERATIONS = (4, 1, 2, 7, 8, 9, 11, 12, 10, 4, 5, 4, 3, 4, 6)
COUNTS = {'refusals': 12, 'matrix': len(OPERATIONS)}


def plans(text):
    result = []
    for line in text.splitlines():
        if not line.startswith('SDK_LOOP_PLAN '):
            continue
        pairs = [p.split('=', 1) for p in line.split()[1:]]
        if len(line) > 512 or len(pairs) != len(PLAN_FIELDS) or any(len(p) != 2 for p in pairs) or {p[0] for p in pairs} != PLAN_FIELDS:
            raise ValueError('exact diagnostic plan fields required')
        raw = dict(pairs)
        if raw['event'] not in ('start', 'done') or raw['kind'] not in COUNTS or raw['status'] not in ('PASS', 'FAIL', 'INCOMPLETE'):
            raise ValueError('unknown diagnostic plan state')
        row = {k: raw[k] for k in ('event', 'kind', 'status')}
        for key in PLAN_FIELDS - row.keys():
            value = raw[key]
            if not re.fullmatch(r"[0-9]+", value) or (value.startswith('0') and value != '0'):
                raise ValueError('canonical diagnostic plan integer required')
            row[key] = int(value)
            if not 0 <= row[key] < 2**64:
                raise ValueError('diagnostic plan integer overflow')
        if row['v'] != 1 or row['deck'] not in range(4) or not row['id'] or not row['load'] or not row['query_ns']:
            raise ValueError('diagnostic plan identity mismatch')
        if row['completed'] > COUNTS[row['kind']]:
            raise ValueError('diagnostic plan completion overflow')
        if row['event'] == 'start' and (row['completed'] or row['status'] != 'INCOMPLETE' or not (compare_timestamps(0, row['deadline_ns'] - row['query_ns']) < 0 and compare_timestamps(row['deadline_ns'] - row['query_ns'], 180_000_000_000) <= 0)):
            raise ValueError('invalid diagnostic plan start/bound')
        if row['status'] == 'PASS' and (row['event'] != 'done' or row['completed'] != COUNTS[row['kind']] or compare_timestamps(row['query_ns'], row['deadline_ns']) > 0):
            raise ValueError('unqualified diagnostic plan PASS')
        result.append(row)
    return result


def next_id(text, *, sample_rate=None):
    allocated = [r['id'] for r in records(text, sample_rate=sample_rate)]
    allocated += [r['id'] + COUNTS[r['kind']] - 1 for r in plans(text)]
    value = max(allocated, default=0) + 1
    if value > 2**64 - 16:
        raise ValueError('diagnostic plan ID space exhausted')
    return value


def batch_request(pid, kind, identity, deck, load, deadline):
    if kind not in COUNTS or not re.fullmatch(r"[0-9]+", str(pid)) or int(pid) <= 0:
        raise ValueError('explicit process-bound diagnostic plan required')
    if any(type(v) is not int or not 0 < v < 2**64 for v in (identity, load, deadline)) or type(deck) is not int or deck not in range(4):
        raise ValueError('bounded diagnostic plan scalars required')
    if identity > 2**64 - 16 or (kind == 'refusals') != (load == 2**64 - 1):
        raise ValueError('diagnostic plan target/range mismatch')
    return (f'v=1 pid={pid} action={kind} id={identity} deck={deck} op=0 load={load} '
            f'deadline_ns={deadline} position=0 end=0 beats=0\n')


def validate(text, kind, identity, deck, load, *, sample_rate=None):
    metadata = [r for r in plans(text) if r['id'] == identity]
    if len(metadata) != 2 or [r['event'] for r in metadata] != ['start', 'done']:
        raise MissingEvidence('one matching diagnostic plan start and completion required')
    start, done = metadata
    if any((r['kind'], r['deck'], r['load']) != (kind, deck, load) for r in metadata) or compare_timestamps(start['deadline_ns'], done['deadline_ns']) != 0 or compare_timestamps(done['query_ns'], start['query_ns']) < 0:
        raise ValueError('diagnostic plan identity/clock changed')
    if done['status'] == 'INCOMPLETE':
        raise MissingEvidence('client plan stopped without complete native/observer evidence')
    if done['status'] != 'PASS':
        raise ValueError('public SDK diagnostic plan failed')
    all_rows = records(text, sample_rate=sample_rate)
    expected_ops = tuple(range(1, 13)) if kind == 'refusals' else OPERATIONS
    evidence = []
    for step, operation in enumerate(expected_ops):
        request_id = identity + step
        submitted = [r for r in all_rows if r['id'] == request_id and r['event'] == 'submit']
        if len(submitted) != 1:
            raise MissingEvidence('one actual SDK submit per diagnostic step required')
        submission = submitted[0]
        if (submission['deck'], submission['op'], submission['target_load']) != (deck, operation, load):
            raise ValueError('diagnostic step targets a different deck/operation/load')
        before_rows = [r for r in all_rows if r['seq'] < submission['seq'] and r['event'] == 'snapshot' and r['deck'] == deck]
        if not before_rows:
            raise MissingEvidence('independent pre-command public snapshot required')
        before = before_rows[-1]
        if not (compare_timestamps(start['query_ns'], before['query_ns']) <= 0 and compare_timestamps(before['query_ns'], submission['query_ns']) <= 0 and compare_timestamps(submission['query_ns'], done['query_ns']) <= 0):
            raise ValueError('diagnostic step falls outside its plan clock fence')
        if kind == 'refusals':
            if (submission['rc'], submission['outcome'], submission['reason'], submission['revision']) != (-4, 2, -4, 0):
                raise ValueError('stale native load was not rejected by the SDK')
            if not (compare_timestamps(before['query_ns'], submission['receipt_ns']) <= 0 and compare_timestamps(submission['receipt_ns'], before['query_ns'] + 10_000_000_000) < 0):
                raise MissingEvidence('refusal deadline cannot be distinguished from stale load')
            evidence.append(dict(request_id=request_id, operation=operation, before=before, terminal=submission))
            continue
        terminal_rows = [r for r in all_rows if r['id'] == request_id and r['event'] in ('submit', 'poll', 'terminal') and r['outcome'] != 0]
        if not terminal_rows:
            raise MissingEvidence('native command terminal receipt unavailable')
        terminal = terminal_rows[-1]
        if (terminal['deck'], terminal['op'], terminal['target_load']) != (deck, operation, load):
            raise ValueError('cross-request native command terminal receipt')
        releases = [r for r in all_rows if r['id'] == request_id and r['event'] in ('release', 'released') and r['rc'] == 0]
        if len(releases) != 1 or releases[0]['seq'] < terminal['seq']:
            raise MissingEvidence('settled explicit native result release unavailable')
        next_submissions = [r['seq'] for r in all_rows if r['event'] == 'submit' and r['id'] == request_id + 1]
        limit = min(next_submissions) if next_submissions else 2**64
        observations = [r for r in all_rows if terminal['seq'] < r['seq'] < limit and r['event'] == 'snapshot' and r['deck'] == deck]
        if not observations:
            raise MissingEvidence('post-command observer snapshot unavailable')
        observed = observations[-1]
        if step == 0 and not before['engaged_valid']:
            # Idempotent bootstrap EXIT has no guaranteed notification. Later
            # active->inactive EXIT steps still require independent observers.
            if terminal['rc'] != 0 or terminal['outcome'] != 1 or terminal['target_load'] != load or not terminal['load_valid'] or terminal['native_load'] != load:
                raise ValueError('bootstrap EXIT did not confirm its native load')
        else:
            applied(before, terminal, operation, load, observed, sample_rate=sample_rate)
            if operation not in (1, 4, 6) and not (compare_timestamps(observed['loop_ns'], before['loop_ns']) > 0 and observed['loop_rev'] > before['loop_rev']):
                raise MissingEvidence('newer independent native loop observation required')
        evidence.append(dict(request_id=request_id, operation=operation, before=before, terminal=terminal, observed=observed, release=releases[0], bootstrap=step == 0))
    return dict(status='PASS', kind=kind, deck=deck, operations=sorted(set(expected_ops)), evidence=evidence, plan=metadata)


def run(kind, read_log, send_request, health, pid, *, decks=(0, 1, 2, 3), setup_deck=None, sleep=time.sleep, clock=time.monotonic, sample_rate=None):
    if kind not in COUNTS or not decks or len(set(decks)) != len(decks) or any(d not in range(4) for d in decks):
        raise ValueError('explicit diagnostic kind and unique logical decks required')
    if kind != 'refusals' and (type(sample_rate) is not int or sample_rate <= 0):
        raise MissingEvidence('bound source sample rate required before loop commands')
    client = LoopClient(read_log, send_request, health, pid, sleep=sleep, clock=clock, sample_rate=sample_rate)
    report = dict(status='INCOMPLETE', scope='public SDK stale-load admission' if kind == 'refusals' else 'public SDK loop application and independently available working-loop state; no audio/persistence acceptance', decks={}, evidence=[], errors=[], missing=[])
    if ready(read_log(), pid)['commands'] & ALL_COMMANDS != ALL_COMMANDS:
        report['missing'].append('all twelve loop command capabilities required')
        return report
    for deck in decks:
        try:
            if kind == 'matrix' and setup_deck is not None:
                setup_deck(deck)
            before = client.snapshot(deck)
            if kind == 'matrix' and not before['load_valid']:
                raise MissingEvidence('current native load required before application plan')
            text = client.last_text
            identity = next_id(text, sample_rate=sample_rate)
            load = 2**64 - 1 if kind == 'refusals' else before['native_load']
            send_request(batch_request(pid, kind, identity, deck, load, before['query_ns'] + 10_000_000_000))
            deadline = clock() + 185
            while True:
                client.read()  # fresh owner, process and service guard every capture
                text = client.last_text
                if any(r['event'] == 'done' and r['id'] == identity for r in plans(text)):
                    break
                if any(r['event'] == 'refused' and r['id'] == identity for r in records(text, sample_rate=sample_rate)):
                    raise MissingEvidence('batch request refused before SDK plan execution')
                if compare_timestamps(clock(), deadline, units_per_second=1) >= 0:
                    raise MissingEvidence('bounded diagnostic plan wait expired')
                sleep(.2)
            result = validate(text, kind, identity, deck, load, sample_rate=sample_rate)
            report['decks'][str(deck)] = result
            report['evidence'].extend(result['evidence'])
        except MissingEvidence as error:
            report['missing'].append(f'deck {deck + 1}: {error}')
            report['decks'][str(deck)] = dict(status='INCOMPLETE')
            break  # a single client may still be draining a pending SDK result
        except (ValueError, RuntimeError) as error:
            report['errors'].append(f'deck {deck + 1}: {error}')
            report['decks'][str(deck)] = dict(status='FAIL')
            break
    for deck in decks:
        if str(deck) not in report['decks']:
            report['decks'][str(deck)] = dict(status='INCOMPLETE')
    report['status'] = 'FAIL' if report['errors'] else 'INCOMPLETE' if report['missing'] else 'PASS'
    return report


def run_batch_refusals(*args, **kwargs):
    return run('refusals', *args, **kwargs)


def run_batch_commands(*args, **kwargs):
    return run('matrix', *args, **kwargs)
