#!/usr/bin/env python3
"""Fit the Tab5 OCV -> SoC table from a real discharge trace.

The table in components/pwr_tab5/pwr_gauge.c ships as a generic 2S Li-ion
curve. This turns one hands-off discharge run into the measured replacement.

Recording the trace (see docs/battery-power-design.md):

    # on the broker host
    mosquitto_sub -h <broker> -t 'esp32p4-mqjs/battery' -v > trace.log
    # on the device: charge to full, unplug, start the battery_trace app,
    # walk away for ~6 h until it shuts itself down.

    python tools/battery_fit.py trace.log

Lines are the CSV the app publishes, optionally prefixed by the topic the way
`mosquitto_sub -v` prints it:

    ms,mv,ma,ocv_mv,pct,mah,mohm,state,usb,raw

WHAT IT ASSUMES. That the run starts at a full pack and ends where the device
shut itself down, because the capacity it reports is exactly the charge the
device delivered between those two points -- the usable capacity, not the
one on the label. A trace that starts at 60% produces a curve compressed
into the top 60%, so do not feed it one.
"""
import sys
from bisect import bisect_left

BREAKPOINTS = [100, 95, 90, 80, 70, 60, 50, 40, 30, 20, 12, 7, 3, 0]


def parse(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            # `mosquitto_sub -v` prefixes the topic; the payload is the rest
            if " " in line and "," in line.split(" ", 1)[1]:
                line = line.split(" ", 1)[1]
            parts = line.split(",")
            if len(parts) < 7:
                continue
            try:
                ms, mv, ma, ocv, pct, mah, mohm = (int(p) for p in parts[:7])
            except ValueError:
                continue
            rows.append((ms, mv, ma, ocv))
    return rows


def integrate(rows):
    """Charge drawn from the pack, in mAh, cumulative per sample."""
    used = [0.0]
    for i in range(1, len(rows)):
        dt_h = (rows[i][0] - rows[i - 1][0]) / 3600000.0
        if dt_h <= 0 or dt_h > 1.0:  # a gap that big is a restart, not a load
            used.append(used[-1])
            continue
        ma = (rows[i][2] + rows[i - 1][2]) / 2.0  # trapezoid
        used.append(used[-1] - ma * dt_h)         # discharge is negative mA
    return used


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    rows = parse(argv[1])
    if len(rows) < 60:
        print(f"only {len(rows)} usable samples -- that is not a discharge run")
        return 1

    mean_ma = sum(r[2] for r in rows) / len(rows)
    if mean_ma > 0:
        print("WARNING: mean current is POSITIVE over the whole trace.")
        print("  This firmware treats + as charging, so either the pack was")
        print("  charging (wrong trace) or the sign is inverted on this board")
        print("  -- see CONFIG_MQJS_TAB5_BATTERY_INVERT_CURRENT.")
        print()

    used = integrate(rows)
    cap = used[-1]
    if cap <= 0:
        print("no net discharge in this trace")
        return 1

    hours = (rows[-1][0] - rows[0][0]) / 3600000.0
    print(f"samples      {len(rows)}")
    print(f"duration     {hours:.2f} h")
    print(f"capacity     {cap:.0f} mAh delivered "
          f"({rows[0][1]} mV -> {rows[-1][1]} mV terminal)")
    print(f"mean current {mean_ma:.0f} mA, peak {min(r[2] for r in rows)} mA")
    print(f"OCV span     {rows[0][3]} mV -> {rows[-1][3]} mV")
    print()

    # SoC(i) = what is left, as a fraction of what the run delivered.
    soc = [100.0 * (cap - u) / cap for u in used]
    # soc is monotonically decreasing; bisect wants increasing, so search the
    # reversed sequence.
    rsoc = soc[::-1]

    print("/* measured on <date>, <pack>, tools/battery_fit.py */")
    print("static const struct { int16_t mv; int8_t soc; } s_ocv[] = {")
    for bp in BREAKPOINTS:
        idx = len(rsoc) - bisect_left(rsoc, bp) - 1
        idx = max(0, min(len(rows) - 1, idx))
        # average the IR-compensated OCV over a small window: one sample is
        # one ADC reading, and the curve is the thing being measured.
        lo, hi = max(0, idx - 3), min(len(rows), idx + 4)
        ocv = sum(r[3] for r in rows[lo:hi]) / (hi - lo)
        print(f"    {{ {round(ocv):5d}, {bp:3d} }},")
    print("};")
    print()
    print("Paste over s_ocv[] in components/pwr_tab5/pwr_gauge.c, then re-run")
    print("the host test: it checks the curve is monotone, which a noisy trace")
    print("at the flat middle of the discharge can easily break.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
