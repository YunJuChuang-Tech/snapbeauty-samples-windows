"""Install the SDK assets and one host-owned makeup style into a new directory."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def prepare(sdk, source, output):
    shutil.copytree(sdk / "Assets", output)
    with tempfile.TemporaryDirectory() as temporary:
        host = Path(temporary) / "host"
        subprocess.run(
            [sys.executable, str(sdk / "tools" / "prepare_host_makeup.py"),
             "--source", str(source), "--out", str(host)],
            check=True,
        )
        for directory in ("decoded", "makeup"):
            shutil.copytree(host / directory, output / directory, dirs_exist_ok=True)

        index = output / "resources.json"
        resources = json.loads(index.read_text(encoding="utf-8"))
        host_resources = json.loads((host / index.name).read_text(encoding="utf-8"))
        overlap = resources["images"].keys() & host_resources["images"].keys()
        if overlap:
            raise ValueError(f"Makeup entries already present in SDK: {sorted(overlap)}")
        resources["images"].update(host_resources["images"])
        index.write_text(json.dumps(resources, indent=2) + "\n", encoding="utf-8")

        manifest = output / "asset-manifest.json"
        engine_manifest = json.loads(manifest.read_text(encoding="utf-8"))
        host_manifest = json.loads((host / manifest.name).read_text(encoding="utf-8"))
        index_bytes = index.read_bytes()
        entry = next(item for item in engine_manifest["files"] if item["path"] == index.name)
        entry.update(bytes=len(index_bytes), sha256=hashlib.sha256(index_bytes).hexdigest())
        engine_manifest["files"].extend(
            item for item in host_manifest["files"] if item["path"].startswith(("decoded/", "makeup/"))
        )
        manifest.write_text(json.dumps(engine_manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    prepare(args.sdk.resolve(), args.source.resolve(), args.output.resolve())
