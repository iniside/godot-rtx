"""Summarize an explicitly requested DDGI GPU capture; optionally export probe positions."""

import argparse
import collections
import csv
import json
import math
import struct
from pathlib import Path


def inspect(metadata_path, csv_path=None):
    metadata = json.loads(metadata_path.read_text())
    prefix = metadata_path.with_suffix("")
    axis = metadata["probe_axis"]
    count = axis**3

    def read(name, format, expected):
        path = Path(f"{prefix}-{name}.bin")
        data = path.read_bytes()
        size = struct.calcsize(format)
        if len(data) != size * expected:
            raise ValueError(f"{path}: expected {size * expected} bytes, got {len(data)}")
        return list(struct.iter_unpack(format, data))

    report = {"metadata": str(metadata_path), "frame": metadata["frame"], "cascades": []}
    rows = []
    for cascade_index, cascade in enumerate(metadata["cascades"]):
        name = f"cascade-{cascade_index}"
        positions = read(f"{name}-position-state-rgba32f", "<4f", count)
        traced = read(f"{name}-traced-position-state-rgba32f", "<4f", count)
        validity = read(f"{name}-validity-r32ui", "<I", count)
        updated = read(f"{name}-update-frame-r32ui", "<I", count)
        irradiance = read(f"{name}-irradiance-rgba16f", "<4e", count * 8 * 8)
        spacing = metadata["base_spacing"] * 2**cascade_index
        valid_count = active_count = unmatched_valid_count = 0
        ages = collections.Counter()
        for probe, position in enumerate(positions):
            physical = (probe % axis, probe // (axis * axis), (probe // axis) % axis)
            absolute = tuple(m + (p - m) % axis for m, p in zip(cascade["minimum_cell"], physical))
            # SDK offsets are normalized by probe spacing, unlike the absolute cells.
            world = tuple((cell + offset) * spacing for cell, offset in zip(absolute, position[:3]))
            valid = validity[probe][0] != 0
            active = position[3] == 0.0
            age = (metadata["frame"] - updated[probe][0]) % 2**32 if valid else None
            valid_count += valid
            active_count += active
            unmatched_valid_count += valid and position[:3] != traced[probe][:3]
            if age is not None:
                ages[age] += 1
            rows.append((cascade_index, probe, *absolute, *world, int(valid), int(active), age))
        channels = [value for texel in irradiance for value in texel[:3]]
        finite = [value for value in channels if math.isfinite(value)]
        report["cascades"].append(
            {
                "minimum_cell": cascade["minimum_cell"],
                "valid": valid_count,
                "active": active_count,
                "valid_with_unmatched_position": unmatched_valid_count,
                "age_histogram": dict(sorted(ages.items())),
                "irradiance_encoded_max": max(finite, default=0),
                "irradiance_nonfinite_components": len(channels) - len(finite),
            }
        )
    camera = read("camera-rgba16f", "<4e", metadata["camera_width"] * metadata["camera_height"])
    channels = [value for texel in camera for value in texel[:3]]
    finite = [value for value in channels if math.isfinite(value)]
    report["camera"] = {
        "min": min(finite, default=0),
        "max": max(finite, default=0),
        "nonfinite_components": len(channels) - len(finite),
        "nonzero_components": sum(value != 0 for value in finite),
    }
    if csv_path:
        with csv_path.open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                ("cascade", "physical_index", "cell_x", "cell_y", "cell_z", "world_x", "world_y", "world_z", "valid", "active", "age")
            )
            writer.writerows(rows)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", type=Path, help="JSON file printed by DDGI_CAPTURE")
    parser.add_argument("--positions", type=Path, help="Optional CSV of absolute probe positions, validity and age")
    args = parser.parse_args()
    print(json.dumps(inspect(args.metadata, args.positions), indent=2))
