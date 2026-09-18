**scan2beams** crops long wooden beams from 16-bit CT NRRDs using sparse 2D thresholding and erosion. Beams are arranged along voxel Y; length is voxel Z. Outputs are raw NRRD crops, original-image bounding boxes in JSON, and a numbered PNG diagnostic, ordered by decreasing Y. By default, detected beam edges are retained with a 3 mm end margin, including when that margin overlaps a holder.

Linux / POSIX, C++17, CMake ≥3.16. No external C++ dependencies. Python 3 is used only for tests, the synthetic example, and optional replay.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The existing local `build/` was moved with the project; its executable is ready to use. Build files and scan payloads are ignored by Git.

```text
src/scan_separator.cpp      CLI, NRRD I/O, detection, crop writer
src/preview.cpp, preview.h  Dependency-free PNG diagnostics
scripts/replay_crops.py      Recreate crops from saved JSON
tests/integration.py        Voxel-exact integration checks
examples/generate.py        Small reproducible four-beam scan
examples/boxes.json         Reference bounds for that scan
.github/workflows/ci.yml     Build and test
```

Run commands from the project root:

```sh
build/scan_separator scan.nrrd -o crops
build/scan_separator scan.nrrd --count 4 --labels A,B,C,D -o named_crops
build/scan_separator scan.nrrd --preview -o preview
python3 scripts/replay_crops.py preview/boxes.json -o crops
```

Without labels, names are `scan_1.nrrd`, `scan_2.nrrd`, etc. Labels are exact filename stems, assigned from highest to lowest voxel Y; they imply the expected count. `--labels-file` accepts comma-separated or newline-separated names. A count mismatch fails before outputs are written. Existing files are refused; use a fresh output directory. One scan per invocation.

The input must be a **raw 3D uint16 or int16 NRRD**, attached or with one detached data file, in either byte order. Decompress gzip inputs first. Crops preserve source values, type, byte order, spacing, directions and custom metadata. Spatial origins shift by the crop offset. Physical parameters use millimetres; absent spacing defaults to 1 mm with a warning.

Generate and process the portable example:

```sh
python3 examples/generate.py
build/scan_separator examples/synthetic.nrrd --count 4 -o examples/synthetic_crops
python3 scripts/replay_crops.py examples/boxes.json -o examples/synthetic_replay_crops
```

The ~7 MB fixture contains four beams, thin end holders, an internal intensity gap, and a streak bridge. `examples/boxes.json` contains reference results with a relative source path. Replay resolves relative source paths against the manifest directory. Use `--source PATH` to override a moved source, or `--binary PATH` for another executable.

The original real scan, four generated crops, validation JSON, and [overlay](examples/boxes_preview.png) are retained locally under `examples/`. The large NRRDs and crop directories are ignored by Git. The real scan is declared `int16`; its crops were compared voxel-for-voxel against the source. Earlier crops made with automatic end trimming are retained in `examples/20260910.152314._K_a__trimmed_crops/` for comparison.

Detection samples nine XY sections to locate Y bands, then three YZ and three XZ planes per beam. Thresholded masks are eroded with a rectangular kernel and fused by majority vote. Short detached components are removed before small Z gaps are closed. The largest component supplies the envelope, including its narrow ends; the outer YZ/XZ length extents are combined, erosion is compensated, and margins are added. Detached holders remain outside the detection mask where possible, but their overlap with a rectangular crop does not shorten the default output. Source pages are memory-mapped and released in chunks; all crops are written in one Z traversal. No 3D masks or resampling.

Common tuning options; see `build/scan_separator --help` for all options:

| Option | Default |
| --- | --- |
| `--threshold` | 200 |
| `--metal-threshold` | disabled; rejects values at/above the cutoff during detection |
| `--erosion-mm` | 1.5 mm radius |
| `--margin-mm` | 5 mm in X/Y, clipped at adjacent Y partitions |
| `--z-margin-mm` | 3 mm outward at both ends |
| `--end-trim-mm` | 0 mm inward at both ends |
| `--trim-end-clutter` | off; opt in to shortening for detached end components |
| `--gap-mm` | 20 mm maximum internal gap |
| `--plate-thickness-mm` | 20 mm maximum detached fragment length to discard |
| `--samples`, `--cross-sections` | 3, 9 |
| `--step`, `--z-step` | 2, 4 analysis sampling strides |

Increase erosion for narrow artefact bridges. Use an upper intensity cutoff only if it remains above the wood range. Use strides of one for finer estimates. Beams need separate Y bands and a shared interior visible in the sampled longitudinal planes. Large gaps, extreme drift, or short fragments may require tuning. Sparse bounds are estimates; margins cover small sampling errors.

Every successful run, including `--preview` and explicit-box replay, writes `boxes_preview.png` alongside `boxes.json` before crop copying begins. It contains an XY section, a full YZ section, close-ups of both ends, and an XZ section for each beam. Large colored numbers match the JSON indices and output order; the same colors follow a beam through all panels. Axes use original voxel indices. XY preserves the X/Y voxel aspect ratio; longitudinal views stretch the length to show the full scan. Contrast is set from twice the median sampled foreground intensity. Use `--no-box-preview` to skip image generation. `--preview` skips large voxel outputs while retaining both diagnostic files.

Rectangular crops may include parts of holders to preserve all detected beam edges and their margin. To deliberately remove such overlap, use `--trim-end-clutter`, `--end-trim-mm`, or explicit bounds. The optional end check is heuristic and can remove real beam length; it considers compact detached components near the outer 10% of a beam.

`boxes.json` stores zero-based XYZ indices: `min` inclusive, `max` exclusive, `size = max - min`. Crop index `p` maps to original index `p + min`. The manifest includes source dimensions, type, header, parameters and write status. Replay validates source size/header, not its payload hash. Failed writes leave recoverable bounds with `crops_written: false`.

Explicit boxes bypass detection and preserve the supplied order; margins do not alter them:

```sh
build/scan_separator scan.nrrd --box 354,679,420,576,819,10400 --labels A -o manual_crops
```

To extend detection, start with `seeds()`, `measure()` and `detect()` in `src/scan_separator.cpp`; I/O is contained in `Volume`, `manifest()` and `write_crops()`. Extend the diagnostics in `src/preview.cpp`. Add behavioral checks to `tests/integration.py`. The reader implements the relevant subset of the [NRRD format](https://teem.sourceforge.net/nrrd/format.html).
