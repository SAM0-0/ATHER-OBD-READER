# Community battery data

`docs/data.html` renders owner-submitted Ather battery readings from
`docs/data/community-scooters.json`. Nothing is fetched at runtime except that
one JSON file, so the site stays static and there is no third-party script.

Live now with the first real submission. `tools/sheet_to_json.py` regenerates
the JSON from the Google Form responses sheet.

---

## 1. The form (already built)

<https://docs.google.com/forms/d/e/1FAIpQLScHCqysfnQ3bhibFqfTHxBx-bJ7zGbs3vp1uxr-HyGAFYlvpQ/viewform>

| Question | Type | Notes |
|---|---|---|
| Name or Community Handle | Short answer | blank / "anonymous" → shown as *Anonymous* |
| Ather Scooter Model | Choice | answer carries the pack, e.g. `Ather 450X (Gen 3 / 3.7 kWh)` |
| Scooter Age | Short answer | **free text** — `4 Years and 2 Months`, `38 months`, or a bare number read as months |
| Odometer Reading (in km) | Short answer | number |
| State of Health - SoH (%) | Short answer | number, required |
| State of Charge - SoC (%) at time of reading | Short answer | number, keeps comparisons honest |
| Cell Delta (mV) / Voltage Imblance | Short answer | in **millivolts**, unit is stripped on import |

Form settings that matter:

- **Collect email addresses: OFF.** This is what makes the page's "no email
  address" promise true.
- **Limit to 1 response: OFF.** An owner resubmitting a year later is useful
  data (it becomes a second point on the same scooter).

## 2. The responses sheet

Responses sheet id: `1Rhd_clf-ZDjO_Y3dDCXs6wxQHehfskhpvqVNrwalLtc`

Readable as CSV without logging in:

```
https://docs.google.com/spreadsheets/d/1Rhd_clf-ZDjO_Y3dDCXs6wxQHehfskhpvqVNrwalLtc/gviz/tq?tqx=out:csv
```

> **Share it as Viewer, never Editor.** That link is public. Anyone with edit
> access can rewrite submissions — including the numbers strangers will compare
> their scooter against. Editor access is also all that stands between a bored
> visitor and deleting the whole sheet.

If the form later moves or the tab name changes, the `gviz` endpoint still works
as long as the first sheet is the responses tab. The `File → Share → Publish to
web → CSV` URL is an equally good alternative; both need no API key.

## 3. Import

```powershell
python tools/sheet_to_json.py --csv-url "https://docs.google.com/spreadsheets/d/1Rhd_clf-ZDjO_Y3dDCXs6wxQHehfskhpvqVNrwalLtc/gviz/tq?tqx=out:csv"
```

or with a downloaded file:

```powershell
python tools/sheet_to_json.py --csv responses.csv
```

The script:

- matches columns by keyword, so reordering the questions does not break it
  (it also copes with the form's "Imblance" typo)
- parses free-text age: `4 Years and 2 Months` → 50 months; a bare number is
  months, or years if the column header says so
- strips units from cell delta: `38.5 MV` → `38.5` stored as `imbalanceMv`
- pulls the pack size out of the model answer: `(Gen 3 / 3.7 kWh)` → `pack: 3.7`
- normalises model names (`450 X`, `450x`, `Ather 450X (Gen 3 / 3.7 kWh)` → `450X`)
- treats blank / "anonymous" names as anonymous
- merges the same owner submitting twice (same name + odometer + SOH); a
  field the two rows disagree on is dropped instead of guessed
- reads `0.0013` as volts when the value is below 1, storing `1.3` mV
- drops out-of-range rows, and says which line and why
- applies `tools/overrides.json` last, so an owner-confirmed reading survives
  a re-import even if the sheet cell was cleared later
- sorts newest first and writes `docs/data/community-scooters.json`

Then review the diff, commit and push. GitHub Pages redeploys in about a minute.

Output shape:

```json
{
  "generated": "2026-11-13",
  "source": "Google Form submissions, self-reported by Ather owners",
  "count": 1,
  "entries": [
    {
      "name": "Sam", "model": "450X", "pack": 3.7,
      "odometerKm": 20660, "ageMonths": 50,
      "soh": 89.0, "imbalanceMv": 38.5, "soc": 67.0,
      "notes": null, "submitted": "2026-11-13"
    }
  ]
}
```

## 4. Cadence

Weekly is plenty. The page shows "last updated" from the JSON's `generated`
field, so it never claims to be fresher than it is.

## 5. Owner-confirmed values (`overrides.json`)

The sheet is the source of truth, but a respondent sometimes clears a cell after
submitting, or the form mangles a value. Editing the generated JSON by hand does
not survive the next import, so confirmed readings live in
`tools/overrides.json`, keyed on name + odometer:

```json
{
  "entries": [
    { "name": "Sam", "odometerKm": 20660, "imbalanceMv": 38.5,
      "reason": "owner confirmed the live cell delta; the sheet cell was cleared" }
  ]
}
```

The importer applies these after merging and prints each one it used, or says
`no matching row` if an owner's odometer changed.

## 6. Before importing, check for personal data

The free-text box is the only place a respondent can leak something identifying
(phone number, VIN, address). The JSON is public — delete that cell in the sheet
before running the importer.

## 7. Why not fetch the sheet live in the browser

Google's CSV endpoint sends no CORS headers, so a browser on `a-obd.in` cannot
read it. The alternatives were:

1. **Apps Script web app** returning JSON — works, but adds a runtime dependency
   on Google for a page that currently has none, and Apps Script is slow and
   rate-limited.
2. **Commit the JSON** — no runtime dependency, fast, versioned in git, and the
   data is reviewable in a diff before it goes public.

Option 2 wins at this volume. If submissions ever reach hundreds and weekly
imports get annoying, swap in Apps Script behind the same JSON shape — the page
would not change.
