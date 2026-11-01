#!/usr/bin/env python3
"""Convert a raw binary capture (e.g. GNU Radio File Sink output) into
decimal unsigned byte values (0-255).

The input is read in binary mode. Despite a .txt extension, a File Sink
file holds raw bytes, so it must not be read as text.

Examples:
    python3 bytes_to_decimal.py bytes.txt
    python3 bytes_to_decimal.py bytes.txt -o decimal.txt
    python3 bytes_to_decimal.py bytes.txt -n 1            # one value per line
    python3 bytes_to_decimal.py bytes.txt -n 0 -s ","     # one comma-separated line
"""

import argparse
import sys


def main():
    p = argparse.ArgumentParser(
        description="Convert raw bytes to decimal unsigned values (0-255)."
    )
    p.add_argument("input", help="raw byte file (e.g. bytes.txt from the File Sink)")
    p.add_argument("-o", "--output", help="output text file (default: stdout)")
    p.add_argument(
        "-n",
        "--per-line",
        type=int,
        default=16,
        help="values per line (default 16; 0 puts everything on one line)",
    )
    p.add_argument(
        "-s", "--sep", default=" ", help="separator between values (default: space)"
    )
    args = p.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    if not data:
        print("warning: input file is empty", file=sys.stderr)
        return

    per_line = args.per_line if args.per_line > 0 else len(data)

    # Indexing a bytes object yields ints in 0-255, so no sign handling is needed.
    lines = [
        args.sep.join(str(b) for b in data[i : i + per_line])
        for i in range(0, len(data), per_line)
    ]
    text = "\n".join(lines) + "\n"

    if args.output:
        with open(args.output, "w") as f:
            f.write(text)
        print(f"wrote {len(data)} values to {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
