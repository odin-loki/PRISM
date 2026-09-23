"""Offline analytics for the PRISM AI layer (roadmap 9.1 / 9.3 / 9.7).

* gbdt.py     tiny pure-Python gradient-boosted trees (depth-3 regression trees)
* predict.py  solver / unwind prediction: train on PRISM's own logs, measure on
              a held-out split, export the JSON model src/prism/solver/predict.cpp loads
* measure.py  per-feature metrics for docs/AI.md (triage, ask, regress, draft)

Nothing here runs inside the pipeline or changes a verdict.
"""
