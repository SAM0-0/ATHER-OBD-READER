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

    entries, skipped, seen = [], [], set()

    for n, row in enumerate(rows[1:], start=2):
        def get(field):
            idx = mapping.get(field)
            return row[idx] if idx is not None and idx < len(row) else None

        soh = to_float(get("soh"))
        soc = to_float(get("soc"))
        odo = to_int(get("odometerKm"))
        imbalance_mv = to_float(get("imbalance"))
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

        key = (model, odo, round(soh, 2), imbalance_mv)
        if key in seen:
            skipped.append((n, "duplicate row"))
            continue
        seen.add(key)

        name = (get("name") or "").strip()
        if norm(name) in ("", "anonymous", "-", "n/a", "na"):
            name = None

        entries.append({
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
        })

    entries.sort(key=lambda e: e["submitted"], reverse=True)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated": date.today().isoformat(),
        "source": "Google Form submissions, self-reported by Ather owners",
        "count": len(entries),
        "entries": entries,
    }
    OUT.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"wrote {len(entries)} entries to {OUT}")
    for e in entries:
        print(f"  {e['submitted']}  {e['name'] or 'Anonymous':<10} {str(e['model']):<9} "
              f"{str(e['odometerKm']):>7} km  {str(e['ageMonths']):>3} mo  SOH {e['soh']}  "
              f"delta {e['imbalanceMv']} mV  SOC {e['soc']}")
    if skipped:
        print(f"skipped {len(skipped)} row(s):")
        for n, why in skipped[:20]:
            print(f"  line {n}: {why}")


if __name__ == "__main__":
    main()
