#!/usr/bin/env python3
"""Search a bit stream for a sync marker, with error tolerance.

The default marker is the four bytes [6, 190, 69, 6]. Written as bits (MSB first):
  00000110 10111110 01000101 00000110   (hex 06 BE 45 06)

Both bit polarities are searched, because the demodulator can lock either way. For
every match the script shows where it was found, how many bits were wrong, and the
bytes that follow the marker (read at the same bit alignment), in decimal.

Input formats (picked automatically unless you say otherwise):
  packed bytes   a File Sink written after Pack K Bits (bits are read MSB first)
  --unpacked     one bit per byte (a File Sink straight off GFSK Demod)
  --text         a text file of 0s and 1s, such as GFSKtest_bits.txt from bin2bits.py
                 (plain 0/1 text files are detected automatically; use --text
                 if the file was made with --index)

Examples:
  python3 find_sync.py GFSKtest.bin
  python3 find_sync.py GFSKtest_bits.txt --tolerance 2 --bytes 32
  python3 find_sync.py GFSKtest.bin --marker "6 190 69 6"
"""
import argparse
import re
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


def parse_marker(text):
    try:
        values = [int(tok, 0) for tok in re.split(r"[\s,\[\]]+", text.strip()) if tok]
    except ValueError:
        sys.exit(f"error: cannot read marker {text!r}; use numbers like \"6 190 69 6\"")
    if not values or any(not 0 <= v <= 255 for v in values):
        sys.exit("error: marker must be one or more byte values from 0 to 255")
    return values


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


def find_hits(bits, marker_val, length, tol):
    """Return (start_bit, errors) for every window within tolerance of the marker."""
    mask = (1 << length) - 1
    reg = 0
    hits = []
    for i, b in enumerate(bits):
        reg = ((reg << 1) | b) & mask
        if i < length - 1:
            continue
        err = bin(reg ^ marker_val).count("1")
        if err <= tol:
            hits.append((i - length + 1, err))
    return hits


def best_per_cluster(hits, length):
    """Keep the best hit from each group of nearby hits."""
    best = []
    for h in hits:
        if best and h[0] - best[-1][0] < length and h[1] < best[-1][1]:
            best[-1] = h
        elif best and h[0] - best[-1][0] < length:
            continue
        else:
            best.append(h)
    return best


def main():
    p = argparse.ArgumentParser(description="Find a sync marker in a bit stream.")
    p.add_argument("infile")
    p.add_argument("-m", "--marker", default="6 190 69 6",
                   help='marker bytes in decimal (default: "6 190 69 6")')
    p.add_argument("-t", "--tolerance", type=int, default=3,
                   help="allowed wrong bits in the marker (default: 3)")
    p.add_argument("-b", "--bytes", type=int, default=16,
                   help="bytes to show after each match (default: 16)")
    p.add_argument("--no-invert", action="store_true",
                   help="do not also search for the inverted bit pattern")
    p.add_argument("--unpacked", action="store_true",
                   help="input has one bit per byte")
    p.add_argument("--text", action="store_true",
                   help="input is a text file of 0s and 1s")
    args = p.parse_args()

    marker = parse_marker(args.marker)
    length = len(marker) * 8
    marker_bits = [(v >> k) & 1 for v in marker for k in range(7, -1, -1)]
    marker_val = int(bitstr(marker_bits), 2)
    if args.tolerance < 0 or args.tolerance >= length // 4:
        p.error(f"--tolerance must be at least 0 and less than {length // 4} "
                f"for a {length}-bit marker")

    try:
        bits, kind = load_bits(args.infile, args.unpacked, args.text)
    except FileNotFoundError:
        sys.exit(f"error: file not found: {args.infile}")

    print(f"marker: {marker}  ->  {' '.join(format(v, '08b') for v in marker)}")
    print(f"read {len(bits)} bits ({kind}); tolerance {args.tolerance} wrong bit(s)\n")

    results = []  # (start, errors, polarity)
    polarities = [("normal", bits)]
    if not args.no_invert:
        polarities.append(("inverted", [b ^ 1 for b in bits]))
    for label, stream in polarities:
        hits = best_per_cluster(find_hits(stream, marker_val, length, args.tolerance), length)
        results.extend((s, e, label, stream) for s, e in hits)
    results.sort(key=lambda r: r[0])

    if not results:
        print("marker not found. Try a larger --tolerance, and check the preamble "
              "and signal first (find_preamble.py).")
        return

    print(f"{len(results)} match(es)\n")
    for k, (start, err, label, stream) in enumerate(results, 1):
        found = stream[start:start + length]
        after = bits_to_bytes(stream[start + length:start + length + args.bytes * 8])
        print(f"match {k}: bit {start} (byte {start // 8} bit {start % 8}), "
              f"{label} polarity, {err} error(s)")
        print(f"  marker: {bitstr(marker_bits)}")
        print(f"  found : {bitstr(found)}")
        if err:
            diff = "".join("^" if a != b else " " for a, b in zip(marker_bits, found))
            print(f"          {diff}")
        shown = " ".join(str(v) for v in after) if after else "(end of data)"
        print(f"  next {len(after)} byte(s), decimal: {shown}\n")


if __name__ == "__main__":
    main()
