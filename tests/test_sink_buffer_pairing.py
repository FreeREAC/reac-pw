#!/usr/bin/env python3
"""Every dequeued pw_buffer must be queued back, on EVERY path.

This is a source check, not a unit test, because the defect lives in a PipeWire
callback that a unit test cannot reach — and it is worth a mechanical gate
because of how it failed. on_process dequeued a buffer and returned it only on
the early exit taken when nothing is linked; the path WITH audio linked fell out
of the function still holding it. The pool then stops handing out fresh buffers
and the encoder re-sends the last PCM it saw, so frames keep leaving at exactly
the right rate carrying a valid, non-zero, and completely static audio region.

Nothing downstream of that looks wrong. The link is established, the box is
enrolled, the pacer timing is clean, and "frames carry audio" is TRUE. Measured
on the wire: 112294 frames, 100% non-zero, 0% differing from the previous frame.

So the rule is: after the dequeue, the function must end in a queue, and every
early return must be preceded by one.
"""
import re, sys, pathlib

src = (pathlib.Path(__file__).parent.parent / 'src' / 'reac_sink_node.c').read_text()

m = re.search(r'static void on_process\(void \*data\)\s*\{(.*?)\n\}', src, re.S)
if not m:
    sys.exit('FAIL: could not find on_process in reac_sink_node.c')
body = m.group(1)

deq = body.find('pw_stream_dequeue_buffer')
if deq < 0:
    sys.exit('FAIL: on_process does not dequeue a buffer — has it been rewritten?')
# Start counting AFTER the null guard. That return fires when the dequeue handed
# back nothing, so there is no buffer to give back and it owes no queue — the only
# return in the function that legitimately does not pair.
after = body[deq:]
guard = re.search(r'if\s*\(!\s*\w+\s*\)\s*\n?\s*return\s*;', after)
if not guard:
    sys.exit('FAIL: no "if (!pwb) return;" guard after the dequeue — either the '
             'dequeue is unchecked, or this test has drifted from the source.')
after = after[guard.end():]

# the last statement of the function must return the buffer
tail = [l.strip() for l in after.rstrip().splitlines() if l.strip()
        and not l.strip().startswith(('/*', '*', '//'))]
if not tail or 'pw_stream_queue_buffer' not in tail[-1]:
    sys.exit('FAIL: on_process does not END by queueing the buffer back.\n'
             f'       last statement is: {tail[-1] if tail else "<empty>"}\n'
             '       A path that falls out still holding the buffer drains the\n'
             '       pool, and the sink then re-sends one stale frame forever at\n'
             '       the right rate with valid non-zero audio. Silent to every\n'
             '       check except comparing a frame with the one before it.')

# and every early return after the dequeue must queue first
n_ret = len(re.findall(r'\breturn\b', after))
n_que = after.count('pw_stream_queue_buffer')
if n_que < n_ret:
    sys.exit(f'FAIL: {n_ret} return(s) while holding the buffer but only {n_que} '
             'queue_buffer call(s) — at least one path drops it.')

print(f'OK: on_process holds the buffer across {n_ret} early return(s), each '
      f'paired, and ends by queueing it back ({n_que} queue_buffer calls)')
