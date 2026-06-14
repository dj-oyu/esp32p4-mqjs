#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "numpy>=2.0",
#   "pillow>=11.0",
#   "qrcode[pil]>=8.0",
#   "zxing-cpp>=2.3",
# ]
# ///
"""Synthetic QR readability benchmark for MQJSP1 provisioning payloads.

This measures optical/readability margin and host decoder time. It does not
predict ESP32-P4 CPU time; the same corpus should later be fed to quirc on the
device. Images emulate the existing 400x300 camera analysis surface.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
import random
import statistics
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import qrcode
import zxingcpp
from PIL import Image, ImageEnhance, ImageFilter

from mqjs_provision_qr import encode_payload


@dataclass
class Result:
    payload: str
    modules: int
    canvas: str
    side_px: int
    module_px: float
    rotation_deg: int
    perspective: float
    blur: float
    contrast: float
    trial: int
    success: bool
    decode_ms: float


def payloads() -> dict[str, str]:
    wifi = {
        "v": 1,
        "id": "bench-wifi",
        "wifi": {"ssid": "home-wifi", "password": "correct horse battery staple"},
    }
    combined = {
        "v": 1,
        "id": "bench-combined",
        "device": "tab5-a1b2",
        "wifi": {"ssid": "home-wifi", "password": "correct horse battery staple"},
        "tailscale": {"authKey": "tskey-auth-" + "A" * 64},
    }
    return {"wifi": encode_payload(wifi), "combined": encode_payload(combined)}


def make_qr(text: str) -> tuple[Image.Image, int]:
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=8,
        border=4,
    )
    qr.add_data(text)
    qr.make(fit=True)
    image = qr.make_image(fill_color="black", back_color="white").convert("L")
    return image, qr.modules_count


def perspective_coeffs(src: list[tuple[float, float]],
                       dst: list[tuple[float, float]]) -> tuple[float, ...]:
    matrix = []
    vector = []
    for (x, y), (u, v) in zip(dst, src):
        matrix.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        vector.append(u)
        matrix.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        vector.append(v)
    return tuple(np.linalg.solve(np.asarray(matrix), np.asarray(vector)))


def render(base: Image.Image, canvas: tuple[int, int], side: int, rotation: int,
           perspective: float, blur: float, contrast: float,
           rng: random.Random) -> Image.Image:
    qr = base.resize((side, side), Image.Resampling.BILINEAR)
    if perspective:
        inset = side * perspective
        src = [(0, 0), (side, 0), (side, side), (0, side)]
        dst = [
            (rng.uniform(0, inset), rng.uniform(0, inset)),
            (side - rng.uniform(0, inset), rng.uniform(0, inset)),
            (side - rng.uniform(0, inset), side - rng.uniform(0, inset)),
            (rng.uniform(0, inset), side - rng.uniform(0, inset)),
        ]
        qr = qr.transform(
            (side, side),
            Image.Transform.PERSPECTIVE,
            perspective_coeffs(src, dst),
            Image.Resampling.BILINEAR,
            fillcolor=255,
        )
    if rotation:
        qr = qr.rotate(rotation, Image.Resampling.BILINEAR, expand=True,
                       fillcolor=255)
    if blur:
        qr = qr.filter(ImageFilter.GaussianBlur(blur))
    if contrast != 1.0:
        qr = ImageEnhance.Contrast(qr).enhance(contrast)
    image = Image.new("L", canvas, 225)
    x = (canvas[0] - qr.width) // 2
    y = (canvas[1] - qr.height) // 2
    image.paste(qr, (x, y))
    return image


def decode(image: Image.Image, expected: str) -> tuple[bool, float]:
    begin = time.perf_counter_ns()
    result = zxingcpp.read_barcode(np.asarray(image))
    elapsed = (time.perf_counter_ns() - begin) / 1_000_000
    return bool(result and result.text == expected), elapsed


def run(args: argparse.Namespace) -> list[Result]:
    rng = random.Random(args.seed)
    results: list[Result] = []
    cases = itertools.product(
        args.side, args.rotation, args.perspective, args.blur, args.contrast
    )
    cases = list(cases)
    for name, text in payloads().items():
        base, modules = make_qr(text)
        for side, rotation, perspective, blur, contrast in cases:
            for trial in range(args.trials):
                image = render(
                    base, (args.width, args.height), side, rotation,
                    perspective, blur, contrast, rng
                )
                ok, ms = decode(image, text)
                results.append(Result(
                    payload=name,
                    modules=modules,
                    canvas=f"{args.width}x{args.height}",
                    side_px=side,
                    module_px=round(side / (modules + 8), 2),
                    rotation_deg=rotation,
                    perspective=perspective,
                    blur=blur,
                    contrast=contrast,
                    trial=trial,
                    success=ok,
                    decode_ms=round(ms, 3),
                ))
                if args.images and (not ok or trial == 0):
                    suffix = "ok" if ok else "fail"
                    path = Path(args.images) / name
                    path.mkdir(parents=True, exist_ok=True)
                    image.save(path / (
                        f"s{side}_r{rotation}_p{perspective}_b{blur}_"
                        f"c{contrast}_t{trial}_{suffix}.png"
                    ))
    return results


def summarize(results: list[Result]) -> str:
    lines = [
        "# QR Readability Benchmark",
        "",
        "Host decoder: zxing-cpp. Canvas emulates the current 400x300 "
        "camera analysis image. Decode time is host-only.",
        "",
        "| Payload | Modules | Side px | px/module | Success | p50 ms | p95 ms |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    groups: dict[tuple[str, int], list[Result]] = {}
    for row in results:
        groups.setdefault((row.payload, row.side_px), []).append(row)
    for (payload, side), rows in sorted(groups.items()):
        times = sorted(row.decode_ms for row in rows)
        p95 = times[max(0, math.ceil(len(times) * 0.95) - 1)]
        success = sum(row.success for row in rows) / len(rows) * 100
        lines.append(
            f"| {payload} | {rows[0].modules} | {side} | "
            f"{rows[0].module_px:.2f} | {success:.1f}% | "
            f"{statistics.median(times):.2f} | {p95:.2f} |"
        )
    lines.extend([
        "",
        "## Clean-case floor",
        "",
        "| Payload | Smallest clean side decoded | px/module |",
        "|---|---:|---:|",
    ])
    for payload in sorted({row.payload for row in results}):
        clean = [
            row for row in results
            if row.payload == payload and row.rotation_deg == 0
            and row.perspective == 0 and row.blur == 0
            and row.contrast == 1.0 and row.success
        ]
        if clean:
            best = min(clean, key=lambda row: row.side_px)
            lines.append(
                f"| {payload} | {best.side_px} | {best.module_px:.2f} |"
            )
        else:
            lines.append(f"| {payload} | none | - |")
    return "\n".join(lines) + "\n"


def csv_write(path: Path, results: list[Result]) -> None:
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=asdict(results[0]).keys())
        writer.writeheader()
        writer.writerows(asdict(row) for row in results)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--width", type=int, default=400)
    root.add_argument("--height", type=int, default=300)
    root.add_argument("--side", type=int, nargs="+", default=[80, 120, 160, 220])
    root.add_argument("--rotation", type=int, nargs="+", default=[0, 15, 30])
    root.add_argument("--perspective", type=float, nargs="+", default=[0, 0.12])
    root.add_argument("--blur", type=float, nargs="+", default=[0, 1.0, 2.0])
    root.add_argument("--contrast", type=float, nargs="+", default=[1.0, 0.6])
    root.add_argument("--trials", type=int, default=3)
    root.add_argument("--seed", type=int, default=1)
    root.add_argument("--csv", type=Path)
    root.add_argument("--markdown", type=Path)
    root.add_argument("--images")
    return root


def main() -> int:
    args = parser().parse_args()
    results = run(args)
    report = summarize(results)
    print(report, end="")
    if args.csv:
        csv_write(args.csv, results)
    if args.markdown:
        args.markdown.write_text(report, encoding="utf-8")
    print(json.dumps({
        "cases": len(results),
        "success": sum(row.success for row in results),
        "success_rate": round(sum(row.success for row in results)
                              / len(results) * 100, 1),
    }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
