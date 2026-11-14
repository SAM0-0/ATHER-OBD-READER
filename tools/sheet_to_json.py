#!/usr/bin/env python3
"""
Turn the Google Form responses sheet into docs/data/community-scooters.json.

Two ways to use it:

  1. Live sheet (works today, no API key, no login):
     python tools/sheet_to_json.py --csv-url "https://docs.google.com/spreadsheets/d/<id>/gviz/tq?tqx=out:csv"

  2. A CSV you downloaded:
     python tools/sheet_to_json.py --csv responses.csv

Then review the diff and commit. The page reads the JSON directly.

Field handling worth knowing:
  * Scooter Age is free text on the form. "4 Years and 2 Months" -> 50 months,
    "38 months" -> 38, a bare number is read as months.
  * Cell Delta is typed with its unit ("38.5 MV") and stored as imbalanceMv.
  * The model answer carries the pack ("Ather 450X (Gen 3 / 3.7 kWh)"), so the
    pack size is pulled out into its own field.
"""

import argparse
import csv
import html
import io
import json
import re
import sys
import urllib.request
from datetime import date, datetime
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "docs" / "data" / "community-scooters.json"

# header keyword -> canonical field. Order matters: first match wins.
FIELDS = [
    ("name", ("name", "handle")),
    ("model", ("model", "scooter")),
    ("odometerKm", ("odometer", "km", "kilomet")),
    ("age", ("age", "month", "year")),
    ("soh", ("soh", "health")),
    ("soc", ("soc", "state of charge")),
    ("imbalance", ("imbalanc", "imblance", "cell delta", "delta", "balanc")),
    ("notes", ("note", "comment", "anything")),
    ("submitted", ("timestamp", "date", "submitted")),
]


def norm(value):
    return re.sub(r"\s+", " ", (value or "").strip().lower())


def to_float(value):
    if value is None:
        return None
    s = re.sub(r"[^0-9.\-]", "", str(value).replace(",", "."))
    if not s or s in ("-", "."):
        return None
    try:
        return float(s)
    except ValueError:
        return None


def to_int(value):
    f = to_float(value)
    return None if f is None else int(round(f))


def parse_age_months(value, header=""):
    """Free-text age -> months. '4 Years and 2 Months' = 50 months."""
    s = norm(value)
    if not s:
        return None
    years = re.search(r"(\d+(?:\.\d+)?)\s*(?:year|yr)", s)
    months = re.search(r"(\d+(?:\.\d+)?)\s*(?:month|mo\b)", s)
    if years or months:
        total = 0.0
        if years:
            total += float(years.group(1)) * 12
        if months:
            total += float(months.group(1))
        return int(round(total))
    bare = to_int(value)
    if bare is None:
        return None
    # A number with no unit: honour the column header if it says years.
    return bare * 12 if "year" in norm(header) else bare


def normalise_delta(value):
    """Owners type either millivolts ('38.5') or volts ('0.0013').

    A cell delta below 1 is always volts - nobody reports 0.4 mV on a 14S pack,
    and 0.0013 V is 1.3 mV, which is a real reading. So anything in (0, 1) is
    multiplied by 1000.
    """
    if value is None:
        return None
    return value * 1000 if 0 < value < 1 else value


def parse_pack(*values):
    """Pull '3.7' out of 'Ather 450X (Gen 3 / 3.7 kWh)'."""
    for value in values:
        m = re.search(r"(\d(?:\.\d)?)\s*kwh", norm(value))
        if m:
            size = float(m.group(1))
            if 1.5 <= size <= 6:
                return size
    return None


def map_headers(header_row):
    mapping = {}
    for idx, raw in enumerate(header_row):
        h = norm(raw)
        if not h:
            continue
        for field, keys in FIELDS:
            if field in mapping:
                continue
            if any(k in h for k in keys):
                mapping[field] = idx
                break
    return mapping


def parse_date(value):
    value = (value or "").strip()
    if not value:
        return None
    for fmt in ("%Y/%m/%d %H:%M:%S", "%Y-%m-%d %H:%M:%S", "%d/%m/%Y %H:%M:%S",
                "%Y-%m-%d", "%d/%m/%Y", "%d-%m-%Y", "%m/%d/%Y %H:%M:%S"):
        try:
            return datetime.strptime(value, fmt).date().isoformat()
        except ValueError:
            continue
    m = re.match(r"(\d{4})-(\d{2})-(\d{2})", value)
    return m.group(0) if m else None


def clean_model(value):
    s = norm(value)
    if not s:
        return None
    if "rizta" in s:
        for tag, name in ((" z", "Rizta Z"), (" s", "Rizta S"), (" x", "Rizta X")):
            if tag in s + " ":
                return name
        return "Rizta"
    if "apex" in s:
        return "450 Apex"
    if "plus" in s:
        return "450 Plus"
    if "450x" in s or "450 x" in s:
        return "450X"
    if "450s" in s or "450 s" in s:
        return "450S"
    if "not sure" in s or "don" in s:
        return None
    return value.strip()[:40]


def load_rows(args):
    if args.csv_url:
        req = urllib.request.Request(args.csv_url, headers={"User-Agent": "a-obd-data-import"})
        with urllib.request.urlopen(req, timeout=30) as r:
            text = r.read().decode("utf-8-sig")
        return list(csv.reader(io.StringIO(text)))
    path = Path(args.csv)
    text = path.read_text(encoding="utf-8-sig")
    return list(csv.reader(io.StringIO(text)))


HTML = OUT.parent.parent / "data.html"


def inr(n):
    """Indian digit grouping, to match the JS table (20,660 / 3,00,000)."""
    digits = str(abs(int(n)))
    if len(digits) > 3:
        head, tail = digits[:-3], digits[-3:]
        parts = []
        while len(head) > 2:
            parts.insert(0, head[-2:])
            head = head[:-2]
        digits = ",".join([head] + parts + [tail])
    return ("-" if n < 0 else "") + digits


def median(values):
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    mid = len(v) // 2
    return v[mid] if len(v) % 2 else (v[mid - 1] + v[mid]) / 2


def model_text(e):
    if not e.get("model"):
        return "\u2014"
    return e["model"] + (" \u00b7 " + str(e["pack"]) + " kWh" if e.get("pack") else "")


def summary_html(entries):
    if not entries:
        return ("<strong>No submissions yet.</strong> The charts appear once the first reading lands &mdash; "
                "add yours with the form below and it stops being theoretical.")
    soh = median([e.get("soh") for e in entries])
    delta = median([e.get("imbalanceMv") for e in entries])
    below = sum(1 for e in entries if (e.get("soh") or 0) < 70)
    n = len(entries)
    parts = [f"So far <strong>{n} {'owner has' if n == 1 else 'owners have'} submitted</strong> a reading"]
    tail = []
    if soh is not None:
        tail.append(f"median SOH <strong>{soh:.1f}%</strong>")
    if delta is not None:
        tail.append(f"median cell delta <strong>{delta:.1f} mV</strong>")
    text = parts[0] + (": " + " and ".join(tail) if tail else "") + "."
    if below:
        text += f" {below} {'reading sits' if below == 1 else 'readings sit'} below the 70% warranty threshold."
    return text


def rows_html(entries):
    rows = []
    ordered = sorted(entries, key=lambda e: (e.get("odometerKm") or 0), reverse=True)
    for e in ordered:
        if e.get("ageMonths") is None:
            age = "\u2014"
        elif e["ageMonths"] < 12:
            age = f"{round(e['ageMonths'])} mo"
        else:
            age = f"{round(e['ageMonths'] / 12, 1):g} yr"
        odo = inr(e["odometerKm"]) + " km" if e.get("odometerKm") is not None else "\u2014"
        soh = f"{e['soh']:.1f}%" if e.get("soh") is not None else "\u2014"
        delta = f"{e['imbalanceMv']:.1f} mV" if e.get("imbalanceMv") is not None else "\u2014"
        soc = f"{e['soc']:.1f}%" if e.get("soc") is not None else "\u2014"
        name = e.get("name")
        first = ("<td>" + html.escape(name) + "</td>") if name else ('<td style="color:#9aa8ba">Anonymous</td>')
        cells = [model_text(e), age, odo, soh, delta, soc]
        rows.append("              <tr>" + first + "".join("<td>" + html.escape(c) + "</td>" for c in cells) + "</tr>")
    return "\n".join(rows)


def write_html(entries):
    """Put the numbers in the HTML itself, so crawlers and no-JS readers see them."""
    if not HTML.exists():
        print("  (data.html not built yet - run the page builder first)")
        return
    page = HTML.read_text(encoding="utf-8")
    replaced = 0
    for start, end, value in (
        ("<!--DATA:SUMMARY:START-->", "<!--DATA:SUMMARY:END-->", summary_html(entries)),
        ("<!--DATA:ROWS:START-->", "<!--DATA:ROWS:END-->", rows_html(entries)),
    ):
        i = page.find(start)
        j = page.find(end)
        if i == -1 or j == -1:
            print(f"  (markers for {start} missing - skipped)")
            continue
        page = page[: i + len(start)] + value + page[j:]
        replaced += 1
    HTML.write_text(page, encoding="utf-8", newline="")
    print(f"  embedded {replaced}/2 blocks into docs/data.html")



OVERRIDES = Path(__file__).resolve().parent / "overrides.json"


def load_overrides():
    """Per-owner corrections that must survive a re-import.

    The Sheet is the source of truth, but owners sometimes clear a cell after
    submitting (or the form mangles a value). Rather than hand-editing the
    generated JSON - which the next import would wipe - real readings that the
    owner has confirmed go here, keyed on name + odometer.
    """
    if not OVERRIDES.exists():
        return []
    try:
        data = json.loads(OVERRIDES.read_text(encoding="utf-8"))
    except Exception as exc:  # a broken file must not silently drop data
        sys.exit(f"could not read {OVERRIDES.name}: {exc}")
    return data.get("entries", [])


def apply_overrides(entries, overrides):
    applied = []
    for rule in overrides:
        match = [
            e for e in entries
            if (norm(e["name"]) or None) == (norm(rule.get("name")) or None)
            and e["odometerKm"] == rule.get("odometerKm")
        ]
        if not match:
            applied.append(f"{rule.get('name')} @ {rule.get('odometerKm')} km: no matching row")
            continue
        for field, value in rule.items():
            if field in ("name", "odometerKm", "reason"):
                continue
            match[0][field] = value
        applied.append(f"{rule.get('name')}: {', '.join(k for k in rule if k not in ('name', 'odometerKm', 'reason'))}"
                       + (f" ({rule['reason']})" if rule.get("reason") else ""))
    return applied


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--csv-url", help="Google Sheets CSV URL (gviz or published)")
    src.add_argument("--csv", help="path to a downloaded CSV")
    args = ap.parse_args()

    rows = load_rows(args)
    if not rows:
        sys.exit("empty sheet")

    header = rows[0]
    mapping = map_headers(header)
    missing = [f for f in ("model", "odometerKm", "soh") if f not in mapping]
    if missing:
        sys.exit("could not find columns for: " + ", ".join(missing) + "\nheaders seen: " + str(header))

    age_header = header[mapping["age"]] if "age" in mapping else ""

    entries, skipped, seen = [], [], {}

    for n, row in enumerate(rows[1:], start=2):
        def get(field):
            idx = mapping.get(field)
            return row[idx] if idx is not None and idx < len(row) else None

        soh = to_float(get("soh"))
        soc = to_float(get("soc"))
        odo = to_int(get("odometerKm"))
        imbalance_mv = normalise_delta(to_float(get("imbalance")))
        age_months = parse_age_months(get("age"), age_header)
        model = clean_model(get("model"))
        pack = parse_pack(get("model"), get("pack"))

        if soh is None or not (0 <= soh <= 100):
            skipped.append((n, "soh missing or out of range"))
            continue
        if odo is not None and not (0 <= odo <= 300000):
            skipped.append((n, "odometer out of range"))
            continue
        if age_months is not None and not (0 <= age_months <= 240):
            skipped.append((n, "age out of range"))
            continue
        if soc is not None and not (0 <= soc <= 100):
            skipped.append((n, "soc out of range"))
            continue
        if imbalance_mv is not None and not (0 <= imbalance_mv <= 1000):
            skipped.append((n, "imbalance out of range (expected mV, 0-1000)"))
            continue

        # The same owner submitting twice often disagrees with themselves about
        # "Gen 4" vs "Gen 5" or leaves one cell blank, so dedupe on the reading
        # itself: same name, same odometer, same SOH is one scooter. Fill gaps
        # from the later row; if the two rows contradict each other, drop the
        # disputed field rather than pick a side.
        key = (norm(get("name")) or None, odo, round(soh, 2))
        if key in seen:
            first = seen[key]
            merged = []
            for field, value in (
                ("pack", pack),
                ("imbalanceMv", None if imbalance_mv is None else round(imbalance_mv, 1)),
                ("soc", None if soc is None else round(soc, 1)),
                ("ageMonths", age_months),
            ):
                if value is None:
                    continue
                if first[field] is None:
                    first[field] = value
                    merged.append(field)
                elif first[field] != value:
                    first[field] = None
                    merged.append(field + " conflict -> dropped")
            skipped.append((n, "merged into line %d (%s)" % (first["_line"], ", ".join(merged) or "no new data")))
            continue

        name = (get("name") or "").strip()
        if norm(name) in ("", "anonymous", "unknown", "anon", "-", "n/a", "na"):
            name = None

        entry = {
            "_line": n,
            "name": name,
            "model": model,
            "pack": pack,
            "odometerKm": odo,
            "ageMonths": age_months,
            "soh": round(soh, 1),
            "imbalanceMv": None if imbalance_mv is None else round(imbalance_mv, 1),
            "soc": None if soc is None else round(soc, 1),
            "notes": (get("notes") or "").strip()[:280] or None,
            "submitted": parse_date(get("submitted")) or date.today().isoformat(),
        }
        entries.append(entry)
        seen[key] = entry

    overrides = apply_overrides(entries, load_overrides())

    entries.sort(key=lambda e: (e["submitted"], e["odometerKm"] or 0), reverse=True)
    for e in entries:
        e.pop("_line", None)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated": date.today().isoformat(),
        "source": "Google Form submissions, self-reported by Ather owners",
        "count": len(entries),
        "entries": entries,
    }
    OUT.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"wrote {len(entries)} entries to {OUT}")
    write_html(entries)
    for e in entries:
        print(f"  {e['submitted']}  {e['name'] or 'Anonymous':<10} {str(e['model']):<9} "
              f"{str(e['odometerKm']):>7} km  {str(e['ageMonths']):>3} mo  SOH {e['soh']}  "
              f"delta {e['imbalanceMv']} mV  SOC {e['soc']}")
    if overrides:
        print(f"applied {len(overrides)} override(s):")
        for line in overrides:
            print(f"  {line}")
    if skipped:
        print(f"skipped {len(skipped)} row(s):")
        for n, why in skipped[:20]:
            print(f"  line {n}: {why}")


if __name__ == "__main__":
    main()
