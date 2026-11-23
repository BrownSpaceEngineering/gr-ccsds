#!/usr/bin/env python3
"""Find a bit pattern in a bit stream, tolerating flipped bits AND dropped bits.

Plain correlation (as in find_sync.py) only copes with flipped bits: once a bit is
dropped, everything after it is shifted by one and the match is lost. This script
uses approximate matching (edit distance), so it counts three kinds of error:

  flipped   a stream bit has the wrong value                    (marked x)
  dropped   a pattern bit is missing from the stream            (marked d)
  extra     the stream has an additional bit the pattern lacks  (marked e)

Each error costs 1. A match is reported when the total is within --errors.
Both bit polarities are searched. Every match is shown as an alignment, followed by
the bytes that come after it in decimal.

The default pattern is the sync marker [6, 190, 69, 6].

Input formats (picked automatically unless you say otherwise):
  packed bytes   a File Sink written after Pack K Bits (bits are read MSB first)
  --unpacked     one bit per byte (a File Sink straight off GFSK Demod)
  --text         a text file of 0s and 1s, such as GFSKtest_bits.txt from bin2bits.py
                 (plain 0/1 text files are detected automatically; use --text
                 if the file was made with --index)

Examples:
  python3 correlate_pattern.py GFSKtest.bin
  python3 correlate_pattern.py GFSKtest.bin --errors 4
  python3 correlate_pattern.py GFSKtest.bin --pattern 00000110101111100100010100000110
  python3 correlate_pattern.py GFSKtest.bin --marker "6 190 69 6" --max-drops 1
"""
import argparse
import re
import sys


# ---------------------------------------------------------------- input ---------
def load_bits(path, unpacked, text):
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        sys.exit("error: input file is empty")
    if not (unpacked or text):
        # a file made only of '0', '1' and whitespace is a bit-text file
        text = not data.translate(None, b"01 \t\r\n")
    if text:
        bits = []
        for line in data.decode("ascii", errors="ignore").splitlines():
            if ":" in line:  # drop an --index prefix such as "00000008: "
                line = line.split(":", 1)[1]
            bits.extend(1 if c == "1" else 0 for c in line if c in "01")
        return bits, "text"
    if unpacked:
        return [b & 1 for b in data], "one bit per byte"
    return [(b >> k) & 1 for b in data for k in range(7, -1, -1)], "packed bytes"


def parse_marker(text):
    try:
        values = [int(tok, 0) for tok in re.split(r"[\s,\[\]]+", text.strip()) if tok]
    except ValueError:
        sys.exit(f"error: cannot read marker {text!r}; use numbers like \"6 190 69 6\"")
    if not values or any(not 0 <= v <= 255 for v in values):
        sys.exit("error: marker must be one or more byte values from 0 to 255")
    return [(v >> k) & 1 for v in values for k in range(7, -1, -1)]


def bitstr(bits):
    return "".join(map(str, bits))


def bits_to_bytes(bits):
    out = []
    for i in range(0, len(bits) - len(bits) % 8, 8):
        v = 0
        for b in bits[i:i + 8]:
            v = (v << 1) | b
        out.append(v)
    return out


# ------------------------------------------------------------- matching ---------
def edit_hits(text, pattern, max_err):
    """Return (end_index, errors) wherever some stretch of `text` ending there is
    within max_err edits of `pattern`. Myers' bit-vector algorithm: fast, even for
    long streams, because one machine word update handles the whole pattern."""
    m = len(pattern)
    mask = (1 << m) - 1
    high = 1 << (m - 1)
    peq = [0, 0]
    for i, b in enumerate(pattern):
        peq[b] |= 1 << i
    pv, mv, score = mask, 0, m
    hits = []
    for j, c in enumerate(text):
        eq = peq[c]
        xv = eq | mv
        xh = ((((eq & pv) + pv) & mask) ^ pv) | eq
        ph = mv | (~(xh | pv) & mask)
        mh = pv & xh
        if ph & high:
            score += 1
        elif mh & high:
            score -= 1
        ph = (ph << 1) & mask
        mh = (mh << 1) & mask
        pv = mh | (~(xv | ph) & mask)
        mv = ph & xv
        if score <= max_err:
            hits.append((j, score))
    return hits


def align(pattern, window):
    """Align the whole pattern to the end of `window`; the start is free.
    Returns (offset_in_window, operations, errors). Operations, in pattern order:
    M match, S flipped bit, D dropped bit, E extra stream bit."""
    m, w = len(pattern), len(window)
    d = [[0] * (w + 1) for _ in range(m + 1)]
    for i in range(1, m + 1):
        d[i][0] = i
    for i in range(1, m + 1):
        for c in range(1, w + 1):
            cost = 0 if pattern[i - 1] == window[c - 1] else 1
            d[i][c] = min(d[i - 1][c - 1] + cost, d[i - 1][c] + 1, d[i][c - 1] + 1)
    i, c, ops = m, w, []
    while i > 0:
        cost = 0 if c > 0 and pattern[i - 1] == window[c - 1] else 1
        if c > 0 and d[i][c] == d[i - 1][c - 1] + cost:
            ops.append("M" if cost == 0 else "S")
            i -= 1
            c -= 1
        elif d[i][c] == d[i - 1][c] + 1:
            ops.append("D")
            i -= 1
        else:
            ops.append("E")
            c -= 1
    ops.reverse()
    return c, ops, d[m][w]


def alignment_rows(pattern, window, ops):
    prow, trow, mrow = [], [], []
    pi = ti = 0
    for op in ops:
        if op in "MS":
            prow.append(str(pattern[pi]))
            trow.append(str(window[ti]))
            mrow.append("|" if op == "M" else "x")
            pi += 1
            ti += 1
        elif op == "D":
            prow.append(str(pattern[pi]))
            trow.append("-")
            mrow.append("d")
            pi += 1
        else:
            prow.append("-")
            trow.append(str(window[ti]))
            mrow.append("e")
            ti += 1
    return "".join(prow), "".join(trow), "".join(mrow)


def best_per_cluster(hits, gap):
    best = []
    for h in hits:
        if best and h[0] - best[-1][2] <= gap:
            if h[1] < best[-1][1]:
                best[-1] = (h[0], h[1], h[0])
            else:
                best[-1] = (best[-1][0], best[-1][1], h[0])
        else:
            best.append((h[0], h[1], h[0]))
    return [(j, s) for j, s, _ in best]


def main():
    p = argparse.ArgumentParser(
        description="Find a bit pattern despite flipped, dropped and extra bits.")
    p.add_argument("infile")
    src = p.add_mutually_exclusive_group()
    src.add_argument("-m", "--marker", default="6 190 69 6",
                     help='pattern as decimal bytes (default: "6 190 69 6")')
    src.add_argument("-p", "--pattern", help="pattern as a string of 0s and 1s")
    p.add_argument("-e", "--errors", type=int, default=3,
                   help="total errors allowed: flipped + dropped + extra (default: 3)")
    p.add_argument("--max-flips", type=int, help="most flipped bits allowed in a match")
    p.add_argument("--max-drops", type=int, help="most dropped bits allowed in a match")
    p.add_argument("--max-extra", type=int, help="most extra bits allowed in a match")
    p.add_argument("-b", "--bytes", type=int, default=16,
                   help="bytes to show after each match (default: 16)")
    p.add_argument("--no-invert", action="store_true",
                   help="do not also search for the inverted pattern")
    p.add_argument("--unpacked", action="store_true", help="input has one bit per byte")
    p.add_argument("--text", action="store_true", help="input is a text file of 0s and 1s")
    args = p.parse_args()

    if args.pattern is not None:
        if not args.pattern or set(args.pattern) - {"0", "1"}:
            sys.exit("error: --pattern must be a string of 0s and 1s")
        pattern = [int(c) for c in args.pattern]
    else:
        pattern = parse_marker(args.marker)
    m = len(pattern)
    if m < 8:
        sys.exit("error: the pattern must be at least 8 bits long")
    if args.errors < 0 or args.errors >= m // 4:
        p.error(f"--errors must be at least 0 and less than {m // 4} "
                f"for a {m}-bit pattern (more would match almost anything)")

    try:
        bits, kind = load_bits(args.infile, args.unpacked, args.text)
    except FileNotFoundError:
        sys.exit(f"error: file not found: {args.infile}")

    print(f"pattern ({m} bits): {bitstr(pattern)}")
    print(f"read {len(bits)} bits ({kind}); up to {args.errors} error(s) "
          f"(flipped + dropped + extra)\n")

    searches = [("normal", pattern)]
    if not args.no_invert:
        searches.append(("inverted", [b ^ 1 for b in pattern]))

    results = []
    for label, pat in searches:
        hits = edit_hits(bits, pat, args.errors)
        for j, score in best_per_cluster(hits, m):
            base = max(0, j - (m + args.errors) + 1)
            window = bits[base:j + 1]
            off, ops, errs = align(pat, window)
            flips, drops, extra = ops.count("S"), ops.count("D"), ops.count("E")
            if args.max_flips is not None and flips > args.max_flips:
                continue
            if args.max_drops is not None and drops > args.max_drops:
                continue
            if args.max_extra is not None and extra > args.max_extra:
                continue
            start = base + off
            results.append((start, j, label, pat, window[off:], ops, errs,
                            flips, drops, extra))
    results.sort(key=lambda r: r[0])

    if not results:
        print("pattern not found. Try a larger --errors, and check the preamble and "
              "signal first (find_preamble.py).")
        return

    print(f"{len(results)} match(es)\n")
    for k, (start, end, label, pat, seg, ops, errs, flips, drops, extra) in enumerate(results, 1):
        prow, trow, mrow = alignment_rows(pat, seg, ops)
        print(f"match {k}: stream bits {start}-{end} ({end - start + 1} bits), "
              f"byte {start // 8} bit {start % 8}, {label} polarity")
        print(f"  {errs} error(s): {flips} flipped, {drops} dropped, {extra} extra "
              f"({ops.count('M')} of {m} pattern bits matched exactly)")
        print(f"  pattern: {prow}")
        print(f"           {mrow}")
        print(f"  stream : {trow}")
        tail = bits[end + 1:end + 1 + args.bytes * 8]
        if label == "inverted":
            tail = [b ^ 1 for b in tail]
        after = bits_to_bytes(tail)
        shown = " ".join(str(v) for v in after) if after else "(end of data)"
        print(f"  next {len(after)} byte(s), decimal: {shown}\n")
    print("key: | match   x flipped bit   d dropped bit (in pattern, missing from "
          "stream)   e extra bit (in stream only)")


if __name__ == "__main__":
    main()
