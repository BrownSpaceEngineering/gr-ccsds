#!/usr/bin/env python3
"""analyze_link.py - one script for the whole bit-level link analysis.

Give it the received capture (and, for the Levenshtein step, the transmitted file).

  step 0  find the most likely PREAMBLE (the longest alternating 1010.../0101... run,
          preferably one that is followed by a header)
  step 1  find every packet HEADER (sync marker, default [6 190 69 6]) in the capture,
          tolerating flipped, dropped and extra bits; both polarities are tried and the
          stream is un-inverted automatically if the demodulator locked the other way
  step 2  cut the received stream and the transmitted stream at their headers, pair
          each received packet with its transmitted packet, and compute the Levenshtein
          (edit) distance between them. The edit script is split into flipped bits,
          dropped bits and extra bits.
  step 3  write the received packets out as binary: <name>_packets.txt (one packet per
          line, as 0s and 1s) and <name>_packets.bin (packed bytes, MSB first; every
          packet is padded with zeros to a whole byte)
  step 4  count the dropped bits per packet against the length a packet should have
          (default 8224 bits)

A "packet" is everything from one header's first bit up to the first bit of the next
header. 8224 = 32 marker bits + 8192 payload bits (1024 bytes), so with the default
settings the preamble is not part of a packet.

dropped (net) = expected length - received length. Positive means bits were lost,
negative means extra bits appeared. The alignment columns (flip / drop / extra) come
from the Levenshtein alignment against the transmitted packet, so they show how many
bits were lost AND gained, and how many were merely flipped.

Input formats (picked automatically unless you say otherwise):
  packed bytes   a File Sink written after Pack K Bits (bits are read MSB first)
  unpacked       one bit per byte (a File Sink straight off GFSK Demod)
  text           a text file of 0s and 1s, such as GFSKtest_bits.txt from bin2bits.py

Examples:
  python3 analyze_link.py rx.bin --tx tx.bin
  python3 analyze_link.py rx.bin --tx tx.bin --packet-bits 8224 --detail
  python3 analyze_link.py rx.bin                      (no --tx: steps 0, 1, 3, 4 only)
  python3 analyze_link.py rx.bin --tx tx_bits.txt --rx-format unpacked --include-last
"""
import argparse
import os
import re
import sys
from collections import Counter

try:
    import numpy as np
except ImportError:  # numpy is only needed for the preamble scan and the alignment
    np = None


# ================================================================== input ==========
def load_bits(path, fmt):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        sys.exit(f"error: file not found: {path}")
    if not data:
        sys.exit(f"error: {path} is empty")
    if fmt == "auto":
        # a file made only of '0', '1' and whitespace is a bit-text file
        fmt = "text" if not data.translate(None, b"01 \t\r\n") else "packed"
    if fmt == "text":
        bits = []
        for line in data.decode("ascii", errors="ignore").splitlines():
            if ":" in line:  # drop an --index prefix such as "00000008: "
                line = line.split(":", 1)[1]
            bits.extend(1 if c == "1" else 0 for c in line if c in "01")
        return bits, "text"
    if fmt == "unpacked":
        return [b & 1 for b in data], "one bit per byte"
    return [(b >> k) & 1 for b in data for k in range(7, -1, -1)], "packed bytes"


def parse_marker(text):
    try:
        values = [int(tok, 0) for tok in re.split(r"[\s,\[\]]+", text.strip()) if tok]
    except ValueError:
        sys.exit(f"error: cannot read marker {text!r}; use numbers like \"6 190 69 6\"")
    if not values or any(not 0 <= v <= 255 for v in values):
        sys.exit("error: marker must be one or more byte values from 0 to 255")
    return values, [(v >> k) & 1 for v in values for k in range(7, -1, -1)]


def bitstr(bits):
    return "".join(map(str, bits))


def pack_bits(bits):
    """MSB first; the last byte is padded with zeros."""
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8]
        v = 0
        for b in chunk:
            v = (v << 1) | b
        out.append(v << (8 - len(chunk)))
    return bytes(out)


# ========================================================= approximate search ======
def edit_hits(text, pattern, max_err):
    """(end_index, errors) wherever a stretch of `text` ending there is within
    max_err edits of `pattern`. Myers' bit-vector algorithm."""
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


def align_small(pattern, window):
    """Align the whole pattern to the end of `window`; the start is free.
    Returns (offset_in_window, ops, errors). ops: M match, S flip, D dropped, E extra."""
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


def find_headers(bits, marker_bits, max_err, min_gap, both=True):
    """Return (polarity_label, headers). Each header: dict(start, end, errors, ops)."""
    m = len(marker_bits)
    searches = [("normal", marker_bits)]
    if both:
        searches.append(("inverted", [b ^ 1 for b in marker_bits]))
    per_polarity = {}
    for label, pat in searches:
        found = []
        for j, _score in best_per_cluster(edit_hits(bits, pat, max_err), m):
            base = max(0, j - (m + max_err) + 1)
            off, ops, errs = align_small(pat, bits[base:j + 1])
            found.append(dict(start=base + off, end=j, errors=errs, ops="".join(ops)))
        found.sort(key=lambda h: h["start"])
        # a real header is far from any other: keep the better of two close hits
        kept = []
        for h in found:
            if kept and h["start"] - kept[-1]["start"] < min_gap:
                if h["errors"] < kept[-1]["errors"]:
                    kept[-1] = h
            else:
                kept.append(h)
        per_polarity[label] = kept
    # the polarity with more headers wins; ties go to the one with fewer errors
    label = max(per_polarity, key=lambda k: (len(per_polarity[k]),
                                             -sum(h["errors"] for h in per_polarity[k])))
    return label, per_polarity[label], {k: len(v) for k, v in per_polarity.items()}


# ======================================================================= step 0 ====
def preamble_runs(bits, length, tol):
    """Runs of an alternating 1010.../0101... pattern. A window of `length` bits is a
    hit when at most `tol` bits differ from a perfect alternating pattern."""
    n = len(bits)
    if np is None or n < length:
        return []
    arr = np.asarray(bits, dtype=np.int8)
    y = arr ^ (np.arange(n) & 1).astype(np.int8)  # constant over a perfect alternation
    cs = np.concatenate(([0], np.cumsum(y, dtype=np.int64)))
    ones = cs[length:] - cs[:-length]
    err = np.minimum(ones, length - ones)
    starts = np.nonzero(err <= tol)[0]
    if starts.size == 0:
        return []
    groups = np.split(starts, np.nonzero(np.diff(starts) > length)[0] + 1)
    runs = []
    for g in groups:
        e = err[g]
        best = int(e.min())
        core = g[e == best]
        c0, c1 = int(core[0]), int(core[-1]) + length
        runs.append(dict(loose=(int(g[0]), int(g[-1]) + length), core=(c0, c1),
                         errors=best, windows=len(g)))
    return runs


def step0_preamble(bits, headers, args):
    print("=" * 78)
    print("STEP 0  most likely preamble (alternating 1010.../0101...)")
    print("=" * 78)
    tried = []
    runs, used = [], None
    for length, tol in ((args.preamble_len, args.preamble_tol),
                        (24, min(args.preamble_tol, 2)), (16, 1)):
        if length > args.preamble_len or length in tried:
            continue
        tried.append(length)
        runs = preamble_runs(bits, length, tol)
        if runs:
            used = (length, tol)
            break
    if np is None:
        print("numpy is not installed; skipped (pip install numpy).\n")
        return None
    if not runs:
        print(f"no alternating run of {tried} bits found. Check the signal strength, or "
              f"raise --preamble-tol.\n")
        return None
    starts = [h["start"] for h in headers]
    for r in runs:
        r["len"] = r["core"][1] - r["core"][0]
        r["hdr"] = None
        for k, s in enumerate(starts):
            gap = s - r["core"][1]
            if -8 <= gap <= 16:
                r["hdr"] = (k + 1, gap)
                break
    runs.sort(key=lambda r: (r["hdr"] is not None, r["len"], -r["errors"]), reverse=True)
    best = runs[0]
    c0, c1 = best["core"]
    pat = "1010..." if bits[c0] == 1 else "0101..."
    print(f"window {used[0]} bits, up to {used[1]} wrong bit(s); {len(runs)} run(s) found")
    print(f"MOST LIKELY PREAMBLE: stream bits {c0}-{c1 - 1} ({best['len']} bits, "
          f"{best['len'] // 8} bytes, pattern {pat}, {best['errors']} wrong bit(s) "
          f"in its best windows)")
    if best["hdr"]:
        print(f"  followed by header #{best['hdr'][0]} (gap {best['hdr'][1]} bits)")
    else:
        print("  no header follows it closely")
    shown = bitstr(bits[c0:c1])
    if len(shown) > 80:
        shown = shown[:40] + " ... " + shown[-40:]
    print(f"  {shown} | {bitstr(bits[c1:c1 + 32])}")
    if len(runs) > 1:
        print(f"\nother runs (top {min(args.top, len(runs) - 1)} of {len(runs) - 1}):")
        for r in runs[1:1 + args.top]:
            tail = f", before header #{r['hdr'][0]}" if r["hdr"] else ""
            print(f"  bits {r['core'][0]}-{r['core'][1] - 1}: {r['len']} bits, "
                  f"{r['errors']} wrong{tail}")
    print()
    return best


# ======================================================================= step 1 ====
def step1_headers(label, headers, counts, marker_vals, marker_bits, total_bits, args):
    print("=" * 78)
    print(f"STEP 1  packet headers (sync marker {marker_vals})")
    print("=" * 78)
    print(f"marker bits: {bitstr(marker_bits)}")
    print(f"hits by polarity: normal {counts['normal']}, inverted {counts.get('inverted', 0)}"
          f"  ->  using {label}" + ("  (stream inverted for all later steps)"
                                     if label == "inverted" else ""))
    if not headers:
        print("no header found. Raise --errors, and check step 0 and the signal first.\n")
        return
    print(f"{len(headers)} header(s) (min spacing {args.min_gap} bits, up to "
          f"{args.errors} error(s) each)\n")
    print("  #    start bit   byte.bit   errors   alignment")
    prev = None
    for k, h in enumerate(headers, 1):
        gap = "" if prev is None else f"   +{h['start'] - prev} bits since previous"
        ops = h["ops"].replace("M", ".")
        print(f"{k:3d}  {h['start']:10d}   {h['start'] // 8:7d}.{h['start'] % 8}   "
              f"{h['errors']:6d}   {ops}{gap}")
        prev = h["start"]
    print("  (alignment: . match  S flipped  D dropped  E extra)\n")


# ======================================================================= step 2 ====
def myers_global(a, b):
    """Levenshtein distance between bit lists a and b (Myers/Hyyro bit-vector)."""
    m = len(a)
    if m == 0:
        return len(b)
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
        ph = ((ph << 1) | 1) & mask  # global alignment: the text start is not free
        mh = (mh << 1) & mask
        pv = mh | (~(xv | ph) & mask)
        mv = ph & xv
    return score


def band_align(a, b, d, max_cells):
    """Optimal global alignment of a (transmitted) to b (received), restricted to the
    band the distance d allows. Returns dict(flips, drops, extras, drop_pos,
    extra_pos) or None when the band is larger than max_cells."""
    m, n = len(a), len(b)
    lo = min(0, n - m) - d
    hi = max(0, n - m) + d
    width = hi - lo + 1
    if np is None or (m + 1) * width > max_cells:
        return None
    INF = 30000
    A = np.asarray(a, dtype=np.int8)
    B = np.asarray(b, dtype=np.int8)
    ks = lo + np.arange(width)
    ramp = np.arange(width, dtype=np.int32)
    D = np.full((m + 1, width), INF, dtype=np.int16)
    j0 = ks
    ok0 = (j0 >= 0) & (j0 <= n)
    D[0, ok0] = j0[ok0]
    for i in range(1, m + 1):
        js = i + ks
        valid = (js >= 0) & (js <= n)
        prev = D[i - 1].astype(np.int32)
        cost = (B[np.clip(js - 1, 0, n - 1)] != A[i - 1]).astype(np.int32)
        diag = np.where(js >= 1, prev + cost, INF)
        up = np.full(width, INF, dtype=np.int32)
        up[:-1] = prev[1:] + 1
        base = np.where(valid, np.minimum(diag, up), INF)
        row = ramp + np.minimum.accumulate(base - ramp)  # allow runs of left moves
        row = np.where(valid, np.minimum(row, INF), INF)
        D[i] = row
    i, j, x = m, n, (n - m) - lo
    flips = drops = extras = 0
    drop_pos, extra_pos = [], []
    while i > 0 or j > 0:
        cur = int(D[i, x])
        if i > 0 and j > 0:
            cost = 0 if a[i - 1] == b[j - 1] else 1
            if cur == int(D[i - 1, x]) + cost:
                flips += cost
                i -= 1
                j -= 1
                continue
        if i > 0 and x + 1 < width and cur == int(D[i - 1, x + 1]) + 1:
            drops += 1
            drop_pos.append(i - 1)  # index of the transmitted bit that never arrived
            i -= 1
            x += 1
        else:
            extras += 1
            extra_pos.append(i)  # an extra bit arrived after transmitted bit i-1
            j -= 1
            x -= 1
    drop_pos.reverse()
    extra_pos.reverse()
    return dict(flips=flips, drops=drops, extras=extras,
                drop_pos=drop_pos, extra_pos=extra_pos)


def cut_packets(bits, headers, include_last):
    """Packets run from one header to the next. The last one has no closing header."""
    packets = []
    for k, h in enumerate(headers):
        s = h["start"]
        if k + 1 < len(headers):
            packets.append(dict(start=s, bits=bits[s:headers[k + 1]["start"]], open=False))
        else:
            packets.append(dict(start=s, bits=bits[s:], open=True))
    if packets and packets[-1]["open"] and not include_last:
        packets[-1]["counted"] = False
    for p in packets:
        p.setdefault("counted", True)
    return packets


def classify(length, expected):
    ratio = length / expected
    if ratio > 1.5:
        return "merged"  # a header was missed, or a whole packet was lost
    if ratio < 0.5:
        return "short"  # a false header, or a truncated packet
    return "ok"


def compact(positions, limit=10):
    if not positions:
        return "-"
    shown = ", ".join(map(str, positions[:limit]))
    return shown + (f", ... (+{len(positions) - limit} more)" if len(positions) > limit else "")


def step2_and_4(rx_packets, tx_packets, args):
    print("=" * 78)
    print("STEP 2  Levenshtein distance per packet" + ("" if tx_packets else "   (skipped)"))
    print("STEP 4  dropped bits per packet vs expected length "
          f"({args.packet_bits})")
    print("=" * 78)
    expected = args.packet_bits
    if tx_packets:
        lens = Counter(len(p["bits"]) for p in tx_packets)
        common, count = lens.most_common(1)[0]
        print(f"TX: {len(tx_packets)} packet(s); header-to-header length "
              f"{common} bits" + (f" (x{count})" if len(lens) > 1 else ""))
        if len(lens) > 1:
            print(f"    TX lengths vary: {dict(lens)}")
        if common != expected:
            print(f"    WARNING: TX packets are {common} bits, not {expected}. If the "
                  f"transmitter really sends {common}, use --packet-bits {common}.")
        uniq = {}
        for i, p in enumerate(tx_packets):
            uniq.setdefault(bitstr(p["bits"]), i)
        print(f"    {len(uniq)} distinct TX packet content(s)")
    print()

    counted = [p for p in rx_packets if p["counted"]]
    tx_bits = [p["bits"] for p in tx_packets] if tx_packets else []
    n_tx = len(tx_bits)
    prev_idx = None
    rows = []
    for k, p in enumerate(rx_packets, 1):
        p["no"] = k
        length = len(p["bits"])
        if not p["counted"]:
            p["status"] = "open"
            continue
        p["status"] = classify(length, expected)
        mult = max(1, round(length / expected)) if p["status"] == "merged" else 1
        p["dropped"] = mult * expected - length
        p["mult"] = mult
        p.update(lev=None, flips=None, drops=None, extras=None, tx=None)
        if tx_bits and p["status"] != "merged":
            if prev_idx is None:
                cands = list(range(n_tx))
            else:
                cands = [(prev_idx + 1 + t) % n_tx for t in range(args.lookahead + 1)]
            best = None
            seen = set()
            for c in cands:
                key = bitstr(tx_bits[c])
                if key in seen:  # identical TX packets give identical distances
                    continue
                seen.add(key)
                dist = myers_global(tx_bits[c], p["bits"])
                if best is None or dist < best[0]:
                    best = (dist, c)
            p["lev"], p["tx"] = best
            prev_idx = best[1]
            if not args.no_breakdown and p["lev"] > 0:
                res = band_align(tx_bits[best[1]], p["bits"], p["lev"], args.max_cells)
                if res:
                    p.update(res)
            elif p["lev"] == 0:
                p.update(flips=0, drops=0, extras=0, drop_pos=[], extra_pos=[])
    # ---- table
    head = "  rx#    start bit    rx len  dropped(net)"
    if tx_bits:
        head += "   tx#  lev dist   flip  drop  extra"
    print(head + "   note")
    for p in rx_packets:
        if not p["counted"]:
            print(f"{p['no']:5d}  {p['start']:11d}  {len(p['bits']):8d}  "
                  f"{'-':>12}" + ("   " + " " * 32 if tx_bits else "") +
                  "   open-ended: runs to the end of the capture, not counted "
                  "(--include-last to count it)")
            continue
        note = ""
        if p["status"] == "merged":
            note = (f"MERGED: about {p['mult']} packets with no header between them "
                    f"(missed header or lost packet)")
        elif p["status"] == "short":
            note = "SHORT: false header or truncated packet"
        elif p["dropped"] > 0:
            note = "bits lost"
        elif p["dropped"] < 0:
            note = "extra bits"
        line = f"{p['no']:5d}  {p['start']:11d}  {len(p['bits']):8d}  {p['dropped']:+12d}"
        if tx_bits:
            if p["lev"] is None:
                line += "   " + "-".rjust(4) + "  " + "-".rjust(8) + "      -     -      -"
            else:
                def f(v):
                    return "-" if v is None else str(v)
                line += (f"   {p['tx'] + 1:4d}  {p['lev']:8d}  {f(p['flips']):>5}  "
                         f"{f(p['drops']):>4}  {f(p['extras']):>5}")
        print(line + ("   " + note if note else ""))
        if args.detail and tx_bits and p.get("drop_pos") is not None and p["lev"]:
            print(f"        drops at TX packet bits : {compact(p['drop_pos'])}")
            print(f"        extras after TX bits    : {compact(p['extra_pos'])}")
    # ---- summary
    print()
    ok = [p for p in counted if p["status"] == "ok"]
    total = len(counted)
    print(f"SUMMARY  ({len(rx_packets)} header(s), {total} packet(s) counted)")
    if not total:
        print("  nothing to summarise.")
        return counted
    exact = sum(1 for p in counted if len(p["bits"]) == expected and p["status"] == "ok")
    lost = sum(p["dropped"] for p in counted if p["dropped"] > 0)
    gained = -sum(p["dropped"] for p in counted if p["dropped"] < 0)
    print(f"  packets with exactly {expected} bits : {exact} of {total}")
    print(f"  packets that lost bits (net)    : {sum(1 for p in counted if p['dropped'] > 0)}"
          f"   ({lost} bits in total)")
    print(f"  packets with extra bits (net)   : {sum(1 for p in counted if p['dropped'] < 0)}"
          f"   ({gained} bits in total)")
    print(f"  net bits dropped over all       : {sum(p['dropped'] for p in counted)} "
          f"of {expected * total} expected")
    merged = [p for p in counted if p["status"] == "merged"]
    if merged:
        print(f"  merged segments                 : {len(merged)} (header missed or "
              f"packet lost; raise --errors to catch headers with more damage)")
    if tx_bits:
        scored = [p for p in counted if p["lev"] is not None]
        if scored:
            perfect = sum(1 for p in scored if p["lev"] == 0)
            tot_lev = sum(p["lev"] for p in scored)
            tot_bits = sum(len(p["bits"]) for p in scored)
            print(f"  bit-perfect packets             : {perfect} of {len(scored)}")
            print(f"  total Levenshtein distance      : {tot_lev} over {tot_bits} bits "
                  f"(mean {tot_lev / len(scored):.1f} per packet, "
                  f"{tot_lev / tot_bits:.2e} per bit)")
            br = [p for p in scored if p.get("flips") is not None]
            if br:
                print(f"  of which (alignment)            : "
                      f"{sum(p['flips'] for p in br)} flipped, "
                      f"{sum(p['drops'] for p in br)} dropped, "
                      f"{sum(p['extras'] for p in br)} extra")
                pure = sum(1 for p in br if p["lev"] and not p["drops"] and not p["extras"])
                slip = sum(1 for p in br if p["drops"] or p["extras"])
                print(f"  packets with only flips         : {pure};  with slips "
                      f"(drops/extras): {slip}")
    print()
    return counted


# ======================================================================= step 3 ====
def step3_write(counted, prefix, marker_len, args):
    print("=" * 78)
    print("STEP 3  binary output")
    print("=" * 78)
    packets = [p for p in counted if p["bits"]]
    if args.payload_only:
        packets = [dict(bits=p["bits"][marker_len:]) for p in packets]
    if args.no_output or not packets:
        print("nothing written.\n")
        return
    txt_path, bin_path = prefix + "_packets.txt", prefix + "_packets.bin"
    with open(txt_path, "w") as f:
        for p in packets:
            f.write(bitstr(p["bits"]) + "\n")
    with open(bin_path, "wb") as f:
        for p in packets:
            f.write(pack_bits(p["bits"]))
    what = "payload only (marker removed)" if args.payload_only else "marker + payload"
    odd = sum(1 for p in packets if len(p["bits"]) % 8)
    print(f"{len(packets)} packet(s), {what}, polarity-corrected")
    print(f"  {txt_path}   one packet per line, 0s and 1s")
    print(f"  {bin_path}   packed bytes (MSB first)")
    if odd:
        print(f"  note: {odd} packet(s) are not a whole number of bytes (bits were "
              f"dropped or added); each is zero-padded to a byte boundary in the .bin")
    print()


# ======================================================================== main =====
def main():
    p = argparse.ArgumentParser(
        description="Preamble, headers, per-packet Levenshtein distance and dropped-bit "
                    "count for a received bit stream.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See the top of this file for the meaning of every column.")
    p.add_argument("rx", help="received capture (the file written by the RX flowgraph)")
    p.add_argument("--tx", help="transmitted file, for the Levenshtein step")
    p.add_argument("-m", "--marker", default="6 190 69 6",
                   help='header marker as decimal bytes (default: "6 190 69 6")')
    p.add_argument("-e", "--errors", type=int, default=3,
                   help="errors allowed in a received header: flipped + dropped + "
                        "extra (default: 3)")
    p.add_argument("--packet-bits", type=int, default=8224,
                   help="bits a packet should contain, header to header (default: 8224)")
    p.add_argument("--min-gap", type=int,
                   help="closest two headers can be, in bits (default: half a packet)")
    p.add_argument("--preamble-len", type=int, default=32,
                   help="alternating window for the preamble scan (default: 32)")
    p.add_argument("--preamble-tol", type=int, default=3,
                   help="wrong bits allowed in that window (default: 3)")
    p.add_argument("--top", type=int, default=5,
                   help="other preamble runs to list (default: 5)")
    p.add_argument("--format", choices=["auto", "packed", "unpacked", "text"],
                   default="auto", help="input format of both files (default: auto)")
    p.add_argument("--rx-format", choices=["auto", "packed", "unpacked", "text"])
    p.add_argument("--tx-format", choices=["auto", "packed", "unpacked", "text"])
    p.add_argument("--include-last", action="store_true",
                   help="also count the last packet (it has no closing header)")
    p.add_argument("--lookahead", type=int, default=3,
                   help="TX packets ahead to try when pairing, to ride over lost "
                        "packets (default: 3)")
    p.add_argument("--detail", action="store_true",
                   help="list where in each packet bits were dropped")
    p.add_argument("--no-breakdown", action="store_true",
                   help="skip the flip/drop/extra alignment (faster)")
    p.add_argument("--max-cells", type=int, default=60_000_000,
                   help="alignment size limit, in cells (default: 60 million)")
    p.add_argument("-o", "--output", help="output prefix (default: <rx file name>)")
    p.add_argument("--payload-only", action="store_true",
                   help="step 3: write the packets without their marker")
    p.add_argument("--no-output", action="store_true", help="do not write step 3 files")
    args = p.parse_args()

    marker_vals, marker_bits = parse_marker(args.marker)
    if args.errors < 0 or args.errors >= len(marker_bits) // 4:
        p.error(f"--errors must be 0 to {len(marker_bits) // 4 - 1} for a "
                f"{len(marker_bits)}-bit marker")
    if args.packet_bits <= len(marker_bits):
        p.error("--packet-bits must be larger than the marker")
    if args.min_gap is None:
        args.min_gap = args.packet_bits // 2
    if args.preamble_len < 8 or args.preamble_tol >= args.preamble_len // 2:
        p.error("--preamble-len must be at least 8 and --preamble-tol under half of it")
    rx_fmt = args.rx_format or args.format
    tx_fmt = args.tx_format or args.format

    rx, kind = load_bits(args.rx, rx_fmt)
    print(f"RX  {args.rx}: {len(rx)} bits ({kind})")
    tx = None
    if args.tx:
        tx, tkind = load_bits(args.tx, tx_fmt)
        print(f"TX  {args.tx}: {len(tx)} bits ({tkind})")
    print()

    # headers first: they decide the polarity, and the later steps need them
    label, headers, counts = find_headers(rx, marker_bits, args.errors, args.min_gap)
    stream = [b ^ 1 for b in rx] if label == "inverted" else rx

    step0_preamble(stream, headers, args)
    step1_headers(label, headers, counts, marker_vals, marker_bits, len(rx), args)
    if not headers:
        sys.exit(1)

    rx_packets = cut_packets(stream, headers, args.include_last)

    tx_packets = None
    if tx is not None:
        tx_headers = find_headers(tx, marker_bits, 0, args.min_gap, both=False)[1]
        if not tx_headers:
            print("warning: the marker was not found in the TX file; skipping the "
                  "Levenshtein step.\n")
        else:
            tx_packets = cut_packets(tx, tx_headers, True)
            last = tx_packets[-1]
            if len(last["bits"]) != args.packet_bits:  # an unfinished tail is not a packet
                tx_packets = tx_packets[:-1] or None
    counted = step2_and_4(rx_packets, tx_packets, args)

    prefix = args.output or os.path.splitext(args.rx)[0]
    step3_write(counted, prefix, len(marker_bits), args)


if __name__ == "__main__":
    main()
