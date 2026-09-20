import json
from pathlib import Path
cc = json.loads(Path("compile_commands.json").read_text(encoding="utf-8"))
for e in cc:
    f = e["file"].replace("\\", "/")
    if f.endswith("bmc.cpp") or f.endswith("test_main.cpp"):
        print(e["command"])
        print("---")
