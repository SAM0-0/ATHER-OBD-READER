# Community battery data

`docs/data.html` renders owner-submitted Ather battery readings from
`docs/data/community-scooters.json`. Nothing is fetched at runtime except that
one JSON file, so the site stays static and there is no third-party script.

Current state: **0 submissions.** The page shows its empty state until the first
row lands.

---

## 1. Create the form

Google Forms → new form. Six questions, all short:

| # | Question | Type | Required | Settings |
|---|---|---|---|---|
| 1 | Name (leave blank to stay anonymous) | Short answer | no | — |
| 2 | Which Ather model? | Multiple choice | yes | `450X`, `450S`, `450 Plus`, `450 Apex`, `Rizta Z`, `Rizta S`, `Rizta X`, `Not sure` |
| 3 | Odometer (km) | Short answer | yes | Response validation → number → between 0 and 300000 |
| 4 | Scooter age (months since purchase) | Short answer | yes | number → between 0 and 240 |
| 5 | Battery SOH (%) | Short answer | yes | number → between 0 and 100 |
| 6 | Cell imbalance (value shown in A-OBD) | Short answer | yes | number → between 0 and 5 |
| 7 | Anything else? (optional) | Paragraph | no | — |

Form settings:

- **Responses → Collect email addresses: OFF.** This is what makes the promise
  on the page ("no email address") true.
- **Limit to 1 response: OFF.** Owners resubmitting a year later is useful data.
- **File upload: OFF** (it forces a Google sign-in).
- Confirmation message: *"Thanks — your numbers will appear on
  a-obd.in/data.html within a few days."*

Then **Send → link** and paste that link into `data.html`:

```js
var FORM_URL = "https://forms.gle/xxxxxxxx";
```

Until you do, the button falls back to a prefilled `mailto:` so the page is
never dead.

---

## 2. Connect the responses sheet

Responses land in a Google Sheet automatically. Publish it as CSV:

**Sheet → File → Share → Publish to web → (the responses sheet) → Comma-separated
values (.csv) → Publish.**

Copy that URL. It is read-only, contains no form structure and cannot be edited
from outside — do **not** publish the whole spreadsheet, only that sheet.

> If any respondent puts their phone number or VIN in the free-text box, delete
> that cell before importing. The JSON is public.

---

## 3. Import

```powershell
python tools/sheet_to_json.py --csv-url "https://docs.google.com/spreadsheets/d/e/.../pub?output=csv"
```

or, with a downloaded file:

```powershell
python tools/sheet_to_json.py --csv responses.csv
```

The script:

- matches columns by keyword, so reordering the form questions does not break it
- converts `age (years)` headers to months if you change the question later
- normalises model names (`450 X`, `450x`, `450X` → `450X`)
- treats blank / "anonymous" names as anonymous
- drops out-of-range rows and exact duplicates, and tells you which line and why
- sorts newest first and writes `docs/data/community-scooters.json`

Then commit and push. GitHub Pages redeploys in about a minute.

---

## 4. Cadence

Weekly is plenty. The page says "last updated" from the JSON's `generated`
field, so it never claims to be fresher than it is.

---

## 5. Why not fetch the sheet live from the browser

Google's published CSV endpoint does not send CORS headers, so a browser on
`a-obd.in` cannot read it. The alternatives were:

1. **Apps Script web app** returning JSON — works, but adds a runtime dependency
   on Google for a page that currently has none, and Apps Script is slow and
   rate-limited.
2. **Commit the JSON** — no runtime dependency, fast, versioned in git, and the
   data is reviewable in a diff before it goes public.

Option 2 wins at this volume. If submissions ever reach hundreds and weekly
imports get annoying, swap in Apps Script behind the same JSON shape — the page
would not change.
