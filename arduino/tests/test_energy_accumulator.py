#!/usr/bin/env python3
"""
Host model of the per-minute energy accumulator, used to validate the F1 fix
(SDM-poll-failure misalignment) without hardware.

Models the firmware's ch==2 (W-frame) handling for both the OLD behavior
(baseline `prevCumKwhAtMin` advanced every minute) and the NEW behavior
(advanced only on a successful paired publish). Replays a frame sequence with an
injected poll failure and asserts the stored box/meter dkWh.

Run standalone (no deps):
    python3 arduino/tests/test_energy_accumulator.py
"""

import sys


def simulate(minutes, mode):
    """minutes: list of (box_delta, sdm_delta, poll_ok). Returns stored rows
    [(box_dkwh, mtr_dkwh), ...] as the collector would receive them."""
    box_cum = sdm_cum = 0.0          # box counter / true meter register
    prev_cum = prev_meter = -1.0     # firmware baselines
    rows = []
    for bd, sd, ok in minutes:
        box_cum += bd                # parse_min advances cumKwh every minute
        sdm_cum += sd                # real meter register advances regardless

        def dbox():
            d = (box_cum - prev_cum) if prev_cum >= 0 else 0.0
            return d if d > 0 else 0.0

        def dmtr():
            d = (sdm_cum - prev_meter) if prev_meter >= 0 else 0.0
            return d if d > 0 else 0.0

        if mode == 'old':
            # baseline advances EVERY minute, before the poll result is known
            b = dbox(); prev_cum = box_cum
            if ok:
                m = dmtr(); prev_meter = sdm_cum
                rows.append((b, m))
            # poll failed -> row skipped, b is lost (prev_cum already advanced)
        elif mode == 'new':
            if ok:
                b = dbox(); prev_cum = box_cum     # advance only on success
                m = dmtr(); prev_meter = sdm_cum
                rows.append((b, m))
            # poll failed -> nothing advances; this minute folds into the next
        else:
            raise ValueError(mode)
    return rows


def _scenario():
    # 10 minutes, box == meter (true deviation should be 0), one poll failure @ idx 5
    mins = [(0.005, 0.005, True)] * 10
    mins[5] = (0.005, 0.005, False)
    return mins


def test_new_is_aligned_and_lossless():
    rows = simulate(_scenario(), 'new')
    box = sum(r[0] for r in rows)
    mtr = sum(r[1] for r in rows)
    # minute 0 establishes the baseline (dkwh 0); energy accrues over minutes 1..9
    true_energy = 0.005 * 9
    assert abs(box - true_energy) < 1e-9, f"box {box} != true {true_energy}"
    assert abs(box - mtr) < 1e-9, f"box {box} != mtr {mtr} (should be aligned)"
    dev = (box - mtr) / mtr * 100
    assert abs(dev) < 1e-9, f"dev {dev:+.3f}% should be 0"


def test_old_loses_the_failed_minute():
    rows = simulate(_scenario(), 'old')
    box = sum(r[0] for r in rows)
    mtr = sum(r[1] for r in rows)
    # OLD drops the failed minute from box but the meter telescopes it in
    assert box < mtr - 1e-9, f"expected box<{mtr}, got {box}"
    assert abs(mtr - 0.005 * 9) < 1e-9          # meter total is correct
    assert abs(box - 0.005 * 8) < 1e-9          # box lost exactly one minute
    dev = (box - mtr) / mtr * 100
    assert dev < -1.0, f"expected clear negative bias, got {dev:+.2f}%"


def test_consecutive_failures_fold_into_next():
    mins = [(0.005, 0.005, True)] * 10
    mins[4] = (0.005, 0.005, False)
    mins[5] = (0.005, 0.005, False)             # two in a row
    rows = simulate(mins, 'new')
    box = sum(r[0] for r in rows); mtr = sum(r[1] for r in rows)
    assert abs(box - mtr) < 1e-9 and abs(box - 0.005 * 9) < 1e-9
    # the minute after the gap carries 3 minutes of box energy (4,5,6)
    assert any(abs(b - 0.015) < 1e-9 for b, m in rows), rows


if __name__ == '__main__':
    tests = [v for k, v in sorted(globals().items())
             if k.startswith('test_') and callable(v)]
    failed = 0
    for t in tests:
        try:
            t(); print(f'  PASS  {t.__name__}')
        except AssertionError as e:
            failed += 1; print(f'  FAIL  {t.__name__}: {e}')
    print(f'\n{len(tests)-failed}/{len(tests)} passed')
    sys.exit(1 if failed else 0)
