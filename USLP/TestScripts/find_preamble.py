#!/usr/bin/env python3
"""Find a repeating 1010... (or 0101...) pattern in a bit stream, with error tolerance.

By default it looks for a 32-bit alternating pattern allowing up to 3 wrong bits.
Both phases (1010... and 0101...) are searched, since the receiver may lock on
either polarity. Consecutive overlapping hits are merged into one "run", which is
the preamble.

Input formats (picked automatically unless you say otherwise):
  packed bytes   a File Sink written after Pack K Bits (bits are read MSB first)
  --unpacked     one bit per byte (a File Sink straight off GFSK Demod)
  --text         a text file of 0s and 1s, such as GFSKtest_bits.txt from bin2bits.py
                 (plain 0/1 text files are detected automatically; use --text
                 if the file was made with --index)

Examples:
  python3 find_preamble.py GFSKtest.bin
  python3 find_preamble.py GFSKtest_bits.txt
  python3 find_preamble.py GFSKtest.bin --tolerance 4 --length 24
"""
import argparse
import sys


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


def find_hits(bits, length, tol):
    """Return (start_bit, errors, pattern_name) for every matching window."""
    mask = (1 << length) - 1
    pat_a = int(("10" * length)[:length], 2)  # 1010...
    pat_b = int(("01" * length)[:length], 2)  # 0101...
    reg = 0
    hits = []
    for i, b in enumerate(bits):
        reg = ((reg << 1) | b) & mask
        if i < length - 1:
            continue
        err_a = bin(reg ^ pat_a).count("1")
        err_b = bin(reg ^ pat_b).count("1")
        err, name = (err_a, "1010...") if err_a <= err_b else (err_b, "0101...")
        if err <= tol:
            hits.append((i - length + 1, err, name))
    return hits


def group_runs(hits, gap):
    runs = []
    for h in hits:
        if runs and h[0] - runs[-1][-1][0] <= gap:
            runs[-1].append(h)
        else:
            runs.append([h])
    return runs


def bitstr(bits):
    return "".join(map(str, bits))


def main():
    p = argparse.ArgumentParser(description="Find a 1010... preamble in a bit stream.")
    p.add_argument("infile")
    p.add_argument("-l", "--length", type=int, default=32,
                   help="pattern length in bits (default: 32)")
    p.add_argument("-t", "--tolerance", type=int, default=3,
                   help="allowed wrong bits per window (default: 3)")
    p.add_argument("--after", type=int, default=32,
                   help="bits to show after each run (default: 32)")
    p.add_argument("--unpacked", action="store_true",
                   help="input has one bit per byte")
    p.add_argument("--text", action="store_true",
                   help="input is a text file of 0s and 1s")
    args = p.parse_args()

    if args.length < 2:
        p.error("--length must be at least 2")
    if args.tolerance < 0 or args.tolerance >= args.length // 2:
        p.error("--tolerance must be at least 0 and less than half the length")

    try:
        bits, kind = load_bits(args.infile, args.unpacked, args.text)
    except FileNotFoundError:
        sys.exit(f"error: file not found: {args.infile}")

    print(f"read {len(bits)} bits ({kind})")
    print(f"looking for {args.length}-bit alternating pattern, "
          f"up to {args.tolerance} wrong bit(s)")

    hits = find_hits(bits, args.length, args.tolerance)
    if not hits:
        print("no preamble found. Try a larger --tolerance or a shorter --length, "
              "and check the signal strength.")
        return

    runs = group_runs(hits, args.length)
    print(f"{len(runs)} run(s) found\n")
    for k, run in enumerate(runs, 1):
        loose_start = run[0][0]
        loose_end = run[-1][0] + args.length
        # "core": the windows with the fewest errors. Windows that tolerate errors
        # can slide a few bits into whatever follows the preamble, so the core's
        # edges are the better guide to where the preamble really ends.
        best_err = min(h[1] for h in run)
        core = [h for h in run if h[1] == best_err]
        core_start = core[0][0]
        core_end = core[-1][0] + args.length
        name = core[0][2]
        context = (bitstr(bits[core_start:core_end]) + "|"
                   + bitstr(bits[core_end:core_end + args.after]))
        print(f"run {k}: {len(run)} matching window(s), pattern {name}")
        print(f"  loose extent (any window within tolerance): "
              f"bits {loose_start}-{loose_end - 1}")
        print(f"  solid core ({best_err} error(s)): bits {core_start}-{core_end - 1}, "
              f"byte {core_start // 8} bit {core_start % 8}")
        print(f"  core, then the next {args.after} bits after the '|':")
        print(f"  {context}")
        print("  (the data after the preamble, e.g. a sync word, starts at or just "
              "after the '|')\n")


if __name__ == "__main__":
    main()
