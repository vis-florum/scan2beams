#!/usr/bin/env python3
"""Recreate exact crops from boxes.json without repeating segmentation."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", "-o", required=True, type=Path)
    parser.add_argument("--source", type=Path, help="Override source path after moving a scan")
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "build/scan_separator")
    args = parser.parse_args()
    data = json.loads(args.manifest.read_text())
    if data.get("schema_version") != 1:
        parser.error("Unsupported boxes.json schema")
    source = args.source or Path(data["source"])
    if args.source is None and not source.is_absolute():
        source = args.manifest.resolve().parent / source
    if source.stat().st_size != data["source_file_bytes"]:
        parser.error("Source file size differs from the recorded scan")
    # Also validate shape/type; equal byte lengths can describe different arrays.
    with source.open("rb") as stream:
        if not stream.readline().startswith(b"NRRD"):
            parser.error("Source is not NRRD")
        fields = {}
        for line in stream:
            if not line.strip():
                break
            if not line.startswith(b"#") and b":=" not in line and b":" in line:
                key, value = line.decode("ascii").split(":", 1)
                fields[key.strip().lower()] = value.strip()
    if list(map(int, fields.get("sizes", "").split())) != data["source_sizes"]:
        parser.error("Source dimensions differ from recorded scan")
    original_fields = {}
    for line in data["original_header"].splitlines():
        if ":" in line and ":=" not in line and not line.startswith("#"):
            key, value = line.split(":", 1)
            original_fields[key.strip().lower()] = value.strip()
    if fields != original_fields:
        parser.error("Source header differs from recorded scan")
    cmd = [str(args.binary.resolve()), str(source), "--output", str(args.output)]
    labels = []
    for obj in data["objects"]:
        cmd.extend(["--box", ",".join(map(str, obj["min"] + obj["max"]))])
        name = obj["file"]
        if not name.endswith(".nrrd"):
            parser.error("Crop filename must end in .nrrd")
        labels.append(name[:-5])
    if not labels:
        parser.error("Manifest has no objects")
    # The CLI uses commas as a delimiter, so reject ambiguity.
    if any("," in label for label in labels):
        parser.error("Labels containing commas cannot be replayed")
    cmd.extend(["--labels", ",".join(labels)])
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
