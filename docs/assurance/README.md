# PRISM assurance package

Roadmap 6.5. These documents map the evidence that exists in this
repository to four frameworks a defence or aerospace buyer is likely to use:

| document | framework |
|---|---|
| [EVIDENCE.md](EVIDENCE.md) | the evidence catalogue: every artefact the other documents cite, what it shows and what it does not |
| [ASSURANCE_CASE.md](ASSURANCE_CASE.md) | a GSN-style assurance case skeleton whose leaves are the catalogue entries |
| [DO-333.md](DO-333.md) | RTCA DO-333, the formal methods supplement to DO-178C |
| [DO-330.md](DO-330.md) | RTCA DO-330, tool qualification considerations for PRISM itself |
| [DEF-STAN-00-055.md](DEF-STAN-00-055.md) | UK Defence Standard 00-055 (safety of programmable elements) |
| [ISM.md](ISM.md) | the Australian Government Information Security Manual (software development controls) |

## What this package is not

- **It is not a qualification, a certification or a compliance statement.**
  No certification authority, designated engineering representative,
  independent safety auditor or IRAP assessor has reviewed PRISM or these
  documents. PRISM holds no DO-330 tool qualification at any TQL, no
  Def Stan 00-055 acceptance and no ISM assessment.
- **It contains no copy of the standards.** RTCA DO-178C/DO-330/DO-333 and
  Def Stan 00-055 are not in this repository. Objective titles and section
  numbers are given as commonly cited and must be checked against the
  licensed text; ISM control identifiers must be checked against the ISM
  release you are assessed against (it is revised several times a year).
- **It does not add evidence.** Every row points at an artefact that
  already exists in the repository, or says "not addressed".

## Rules the documents follow

1. Every claim cites a concrete artefact: a file path, a test
   (`<path>::<name>`), a Lean theorem (`thm:<Name>`), a workflow or a doc
   section (`docs/<X>.md#<anchor>`).
2. `tools/assurance_check.py` verifies that every cited path exists, every
   `<path>::<name>` is found in its file, every anchor resolves and every
   `thm:<Name>` is declared as a `theorem` or `lemma` in the Lean sources
   under `proofs/`.
   `.github/workflows/docs.yml` runs it on every push, so a renamed theorem
   or a deleted test breaks CI instead of leaving an unsupported claim.
3. A Lean theorem proves a statement about a **model** (the verdict lattice,
   the PIR semantics and an encoder of the same design as PRISM's, the
   verification algorithms). It is never cited as a proof about the C++
   code in `src/prism/`; the documents say what connects the two, and where
   nothing does.
4. Rows that PRISM does not address say "not addressed" and why.

The check script does not judge whether an artefact supports the claim made
about it. That is a review task, and the "does not show" column of
[EVIDENCE.md](EVIDENCE.md) exists to make it easier.
