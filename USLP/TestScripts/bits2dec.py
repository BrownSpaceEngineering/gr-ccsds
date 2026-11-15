#!/usr/bin/env python3
"""Turn a pasted string of bits into 8-bit symbols and print each one in decimal.

The bits can be pasted with spaces, line breaks or an index prefix such as
"00000008: "; anything that is not a 0 or a 1 is ignored.

Ways to give it the bits:
  python3 bits2dec.py                  # prompts; paste the bits, then press Enter on an empty line
  python3 bits2dec.py -c               # read the bits from the clipboard (macOS pbpaste)
  python3 bits2dec.py "00000110 10111110 01000101 00000110"
  python3 bits2dec.py -f GFSKtest_bits.txt
  echo 0000011010111110 | python3 bits2dec.py

Options:
  --offset N      skip the first N bits (use when the symbols are not byte-aligned)
  --all-offsets   show the result for every possible bit alignment (0 to 7)
  --invert        flip every bit before converting
  -w N            symbol size in bits (default 8)
"""
import argparse
import subprocess
import sys


def clean_bits(text):
    bits = []
    for line in text.splitlines():
        if ":" in line:  # drop an index prefix such as "00000008: "
            line = line.split(":", 1)[1]
        bits.extend(c for c in line if c in "01")
    return "".join(bits)


def read_clipboard():
    for cmd in (["pbpaste"], ["wl-paste"], ["xclip", "-o", "-selection", "clipboard"]):
        try:
            return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    sys.exit("error: could not read the clipboard (pbpaste not found). "
             "Run without -c and paste the bits instead.")


def read_prompt():
    if not sys.stdin.isatty():
        return sys.stdin.read()
    print("Paste the bits, then press Enter on an empty line:")
    lines = []
    try:
        while True:
            line = input()
            if not line.strip() and lines:
                break
            lines.append(line)
    except EOFError:
        pass
    return "\n".join(lines)


def symbols(bits, width, offset):
    """Split a bit string into width-bit symbols; return (values, leftover_bits)."""
    bits = bits[offset:]
    usable = len(bits) - len(bits) % width
    values = [int(bits[i:i + width], 2) for i in range(0, usable, width)]
    return values, bits[usable:]


def format_values(values, per_line):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append(" ".join(str(v) for v in values[i:i + per_line]))
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description="Convert pasted bits to 8-bit symbols in decimal.")
    p.add_argument("bits", nargs="?", help="the bits as text (optional; otherwise you are prompted)")
    p.add_argument("-c", "--clipboard", action="store_true", help="read the bits from the clipboard")
    p.add_argument("-f", "--file", help="read the bits from a text file")
    p.add_argument("-w", "--width", type=int, default=8, help="bits per symbol (default: 8)")
    p.add_argument("--offset", type=int, default=0, help="skip this many bits at the start")
    p.add_argument("--all-offsets", action="store_true",
                   help="show the result for every bit alignment")
    p.add_argument("--invert", action="store_true", help="flip every bit first")
    p.add_argument("-n", "--per-line", type=int, default=16, help="symbols per line (default: 16)")
    p.add_argument("-o", "--output", help="also write the decimal values to this text file")
    args = p.parse_args()

    if args.width < 1 or args.per_line < 1 or args.offset < 0:
        p.error("--width and --per-line must be at least 1, and --offset cannot be negative")

    if args.bits is not None:
        raw = args.bits
    elif args.clipboard:
        raw = read_clipboard()
    elif args.file:
        try:
            with open(args.file, "r", errors="ignore") as f:
                raw = f.read()
        except FileNotFoundError:
            sys.exit(f"error: file not found: {args.file}")
    else:
        raw = read_prompt()

    bits = clean_bits(raw)
    if not bits:
        sys.exit("error: no 0s or 1s found in the input")
    if args.invert:
        bits = "".join("1" if c == "0" else "0" for c in bits)

    print(f"{len(bits)} bits -> {args.width}-bit symbols")
    offsets = range(args.width) if args.all_offsets else [args.offset]
    report = []
    for off in offsets:
        values, leftover = symbols(bits, args.width, off)
        head = f"offset {off}: {len(values)} symbol(s)"
        if leftover:
            head += f", {len(leftover)} leftover bit(s) ignored ({leftover})"
        body = format_values(values, args.per_line) if values else "(not enough bits)"
        report.append(f"{head}\n{body}")
    text = "\n\n".join(report)
    print(text)

    if args.output:
        with open(args.output, "w") as f:
            f.write(text + "\n")
        print(f"\nwrote {args.output}")


if __name__ == "__main__":
    main()
