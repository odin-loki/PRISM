#!/usr/bin/env python3
"""CycloneDX 1.5 SBOM from third_party/MANIFEST.toml (roadmap Part 1.3).

Standard library only. Deterministic: no timestamp unless SOURCE_DATE_EPOCH
is set (then metadata.timestamp is that instant), and the serial number is a
UUIDv5 of the manifest bytes, so the same manifest gives the same file.

  python scripts/sbom.py [--manifest PATH] [--version V] [-o prism.cdx.json]

linked   -> type "library", scope "required"  (compiled into PRISM)
external -> type "application", scope "optional" (separate process, pinned)
system   -> type "application", scope "optional" (host tool, not pinned)
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import sys
import tomllib
import uuid
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "third_party" / "MANIFEST.toml"
_NS = uuid.UUID("5b0e7c52-3a4f-4c33-9d53-7072736d7362")  # fixed namespace for PRISM SBOMs


def _purl(c: dict[str, Any]) -> str | None:
    url = str(c.get("url") or "")
    if url.startswith("https://github.com/") and c.get("commit"):
        owner_repo = url.removeprefix("https://github.com/").strip("/")
        if owner_repo.count("/") == 1:
            owner, repo = owner_repo.split("/")
            return f"pkg:github/{owner.lower()}/{repo.lower()}@{c['commit']}"
    return None


def _licenses(spdx: str) -> list[dict[str, Any]]:
    if not spdx or spdx == "NOASSERTION":
        return []
    if any(op in spdx for op in (" AND ", " OR ", " WITH ")):
        return [{"expression": spdx}]
    return [{"license": {"id": spdx}}]


def component(c: dict[str, Any]) -> dict[str, Any]:
    kind = c["kind"]
    out: dict[str, Any] = {
        "type": "library" if kind == "linked" else "application",
        "bom-ref": f"{kind}:{c['name']}",
        "name": c["name"],
        "version": str(c.get("version") or ""),
        "scope": "required" if kind == "linked" else "optional",
    }
    lic = _licenses(str(c.get("spdx") or ""))
    if lic:
        out["licenses"] = lic
    purl = _purl(c)
    if purl:
        out["purl"] = purl
    if c.get("archive_sha256"):
        out["hashes"] = [{"alg": "SHA-256", "content": c["archive_sha256"]}]
    refs = [{"type": "vcs", "url": c["url"]}] if c.get("url") else []
    if refs:
        out["externalReferences"] = refs
    props = [{"name": "prism:kind", "value": kind}]
    if c.get("commit"):
        props.append({"name": "prism:commit", "value": c["commit"]})
    if c.get("tag"):
        props.append({"name": "prism:tag", "value": c["tag"]})
    if c.get("archive_sha256"):
        props.append({"name": "prism:archive_format", "value": "git-archive-tar"})
    if c.get("tree_sha256"):
        props.append({"name": "prism:tree_sha256", "value": c["tree_sha256"]})
    if c.get("path"):
        props.append({"name": "prism:path", "value": c["path"]})
    if c.get("drift"):
        props.append({"name": "prism:drift", "value": "; ".join(c["drift"])})
    if c.get("licence_note"):
        props.append({"name": "prism:licence_note", "value": c["licence_note"]})
    out["properties"] = props
    return out


def build(manifest_bytes: bytes, prism_version: str) -> dict[str, Any]:
    data = tomllib.loads(manifest_bytes.decode("utf-8"))
    comps = [component(c) for c in data.get("component") or []]
    meta: dict[str, Any] = {
        "tools": {"components": [{"type": "application", "name": "prism-sbom",
                                  "version": "1"}]},
        "component": {
            "type": "application", "bom-ref": "prism", "name": "prism",
            "version": prism_version,
            "licenses": [{"license": {"name": "PRISM Source-Available Evaluation Licence (DRAFT)"}}],
        },
        "properties": [{"name": "prism:manifest_sha256",
                        "value": hashlib.sha256(manifest_bytes).hexdigest()}],
    }
    sde = os.environ.get("SOURCE_DATE_EPOCH")
    if sde and sde.isdigit():
        meta["timestamp"] = _dt.datetime.fromtimestamp(int(sde), _dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    linked = [c["bom-ref"] for c in comps if c["scope"] == "required"]
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid5(_NS, hashlib.sha256(manifest_bytes).hexdigest() + prism_version)}",
        "version": 1,
        "metadata": meta,
        "components": comps,
        "dependencies": [{"ref": "prism", "dependsOn": linked}],
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="CycloneDX 1.5 SBOM from third_party/MANIFEST.toml")
    ap.add_argument("--manifest", type=Path, default=MANIFEST)
    ap.add_argument("--version", default=os.environ.get("PRISM_VERSION", "0.0.0-dev"))
    ap.add_argument("-o", "--output", type=Path, default=None)
    args = ap.parse_args(argv)
    bom = build(args.manifest.read_bytes(), args.version)
    text = json.dumps(bom, indent=2, sort_keys=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
        print(f"wrote {args.output} ({len(bom['components'])} components)")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
