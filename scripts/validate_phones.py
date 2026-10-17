"""Pre-release check: run generated numbers through Python's phonenumbers.

    pip install phonenumbers
    python scripts/validate_phones.py [--count 100000] [--clang clang++]

Builds scripts/dump_phones.cpp, draws numbers from every country, and reports
the share that phonenumbers calls valid (is_valid_number) and possible, per
country for the ones that fall short. The numbers come from libphonenumber's
patterns, so this checks the expansion into ranges and the formats, not the
data itself; a different libphonenumber release can disagree on a few ranges.
"""

import argparse
import collections
import subprocess
import sys
from pathlib import Path

import phonenumbers


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=100000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--clang", default="clang++")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    exe = root / "build" / "dump_phones"
    exe.parent.mkdir(exist_ok=True)
    subprocess.run([args.clang, "-std=c++23", "-O2", "-I", str(root), "-o", str(exe),
                    str(root / "scripts" / "dump_phones.cpp")], check=True)
    out = subprocess.run([str(exe), str(args.count), str(args.seed)], check=True, capture_output=True, text=True,
                         cwd=root).stdout

    total = collections.Counter()
    valid = collections.Counter()
    possible = collections.Counter()
    for line in out.splitlines():
        region, kind, e164 = line.split("\t")
        total[region] += 1
        try:
            num = phonenumbers.parse(e164, None)
        except phonenumbers.NumberParseException:
            continue
        valid[region] += phonenumbers.is_valid_number(num)
        possible[region] += phonenumbers.is_possible_number(num)

    n = sum(total.values())
    print(f"{n} numbers: {sum(valid.values()) / n:.4%} valid, {sum(possible.values()) / n:.4%} possible")
    for region in sorted(total, key=lambda r: valid[r] / total[r]):
        if valid[region] < total[region]:
            print(f"  {region}: {valid[region]}/{total[region]} valid, {possible[region]} possible")
    return 0


if __name__ == "__main__":
    sys.exit(main())
