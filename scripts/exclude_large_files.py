"""Find files >10MB and append relative paths to .gitignore."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 10 * 1024 * 1024
SKIP_PARTS = {
    ".git",
    "build",
    "build_wsl",
    "build_cxx",
    "__pycache__",
    ".venv",
    "venv",
}


def should_skip(p: Path) -> bool:
    parts = set(p.relative_to(ROOT).parts)
    if ".git" in parts:
        return True
    rel = p.relative_to(ROOT).as_posix()
    for prefix in ("build/", "build_wsl/", "build_cxx/", "native/build/", "__pycache__/"):
        if rel.startswith(prefix) or f"/{prefix}" in f"/{rel}":
            return True
    return False


def main() -> None:
    large: list[tuple[int, str]] = []
    for p in ROOT.rglob("*"):
        if not p.is_file():
            continue
        if should_skip(p):
            continue
        try:
            sz = p.stat().st_size
        except OSError:
            continue
        if sz > LIMIT:
            large.append((sz, p.relative_to(ROOT).as_posix()))

    large.sort(reverse=True)
    print(f"found {len(large)} files >10MB")
    for sz, rel in large[:50]:
        print(f"  {sz/1024/1024:8.1f} MB  {rel}")
    if len(large) > 50:
        print(f"  ... and {len(large)-50} more")

    gi = ROOT / ".gitignore"
    text = gi.read_text(encoding="utf-8")
    marker = "\n# Auto-excluded files larger than 10MB\n"
    if marker in text:
        text = text.split(marker)[0].rstrip() + "\n"

    lines = [marker.lstrip("\n")]
    lines.append("# Generated — do not hand-edit the list below\n")
    for _, rel in large:
        # gitignore: escape nothing special for normal paths; use forward slashes
        lines.append(rel + "\n")
    gi.write_text(text.rstrip() + "\n\n" + "".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {len(large)} entries into .gitignore")
    list_path = ROOT / "scripts" / "large_files_excluded.txt"
    list_path.write_text(
        "\n".join(f"{sz}\t{rel}" for sz, rel in large) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print("list:", list_path)


if __name__ == "__main__":
    main()
