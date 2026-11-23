#!/usr/bin/env python3
"""Compare two bit files and measure how well they correlate.

Typical use: file 1 is what you sent (the reference), file 2 is what you received.
Nothing is searched for inside a larger file; the two whole files are compared.

Three views of the same question, "how alike are these two bit streams?":

  1) Bit for bit      both files lined up from their first bit. Reports the matching
                      bits, the bit error rate and the correlation coefficient
                      (+1 identical, 0 unrelated, -1 exactly inverted).
  2) Best shift       slides file 2 against file 1 (+/- --max-lag bits) and reports the
                      shift with the strongest correlation, so a few extra or missing
                      bits at the start do not hide a match. The z-score says how far
                      the result is from what two random streams would give
                      (about 6 or more means "not chance").
  3) Edit distance    allows flipped, dropped and extra bits anywhere (see
                      correlate_pattern.py), and lists where they happened. It is
                      compared with a random baseline so you can judge the number.

Input formats (picked automatically for each file unless you say otherwise):
  text file of 0s and 1s   such as GFSKtest_bits.txt from bin2bits.py
  packed bytes             a File Sink written after Pack K Bits (MSB first)
  --unpacked               one bit per byte (applies to both files)
  --text                   force text parsing (applies to both files; use it for a
                           bin2bits.py file made with --index)

Examples:
  python3 compare_bits.py sent_bits.txt received_bits.txt
  python3 compare_bits.py sent.bin received.bin --max-lag 500
  python3 compare_bits.py sent_bits.txt received_bits.txt --show 256
"""
import argparse
import math
import random
import sys


# ---------------------------------------------------------------- input ---------
def load_bits(path, unpacked, text):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        sys.exit(f"error: file not found: {path}")
    if not data:
        sys.exit(f"error: {path} is empty")
    is_text = text
    if not (unpacked or text):
        # a file made only of '0', '1' and whitespace is a bit-text file
        is_text = not data.translate(None, b"01 \t\r\n")
    if is_text:
        bits = []
        for line in data.decode("ascii", errors="ignore").splitlines():
            if ":" in line:  # drop an --index prefix such as "00000008: "
                line = line.split(":", 1)[1]
            bits.extend(1 if c == "1" else 0 for c in line if c in "01")
        if not bits:
            sys.exit(f"error: no 0s or 1s found in {path}")
        return bits, "text"
    if unpacked:
        return [b & 1 for b in data], "one bit per byte"
    return [(b >> k) & 1 for b in data for k in range(7, -1, -1)], "packed bytes"


def bitstr(bits):
    return "".join(map(str, bits))


def popcount(x):
    return x.bit_count() if hasattr(x, "bit_count") else bin(x).count("1")


# ---------------------------------------------------- 1 + 2: shift / lag ---------
def sub_bits(x, n, i0, i1):
    """Bits i0..i1-1 of an n-bit integer whose bit 0 is the most significant."""
    return (x >> (n - i1)) & ((1 << (i1 - i0)) - 1)


def lag_scores(a, b, max_lag, min_overlap):
    """For each shift s, compare a[i] with b[i - s] over the overlap.
    Returns a list of (lag, overlap, mismatches, rho, z)."""
    na, nb = len(a), len(b)
    ia, ib = int(bitstr(a), 2), int(bitstr(b), 2)
    out = []
    for s in range(-max_lag, max_lag + 1):
        i0, i1 = max(0, s), min(na, nb + s)
        n = i1 - i0
        if n < min_overlap:
            continue
        mism = popcount(sub_bits(ia, na, i0, i1) ^ sub_bits(ib, nb, i0 - s, i1 - s))
        rho = (n - 2 * mism) / n
        out.append((s, n, mism, rho, rho * math.sqrt(n)))
    return out


# --------------------------------------------------------- 3: edit distance -----
def edit_distance(a, b):
    """Global edit distance (flip, drop, extra each cost 1), Myers bit-vector method."""
    m = len(a)
    mask = (1 << m) - 1
    high = 1 << (m - 1)
    peq = [0, 0]
    for i, bit in enumerate(a):
        peq[bit] |= 1 << i
    pv, mv, score = mask, 0, m
    for c in b:
        eq = peq[c]
        xv = eq | mv
        xh = ((((eq & pv) + pv) & mask) ^ pv) | eq
        ph = mv | (~(xh | pv) & mask)
        mh = pv & xh
        if ph & high:
            score += 1
        elif mh & high:
            score -= 1
        ph = ((ph << 1) | 1) & mask
        mh = (mh << 1) & mask
        pv = mh | (~(xv | ph) & mask)
        mv = ph & xv
    return score


def banded_align(a, b, d, max_cells):
    """Optimal alignment restricted to the band |i - j| <= d (enough for cost d).
    Returns a list of operations in order, or None if the band is too big.
    M match, S flipped bit, D dropped bit (in a, missing from b), E extra bit (in b)."""
    m, n = len(a), len(b)
    if m * (2 * d + 1) > max_cells:
        return None
    inf = 10 ** 9
    lows, rows = [], []
    for i in range(m + 1):
        lo, hi = max(0, i - d), min(n, i + d)
        row = [inf] * (hi - lo + 1)
        for j in range(lo, hi + 1):
            if i == 0:
                v = j
            elif j == 0:
                v = i
            else:
                plo, prow = lows[i - 1], rows[i - 1]
                best = inf
                k = j - 1 - plo  # diagonal
                if 0 <= k < len(prow):
                    best = prow[k] + (0 if a[i - 1] == b[j - 1] else 1)
                k = j - plo  # above (drop)
                if 0 <= k < len(prow) and prow[k] + 1 < best:
                    best = prow[k] + 1
                if j - 1 >= lo and row[j - 1 - lo] + 1 < best:  # left (extra)
                    best = row[j - 1 - lo] + 1
                v = best
            row[j - lo] = v
        lows.append(lo)
        rows.append(row)

    def cell(i, j):
        k = j - lows[i]
        return rows[i][k] if 0 <= k < len(rows[i]) else inf

    i, j, ops = m, n, []
    while i > 0 or j > 0:
        cur = cell(i, j)
        if i > 0 and j > 0:
            cost = 0 if a[i - 1] == b[j - 1] else 1
            if cur == cell(i - 1, j - 1) + cost:
                ops.append("M" if cost == 0 else "S")
                i -= 1
                j -= 1
                continue
        if i > 0 and cur == cell(i - 1, j) + 1:
            ops.append("D")
            i -= 1
        else:
            ops.append("E")
            j -= 1
    ops.reverse()
    return ops


def alignment_rows(a, b, ops):
    arow, brow, mrow = [], [], []
    i = j = 0
    for op in ops:
        if op in "MS":
            arow.append(str(a[i]))
            brow.append(str(b[j]))
            mrow.append("|" if op == "M" else "x")
            i += 1
            j += 1
        elif op == "D":
            arow.append(str(a[i]))
            brow.append("-")
            mrow.append("d")
            i += 1
        else:
            arow.append("-")
            brow.append(str(b[j]))
            mrow.append("e")
            j += 1
    return arow, brow, mrow


def describe_errors(ops, limit=12):
    """List where errors happened, as positions in file 1 (0-based bit numbers)."""
    notes, i = [], 0
    for op in ops:
        if op in "MS":
            if op == "S":
                notes.append(f"bit {i} flipped")
            i += 1
        elif op == "D":
            notes.append(f"bit {i} dropped")
            i += 1
        else:
            notes.append(f"extra bit before bit {i}")
    shown = ", ".join(notes[:limit])
    more = f" ... and {len(notes) - limit} more" if len(notes) > limit else ""
    return shown + more


# ------------------------------------------------------------------ main --------
def main():
    p = argparse.ArgumentParser(
        description="Compare two bit files and measure how well they correlate.")
    p.add_argument("file1", help="reference bits (e.g. what you sent)")
    p.add_argument("file2", help="bits to compare (e.g. what you received)")
    p.add_argument("--max-lag", type=int, default=200,
                   help="largest shift to try, in bits, each direction (default: 200)")
    p.add_argument("--show", type=int, default=128,
                   help="alignment columns to display, 0 for none (default: 128)")
    p.add_argument("--max-edit-bits", type=int, default=100000,
                   help="skip the edit-distance step above this many bits (default: 100000)")
    p.add_argument("--max-cells", type=int, default=3000000,
                   help="largest alignment table to build (default: 3000000)")
    p.add_argument("--unpacked", action="store_true", help="inputs have one bit per byte")
    p.add_argument("--text", action="store_true", help="inputs are text files of 0s and 1s")
    args = p.parse_args()
    if args.max_lag < 0 or args.show < 0:
        p.error("--max-lag and --show cannot be negative")

    a, kind_a = load_bits(args.file1, args.unpacked, args.text)
    b, kind_b = load_bits(args.file2, args.unpacked, args.text)
    na, nb = len(a), len(b)
    print(f"file 1 (reference): {args.file1}: {na} bits ({kind_a}), "
          f"{100 * sum(a) / na:.1f}% ones")
    print(f"file 2 (compared) : {args.file2}: {nb} bits ({kind_b}), "
          f"{100 * sum(b) / nb:.1f}% ones")

    # ---- 1) bit for bit
    n = min(na, nb)
    mism0 = sum(x != y for x, y in zip(a[:n], b[:n]))
    rho0 = (n - 2 * mism0) / n
    print("\n1) Bit for bit, no shifting")
    print(f"   compared the first {n} bits: {n - mism0} match, {mism0} differ "
          f"(bit error rate {100 * mism0 / n:.2f}%)")
    print(f"   correlation coefficient: {rho0:+.3f}  "
          f"(two unrelated streams give 0 +/- {1 / math.sqrt(n):.3f})")

    # ---- 2) best shift
    min_overlap = min(32, n)
    scores = lag_scores(a, b, args.max_lag, min_overlap)
    print(f"\n2) Best shift (file 2 slid +/-{args.max_lag} bits against file 1)")
    best = None
    if scores:
        ranked = sorted(scores, key=lambda r: -abs(r[4]))
        best = ranked[0]
        print("   lag   overlap  matching    rho      z-score")
        shown, seen = 0, []
        for r in ranked:
            if any(abs(r[0] - s) < 2 for s in seen):
                continue  # skip neighbours of a peak already listed
            seen.append(r[0])
            tag = "   <- best" if r is best else ""
            print(f"   {r[0]:+5d}  {r[1]:7d}  {100 * (r[1] - r[2]) / r[1]:6.1f}%  "
                  f"{r[3]:+.3f}  {r[4]:+8.1f}{tag}")
            shown += 1
            if shown == 3:
                break
        if best[3] < 0:
            print("   (negative rho: file 2 looks like file 1 with every bit inverted)")
    else:
        print("   files too short for this lag range")

    # ---- 3) edit distance
    print("\n3) Allowing flipped, dropped and extra bits (edit distance)")
    verdict_ratio = None
    d_use = None
    inverted = False
    if max(na, nb) > args.max_edit_bits:
        print(f"   skipped: files longer than {args.max_edit_bits} bits "
              f"(raise --max-edit-bits to force it)")
    else:
        d_norm = edit_distance(a, b)
        b_inv = [x ^ 1 for x in b]
        d_inv = edit_distance(a, b_inv)
        inverted = d_inv < d_norm
        d_use = d_inv if inverted else d_norm
        b_use = b_inv if inverted else b
        shuffled = b_use[:]
        random.Random(12345).shuffle(shuffled)
        d_base = edit_distance(a, shuffled)
        verdict_ratio = d_use / d_base if d_base else 1.0
        denom = max(na, nb)
        print(f"   {'inverted ' if inverted else ''}file 2 is {d_use} edit(s) away from file 1 "
              f"(similarity {100 * (1 - d_use / denom):.1f}%)")
        print(f"   random baseline (file 2's bits shuffled): {d_base} edit(s) "
              f"-> your result is {verdict_ratio:.2f}x the baseline")
        if d_use == 0:
            print("   the files are identical" + (" once inverted" if inverted else ""))
        else:
            ops = banded_align(a, b_use, d_use, args.max_cells)
            if ops is None:
                print("   (alignment not drawn: table too large, raise --max-cells)")
            else:
                print(f"   breakdown: {ops.count('S')} flipped, {ops.count('D')} dropped, "
                      f"{ops.count('E')} extra, {ops.count('M')} matching")
                print(f"   where: {describe_errors(ops)}")
                if args.show:
                    arow, brow, mrow = alignment_rows(a, b_use, ops)
                    cols = min(args.show, len(mrow))
                    print(f"\n   alignment of the first {cols} column(s)"
                          f"   (| match, x flipped, d dropped, e extra):")
                    for s in range(0, cols, 64):
                        e = min(s + 64, cols)
                        print(f"   file 1: {''.join(arow[s:e])}")
                        print(f"           {''.join(mrow[s:e])}")
                        print(f"   file 2: {''.join(brow[s:e])}\n")

    # ---- verdict
    print("Verdict: ", end="")
    inv_note = " (with every bit inverted)" if inverted else ""
    if d_use == 0:
        print("identical" + (" once inverted." if inverted else "."))
    elif verdict_ratio is not None and verdict_ratio < 0.25:
        print(f"STRONG correlation. File 2 is a close copy of file 1{inv_note}.")
    elif verdict_ratio is not None and verdict_ratio < 0.6:
        print(f"PARTIAL correlation. File 2 resembles file 1{inv_note} but with many errors.")
    elif best is not None and abs(best[4]) >= 6:
        print(f"WEAK but real correlation at a shift of {best[0]:+d} bits "
              f"(z = {best[4]:+.1f}, well above chance).")
    else:
        print("NO meaningful correlation. The result is consistent with chance.")


if __name__ == "__main__":
    main()
