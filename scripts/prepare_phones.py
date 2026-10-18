"""Build the phone-number tables from libphonenumber's PhoneNumberMetadata.xml.

    python scripts/prepare_phones.py [--cache build/phones] [--out resources] [--countries ../country-generator/resources]
                                     [--max-depth 7]

Downloads PhoneNumberMetadata.xml of the pinned release (cached under --cache)
and writes, per tier (the regions of country-generator's lite and full tiers
that libphonenumber has; the non-geographic '001' entries are skipped):

  countries.tsv   region, calling code, international prefix, national prefix,
                  main (1 when the region is the main one for its calling
                  code), population (country-generator's, for weighting)
  ranges.tsv      region, type (mobile or fixed), prefix, mask, length,
                  weight, format
  formats.tsv     id, groups (lengths joined with '-'), national, international
  reserved.tsv    region, type, prefix, mask, length, weight: the numbers the
                  regulators set aside for fiction (the 'safe' option); same
                  columns as ranges.tsv, without the format

A range is a digit prefix plus a mask for the digits after it: '#' a digit,
'[2-9]' a class, '#{6}' six of the last. The regular expression of each type
is compiled to an automaton and walked digit by digit until the rest of the
number is a plain product of per-position digit sets, so the runtime needs no
regular expressions and every number it draws is valid. 'weight' is the range's
share of the draws (its numbers over its length's, scaled);
'length' is the national number length. Toll-free, premium and the other
types are left out; so are lengths that would pass 15 digits with the calling
code (E.164). Within a length every valid number is equally likely; where a
plan has several lengths, the length of libphonenumber's example number gets
the draws of one, and every digit away from it a quarter of the length before
(LENGTH_DECAY), because the plans list every length that is allowed, not how
common it is.

Formats come from the territory's numberFormat list: the first entry whose
leading-digits pattern and pattern fit a sample number of the range. Templates
use $1, $2 ... for the digit groups; the national template has the national
prefix rule applied ('0$1 $2', '($1) $2-$3'), and an international template
of '-' means the format has none (the number is written without separators).

The regular expressions are test data (tests/data/phone_regex.tsv, written when
--regex-out is given) and are not shipped.
"""

import argparse
import csv
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET
from functools import lru_cache
from pathlib import Path

VERSION = "v9.0.41"
URL = f"https://raw.githubusercontent.com/google/libphonenumber/{VERSION}/resources/PhoneNumberMetadata.xml"
TYPES = (("mobile", "mobile"), ("fixedLine", "fixed"))
DIGITS = "0123456789"
E164_DIGITS = 15
# Curated: a number plan with several lengths is drawn mostly at the length of its example number
# (libphonenumber gives no frequencies): each digit away from it, a length gets this factor of the draws.
LENGTH_DECAY = 0.25
WEIGHT_SCALE = 1e9


# Curated (rule 6): reserved ranges confirmed on the regulators' pages, in
# national significant numbers (no national prefix). See build/survey.md; GB
# geographic, AU beyond (02) 5550, FR, IE and NZ are unconfirmed and left out.
RESERVED = [
    # Bundesnetzagentur "Drama numbers": 000-999 after the fixed part
    ("DE", "fixed", "3023125", "###", 10),
    ("DE", "fixed", "69" "90009", "###", 10),
    ("DE", "fixed", "40" "66969", "###", 10),
    ("DE", "fixed", "221" "4710", "###", 10),
    ("DE", "fixed", "89" "99998", "###", 10),
    ("DE", "mobile", "171" "39200", "##", 10),
    ("DE", "mobile", "176" "04069", "###", 11),
    # Ofcom: 07700 900000 to 900999
    ("GB", "mobile", "7700900", "###", 10),
    # ACMA: (02) 5550 xxxx
    ("AU", "fixed", "25550", "####", 9),
]
# NANPA: 555-0100 to 555-0199 under any assigned area code
NANP_REGIONS = ("US", "CA")


def reserved_rows(regexes):
    rows = []
    for region, kind, prefix, mask, length in RESERVED:
        rows.append([region, kind, prefix, mask, str(length), str(10 ** mask.count("#"))])
    for region in NANP_REGIONS:
        for kind in ("fixed", "mobile"):
            rx = re.compile(regexes[(region, kind)])
            # area codes under which both ends of 555-0100..0199 are valid numbers
            for code in map(str, range(200, 1000)):
                if rx.fullmatch(code + "5550100") and rx.fullmatch(code + "5550199"):
                    rows.append([region, kind, code + "55501", "##", "10", "100"])
    return rows


def check_country_generator(countries, path):
    """Cross-checks calling codes against country-generator's idd_* columns."""
    import csv
    if not Path(path).exists():
        print("country-generator not found, cross-check skipped")
        return
    mine = {c[0]: c[1] for c in countries}
    bad = []
    for row in csv.DictReader(open(path, encoding="utf-8"), delimiter="\t"):
        cc, root, suffix = row["cca2"], row["idd_root"].lstrip("+"), row["idd_suffix"]
        code = mine.get(cc)
        if code is None:
            bad.append((cc, "missing"))
        elif not code.startswith(root):
            bad.append((cc, f"{code} vs +{root}"))
        elif code != root and not (root + suffix).startswith(code):
            bad.append((cc, f"{code} vs +{root}{suffix}"))
    print("cross-check against country-generator:", "ok" if not bad else bad)


# ---- regular expression to NFA ---------------------------------------------

class Nfa:
    def __init__(self):
        self.eps = []    # state -> list of states
        self.edges = []  # state -> list of (frozenset digits, state)

    def new(self):
        self.eps.append([])
        self.edges.append([])
        return len(self.eps) - 1


class Parser:
    """Parses the subset libphonenumber uses: literals, classes, \\d, groups,
    alternation, ?, {n} and {n,m}."""

    def __init__(self, text, nfa):
        self.s = re.sub(r"\s+", "", text)
        self.i = 0
        self.nfa = nfa

    def peek(self):
        return self.s[self.i] if self.i < len(self.s) else ""

    def alt(self):
        start, end = self.nfa.new(), self.nfa.new()
        while True:
            a, b = self.seq()
            self.nfa.eps[start].append(a)
            self.nfa.eps[b].append(end)
            if self.peek() == "|":
                self.i += 1
                continue
            return start, end

    def seq(self):
        start = cur = self.nfa.new()
        while self.peek() not in ("", "|", ")"):
            a, b = self.item()
            self.nfa.eps[cur].append(a)
            cur = b
        return start, cur

    def atom(self):
        c = self.peek()
        n = self.nfa
        if c == "(":
            self.i += 1
            if self.s.startswith("?:", self.i):
                self.i += 2
            r = self.alt()
            assert self.peek() == ")", self.s
            self.i += 1
            return r
        if c == "[":
            j = self.s.index("]", self.i)
            body = self.s[self.i + 1:j]
            self.i = j + 1
            digs = set()
            k = 0
            while k < len(body):
                if k + 2 < len(body) and body[k + 1] == "-":
                    digs.update(chr(x) for x in range(ord(body[k]), ord(body[k + 2]) + 1))
                    k += 3
                else:
                    digs.add(body[k])
                    k += 1
            digs = frozenset(digs)
        elif c == "\\":
            assert self.s[self.i + 1] == "d", self.s
            self.i += 2
            digs = frozenset(DIGITS)
        elif c in DIGITS and c:
            self.i += 1
            digs = frozenset(c)
        else:
            raise ValueError(f"unsupported regex syntax {c!r} in {self.s[:80]}")
        a, b = n.new(), n.new()
        n.edges[a].append((digs, b))
        return a, b

    def item(self):
        start = self.i
        a, b = self.atom()
        end = self.i
        c = self.peek()
        lo = hi = 1
        if c == "?":
            self.i += 1
            lo, hi = 0, 1
        elif c == "{":
            j = self.s.index("}", self.i)
            body = self.s[self.i + 1:j]
            self.i = j + 1
            if "," in body:
                x, y = body.split(",")
                lo, hi = int(x), int(y)
            else:
                lo = hi = int(body)
        if (lo, hi) == (1, 1):
            return a, b
        # The atom's NFA fragment is not reusable, so re-parse its text per copy.
        text = self.s[start:end]
        return self._repeat(text, lo, hi)

    def _repeat(self, text, lo, hi):
        n = self.nfa
        start = cur = n.new()
        exits = [cur] if lo == 0 else []
        for k in range(hi):
            x, y = Parser(text, n).atom()
            n.eps[cur].append(x)
            cur = y
            if k + 1 >= lo:
                exits.append(cur)
        end = n.new()
        for e in exits:
            n.eps[e].append(end)
        return start, end


def compile_regex(text):
    nfa = Nfa()
    p = Parser(text, nfa)
    start, end = p.alt()
    assert p.i == len(p.s), f"trailing text at {p.i}: {p.s[p.i:p.i + 20]}"
    return Automaton(nfa, start, end)


class Automaton:
    def __init__(self, nfa, start, end):
        self.nfa, self.end = nfa, end
        self.start = self.closure([start])
        self._step = {}
        self._count = {}

    def closure(self, states):
        seen = set(states)
        stack = list(states)
        while stack:
            s = stack.pop()
            for t in self.nfa.eps[s]:
                if t not in seen:
                    seen.add(t)
                    stack.append(t)
        return frozenset(seen)

    def step(self, S, d):
        key = (S, d)
        r = self._step.get(key)
        if r is None:
            nxt = [t for s in S for (digs, t) in self.nfa.edges[s] if d in digs]
            r = self._step[key] = self.closure(nxt) if nxt else frozenset()
        return r

    def count(self, S, rem):
        """Digit strings of length rem accepted from S."""
        if not S:
            return 0
        if rem == 0:
            return 1 if self.end in S else 0
        key = (S, rem)
        r = self._count.get(key)
        if r is None:
            r = self._count[key] = sum(self.count(self.step(S, d), rem - 1) for d in DIGITS)
        return r


# ---- ranges ------------------------------------------------------------------

def parse_lengths(spec):
    out = set()
    for part in re.sub(r"[\[\]\s]", "", spec).split(","):
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        else:
            out.add(int(part))
    return out


def mask_of(sets):
    """'#' a digit; '[2-9]' a class; runs written '#{6}'."""
    toks = []
    for s in sets:
        if len(s) == 10:
            toks.append("#")
        elif len(s) == 1:
            toks.append(next(iter(s)))
        else:
            toks.append("[" + class_text(sorted(s)) + "]")
    out, i = [], 0
    while i < len(toks):
        j = i
        while j < len(toks) and toks[j] == toks[i]:
            j += 1
        out.append(toks[i] if j - i == 1 else f"{toks[i]}{{{j - i}}}")
        i = j
    return "".join(out)


def class_text(digs):
    out, i = [], 0
    while i < len(digs):
        j = i
        while j + 1 < len(digs) and int(digs[j + 1]) == int(digs[j]) + 1:
            j += 1
        out.append(digs[i] if j == i else (digs[i] + digs[j] if j == i + 1 else f"{digs[i]}-{digs[j]}"))
        i = j + 1
    return "".join(out)


def tail_sets(auto, S, rem):
    """Per-position digit sets of the next rem digits when the language from S
    is a product of them, else None."""
    sets, cur, total = [], S, 1
    for pos in range(rem):
        allowed = [d for d in DIGITS if auto.count(auto.step(cur, d), rem - pos - 1) > 0]
        nxt = set()
        for d in allowed:
            nxt |= auto.step(cur, d)
        sets.append(frozenset(allowed))
        total *= len(allowed)
        cur = frozenset(nxt)
    return sets if total == auto.count(S, rem) else None


def walk(auto, length, max_depth, out, stats):
    """Appends (prefix, mask, count) for every range of national length."""
    def go(prefix, S):
        rem = length - len(prefix)
        n = auto.count(S, rem)
        if n == 0:
            return
        sets = tail_sets(auto, S, rem)
        if prefix and (sets is not None or len(prefix) >= max_depth or rem == 0):
            if sets is None:
                stats["approx"] += 1
                sets = [frozenset(DIGITS)] * rem
            out.append((prefix, mask_of(sets), n))
            return
        for d in DIGITS:
            go(prefix + d, auto.step(S, d))

    go("", auto.start)


def merge(rows, length):
    """Merges sibling rows that differ only in the last prefix digit."""
    changed = True
    while changed:
        changed = False
        groups = {}
        for r in rows:
            prefix, mask, n = r
            if len(prefix) > 1:
                groups.setdefault((prefix[:-1], mask), []).append(r)
        drop = set()
        add = []
        for (parent, mask), g in groups.items():
            if len(g) == 10:
                # all ten digits: the prefix shortens and the mask gains a '#'
                drop.update(g)
                add.append((parent, "#" + mask if mask else "#", sum(x[2] for x in g)))
                changed = True
        if changed:
            rows = [r for r in rows if r not in drop] + add
    return rows


# ---- formats -----------------------------------------------------------------

def strip(text):
    return re.sub(r"\s+", "", text or "")


def pick_format(terr, np_, sample):
    default_rule = terr.get("nationalPrefixFormattingRule")
    for nf in terr.iter("numberFormat"):
        leads = nf.findall("leadingDigits")
        if leads and not re.match(strip(leads[-1].text), sample):
            continue
        m = re.fullmatch(strip(nf.get("pattern")), sample)
        if not m:
            continue
        groups = [len(g) for g in m.groups()]
        fmt = nf.find("format").text.strip()
        rule = nf.get("nationalPrefixFormattingRule") or default_rule or "$FG"
        rule = rule.replace("$NP", np_ or "").replace("$FG", "$1")
        national = fmt.replace("$1", rule, 1) if "$1" in fmt else fmt
        intl = nf.find("intlFormat")
        intl_text = "-" if intl is not None and intl.text.strip() == "NA" else (intl.text.strip() if intl is not None else fmt)
        return groups, national, intl_text
    return [len(sample)], "$1", "$1"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", default="build/phones")
    ap.add_argument("--out", default="resources")
    ap.add_argument("--countries", default="../country-generator/resources",
                    help="country-generator resources: the tiers and the populations")
    ap.add_argument("--max-depth", type=int, default=7)
    ap.add_argument("--regex-out", default="")
    args = ap.parse_args()

    cache = Path(args.cache)
    cache.mkdir(parents=True, exist_ok=True)
    xml = cache / f"PhoneNumberMetadata-{VERSION}.xml"
    if not xml.exists():
        print("downloading", URL)
        urllib.request.urlretrieve(URL, xml)
    root = ET.parse(xml).getroot()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    countries, ranges, regex_rows = [], [], []
    formats = {}
    stats = {"approx": 0}
    for terr in root.iter("territory"):
        rid, code = terr.get("id"), terr.get("countryCode")
        if rid == "001" or not code:
            continue
        np_ = terr.get("nationalPrefix") or ""
        countries.append([rid, code, terr.get("internationalPrefix") or "", np_,
                          "1" if terr.get("mainCountryForCode") == "true" else "0"])
        general = terr.find("generalDesc")
        for xml_name, kind in TYPES:
            e = terr.find(xml_name)
            if e is None:
                continue
            pat = e.find("nationalNumberPattern")
            if pat is None or not pat.text:
                continue
            regex_rows.append((rid, kind, strip(pat.text)))
            pl = e.find("possibleLengths")
            lengths = parse_lengths(pl.get("national", "")) if pl is not None else set()
            if not lengths:
                continue
            auto = compile_regex(pat.text)
            # E.164 allows 15 digits in all, calling code included.
            lengths = {n for n in lengths if len(code) + n <= E164_DIGITS}
            example = e.find("exampleNumber")
            typical = len(strip(example.text)) if example is not None and example.text else None
            buckets = {}
            for length in sorted(lengths):
                rows = []
                walk(auto, length, args.max_depth, rows, stats)
                total = sum(n for _, _, n in rows)
                if total:
                    buckets[length] = (merge(rows, length), total)
            if not buckets:
                continue
            if typical not in buckets:
                # no usable example: the length with the most numbers up to the example-less norm
                typical = max(buckets, key=lambda n: buckets[n][1])
            for length, (merged, total) in buckets.items():
                # A length's draws: 1 at the example's length, LENGTH_DECAY for each digit away from it,
                # spread evenly over the length's numbers.
                share = LENGTH_DECAY ** abs(length - typical) * WEIGHT_SCALE / total
                for prefix, mask, n in merged:
                    sample = prefix + re.sub(r"\[(\d)[^\]]*\]|#", lambda m: m.group(1) or "0", re.sub(
                        r"(\[[^\]]+\]|#|\d)\{(\d+)\}", lambda m: m.group(1) * int(m.group(2)), mask))
                    sample = sample[:length].ljust(length, "0")
                    groups, national, intl = pick_format(terr, np_, sample)
                    key = ("-".join(map(str, groups)), national, intl)
                    fid = formats.setdefault(key, len(formats) + 1)
                    ranges.append([rid, kind, prefix, mask, str(length), f"{n * share:.10g}", str(fid)])

    reserved = reserved_rows({(r[0], r[1]): r[2] for r in regex_rows})
    cg = Path(args.countries)

    def write(folder, name, header, rows):
        folder.mkdir(parents=True, exist_ok=True)
        with open(folder / name, "w", encoding="utf-8", newline="\n") as f:
            f.write("\t".join(header) + "\n")
            for r in rows:
                f.write("\t".join(map(str, r)) + "\n")

    for tier in ("lite", "full"):
        tier_file = cg / tier / "countries.tsv"
        if not tier_file.exists():
            print(f"{tier_file} not found, {tier} tier skipped")
            continue
        pops = {r["cca2"]: r["population"] for r in csv.DictReader(open(tier_file, encoding="utf-8"), delimiter="\t")}
        folder = out / tier
        keep = {c[0] for c in countries} & set(pops)
        write(folder, "countries.tsv", ["region", "code", "intl_prefix", "national_prefix", "main", "population"],
              [c + [pops[c[0]]] for c in countries if c[0] in keep])
        write(folder, "ranges.tsv", ["region", "type", "prefix", "mask", "length", "weight", "format"],
              [r for r in ranges if r[0] in keep])
        write(folder, "formats.tsv", ["id", "groups", "national", "international"],
              [[i, *k] for k, i in formats.items()])
        write(folder, "reserved.tsv", ["region", "type", "prefix", "mask", "length", "weight"],
              [r for r in reserved if r[0] in keep])
        print(f"{tier}: {len(keep)} regions, {sum(1 for r in ranges if r[0] in keep)} ranges, "
              f"{sum(1 for r in reserved if r[0] in keep)} reserved; missing from libphonenumber: "
              f"{sorted(set(pops) - keep)}")
    check_country_generator(countries, cg / "lite" / "countries.tsv")
    if args.regex_out:
        p = Path(args.regex_out)
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write("region\ttype\tregex\n")
            for r in regex_rows:
                f.write("\t".join(r) + "\n")
    print(f"{len(countries)} regions, {len(ranges)} ranges, {len(formats)} formats, "
          f"{stats['approx']} approximate (cut at depth {args.max_depth})")


if __name__ == "__main__":
    sys.exit(main())
