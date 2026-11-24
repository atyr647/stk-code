#!/usr/bin/env python3
"""Profile report from an STK_N64_PROFILE run.

    n64/tools/n64_profile.py ELF LOG [--top N] [--chains FUNCTION]

The ROM logs one "[prof] pc ra addr..." line per 2 ms of N64 time (see
n64/compat/src/n64_diag.cpp): the interrupted PC, its RA, and the return
addresses found on the interrupted stack. Return addresses are only
candidates (stale stack slots look the same), so "inclusive" figures are an
upper bound; "self" figures are exact samples.
"""
import argparse
import bisect
import collections
import re
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument('elf')
ap.add_argument('log')
ap.add_argument('--top', type=int, default=40)
ap.add_argument('--chains', help='show the commonest call chains through '
                'functions whose name contains this text')
ap.add_argument('--nm', default='/opt/pak-n64/bin/mips64-elf-nm')
args = ap.parse_args()

syms = []
out = subprocess.run([args.nm, '-n', '-C', args.elf], capture_output=True,
                     text=True, check=True).stdout
for line in out.splitlines():
    p = line.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]


def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 else '?'


def short(n):
    return n.split('(')[0]


samples = []   # main thread running
others = []    # another thread (or idle) running
waits = []     # ... and where the main thread was parked meanwhile
pat = re.compile(r'\[(prof|othr|wait)\] ([0-9a-f]{8}) ([0-9a-f]{8})'
                 r'((?: [0-9a-f]{8})*)')
with open(args.log, 'rb') as f:
    for raw in f:
        m = pat.search(raw.decode('latin-1'))
        if m:
            s = [int(m.group(2), 16), int(m.group(3), 16)] + \
                [int(x, 16) for x in m.group(4).split()]
            {'prof': samples, 'othr': others, 'wait': waits}[m.group(1)] \
                .append(s)
n = len(samples) + len(others)
if not n:
    raise SystemExit('no [prof] samples in ' + args.log)



def report(title, samples, total):
    flat = collections.Counter()
    incl = collections.Counter()
    chains = collections.Counter()
    for s in samples:
        chain = [name(s[0])]
        for a in s[1:]:
            g = name(a)
            if g != chain[-1]:
                chain.append(g)
        flat[chain[0]] += 1
        for g in set(chain):
            incl[g] += 1
        if args.chains and any(args.chains in g for g in chain):
            chains[' <- '.join(short(g) for g in chain[:8])] += 1
    print('\n=== %s: %d samples, %.1f%% of all' %
          (title, len(samples), 100.0 * len(samples) / total))
    print('--- self')
    for f, c in flat.most_common(args.top):
        print('%5.1f%%  %s' % (100.0 * c / total, f[:140]))
    print('--- inclusive (upper bound)')
    for f, c in incl.most_common(args.top):
        print('%5.1f%%  %s' % (100.0 * c / total, f[:140]))
    if args.chains:
        print('--- chains through %s' % args.chains)
        for k, c in chains.most_common(20):
            print('%5.1f%%  %s' % (100.0 * c / total, k[:220]))


print('%d samples (%.1f s of N64 time)' % (n, n * 0.002))
report('main thread running', samples, n)
if others:
    report('other threads running (idle included)', others, n)
if waits:
    report('main thread parked at, while others ran', waits, n)
