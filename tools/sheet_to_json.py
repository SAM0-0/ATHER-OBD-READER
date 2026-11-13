#!/usr/bin/env python3
"""
Turn the Google Form responses sheet into docs/data/community-scooters.json.

Two ways to use it:

  1. Published CSV (no API key, no login):
     File -> Share -> Publish to web -> select the responses sheet -> CSV
     python tools/sheet_to_json.py --csv-url "https://docs.google.com/spreadsheets/d/e/.../pub?output=csv"

  2. A CSV you downloaded:
     python tools/sheet_to_json.py --csv responses.csv

Then review the diff and commit. The page reads the JSON directly.
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
    ("name", ("name",)),
    ("model", ("model", "scooter")),
    ("odometerKm", ("odo", "km", "kilomet")),
    ("age", ("age", "month", "year")),
    ("soh", ("soh", "health")),
    ("imbalance", ("imbalanc", "balanc")),
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
    src.add_argument("--csv-url", help="published Google Sheets CSV URL")
    src.add_argument("--csv", help="path to a downloaded CSV")
    args = ap.parse_args()

    rows = load_rows(args)
    if not rows:
        sys.exit("empty sheet")

    mapping = map_headers(rows[0])
    missing = [f for f in ("model", "odometerKm", "soh") if f not in mapping]
    if missing:
        sys.exit("could not find columns for: " + ", ".join(missing) + "\nheaders seen: " + str(rows[0]))

    entries, skipped, seen = [], [], set()

    for n, row in enumerate(rows[1:], start=2):
        def get(field):
            idx = mapping.get(field)
            return row[idx] if idx is not None and idx < len(row) else None

        soh = to_float(get("soh"))
        odo = to_int(get("odometerKm"))
        imbalance = to_float(get("imbalance"))
        age_raw = to_int(get("age"))

        if age_raw is None:
            age_months = None
        else:
            age_months = age_raw * 12 if "year" in norm(rows[0][mapping["age"]]) else age_raw

        if soh is None or not (0 <= soh <= 100):
            skipped.append((n, "soh missing or out of range"))
            continue
        if odo is not None and not (0 <= odo <= 300000):
            skipped.append((n, "odometer out of range"))
            continue
        if age_months is not None and not (0 <= age_months <= 240):
            skipped.append((n, "age out of range"))
            continue
        if imbalance is not None and not (0 <= imbalance <= 5):
            skipped.append((n, "imbalance out of range"))
            continue

        key = (clean_model(get("model")), odo, round(soh, 2), imbalance)
        if key in seen:
            skipped.append((n, "duplicate row"))
            continue
        seen.add(key)

        name = (get("name") or "").strip()
        if norm(name) in ("", "anonymous", "-", "n/a", "na"):
            name = None

        entries.append({
            "name": name,
            "model": clean_model(get("model")),
            "odometerKm": odo,
            "ageMonths": age_months,
            "soh": round(soh, 1),
            "imbalance": None if imbalance is None else round(imbalance, 3),
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
    if skipped:
        print(f"skipped {len(skipped)} row(s):")
        for n, why in skipped[:20]:
            print(f"  line {n}: {why}")


if __name__ == "__main__":
    main()
