"""Find the bfin GCC 4.3 hardware-loop CC miscompile in generated assembly (gcc -S output).

The bug (found by CHORD v2's factory restore, notes/chord2.md): GCC's RTL loop-invariant motion (or GCSE) hoists
a loop-invariant compare out of a counted loop, and the register allocator gives the BImode result the CC register
for the whole loop. The loop's counter test is
the `loop_end` insn (doloop), which does not mention CC. When bfin's machine reorg then rejects the loop as a
hardware loop (here: two latches after block reordering), it rewrites `loop_end` as

    Px += -1;
    cc =Px==0;
    if !cc jump .Lhead;

which overwrites CC. The next pass through the loop (or the code after it) branches on the counter test instead of
the hoisted condition. Signature: a CC read reached from that rewritten counter test before any CC write.

find(asm_text) -> [(function, line number, text)] for every such read.
"""
import re

_LOOPTEST = re.compile(r"^cc\s*=\s*([RP][0-7])\s*==\s*0\s*;", re.I)
_DEC = r"^{}\s*\+=\s*-1\s*;"
_CONDJ = re.compile(r"^if\s+!?cc\s+jump(?:\.[sl])?\s+(\S+?)(?:\s*\(bp\))?\s*;", re.I)
_JUMP = re.compile(r"^jump(?:\.[sl])?\s+(\.?\w+)\s*;", re.I)
_READ = re.compile(r"\bif\s+!?cc\b|=\s*!?\s*cc\b|=\s*astat\b", re.I)
_WRITE = re.compile(r"^(cc|astat)\s*=", re.I)


def _insns(line):
    line = line.split("//")[0].strip()
    return [p.strip() for p in line.split("||") if p.strip()]


def _functions(text):
    fn, body = None, []
    for n, raw in enumerate(text.splitlines(), 1):
        s = raw.strip()
        m = re.match(r"^(_\w+):$", s)
        if m:
            if fn:
                yield fn, body
            fn, body = m.group(1), []
            continue
        if fn and s.startswith(".size"):
            yield fn, body
            fn, body = None, []
            continue
        if fn:
            body.append((n, s))
    if fn:
        yield fn, body


def _walk(body, labels, i):
    """From body index i: the first CC read before a CC write, a call or a return, else None."""
    seen = set()
    while i < len(body) and i not in seen:
        seen.add(i)
        n, s = body[i]
        if s.endswith(":") or s.startswith("."):
            i += 1
            continue
        ins = _insns(s)
        if any(_READ.search(x) for x in ins):
            return n, s
        if any(_WRITE.match(x) for x in ins):
            return None
        low = s.lower()
        if low.startswith(("rts", "rti", "rtx", "rtn", "call", "raise", "excpt")):
            return None
        m = _JUMP.match(s)
        if m:
            if m.group(1) not in labels:
                return None
            i = labels[m.group(1)]
            continue
        i += 1
    return None


def find(text):
    hits = []
    for fn, body in _functions(text):
        labels = {s[:-1]: k for k, (_, s) in enumerate(body) if s.endswith(":")}
        code = [k for k, (_, s) in enumerate(body) if s and not s.endswith(":") and not s.startswith(".")]
        for j, k in enumerate(code):
            m = _CONDJ.match(body[k][1])
            if not m or j < 2:
                continue
            t = _LOOPTEST.match(body[code[j - 1]][1])
            if not t:
                continue
            dec = re.compile(_DEC.format(t.group(1)), re.I)
            if not any(dec.match(body[code[q]][1]) for q in range(max(0, j - 5), j - 1)):
                continue
            for start in ([labels[m.group(1)]] if m.group(1) in labels else []) + [k + 1]:
                r = _walk(body, labels, start)
                if r and (fn, *r) not in hits:
                    hits.append((fn, *r))
    return hits
