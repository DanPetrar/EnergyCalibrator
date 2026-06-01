# EnergyCalibrator — Firmware dkWh Accumulation Audit (Phase 1)

**Date:** 2026-06-01
**Scope:** Verify that the per-minute box **dkWh** values stored in `cal_min`
(which the report sums) faithfully represent the box's true energy — no
double-counting, loss, drift, or restart/rollback artifacts. Phase 1 =
static code audit + live cross-checks; **no hardware changes.**
**Unit:** D (`cal_F07F8C`, 192.168.110.104), firmware v1.0.2.

---

## 1. Energy data flow

```
Box (own kWh counters)
  └─ U/V/W per-minute frames "kwh,kvarh"  ──serial GPIO5──▶ ESP
        parse_min():  dKwh = kwh − prevBoxKwh[ch]   (rollback-guarded)
                      cumKwh[ch] += dKwh            (running total, persisted)
        loop @ W-event: dKwhThisMin[ch] = cumKwh[ch] − prevCumKwhAtMin[ch]
                        sdm630Poll()  → dMeterKwh = meter.kwh − prevMeterKwh
                        mqttPublishPaired(dKwhThisMin)  → <topic>/min JSON
  collector → cal_min:  R/S/T_dkwh = dKwhThisMin,  mtr_dkwh = dMeterKwh
  report:  total = Σ dkwh,  dev = (Σ box_dkwh − Σ mtr_dkwh)/Σ mtr_dkwh
```

Both `dKwhThisMin` and `dMeterKwh` are measured at the **same W-frame event**,
so in the normal case box and SDM dkWh cover the **same wall-clock interval** —
the box-vs-SDM comparison is valid.

---

## 2. Methods

- **M1** Static code audit of `EnergyCalibrator.ino` (parse_min, loop W-event),
  `EnergyLog.h` (persistence), `cal_collector.py` (storage).
- **Forensics** Inspected today's `cal_min` for gaps / poll failures.
- **M3** SDM telescoping: `/api/sdm` absolute kWh delta vs `Σ mtr_dkwh` over a
  clean 13-min window (no reboot).
- **M4** Box reconstruction: `/api/data total_kwh` delta vs `Σ (R+S+T)_dkwh`
  over the same window.

---

## 3. Findings

| ID | Severity | Status | Summary |
|----|----------|--------|---------|
| F1 | Moderate | **Fixed (v1.0.3)** | SDM-poll-failure misalignment (see below) |
| F2 | Low | By design | Reboot/OTA gap dropped from per-minute series, symmetric |
| F3 | Low | Report-immune | First-sample adds absolute box counter to `cumKwh` |
| F4 | Info | Documented | Dual accumulator is consistent; roles now commented in source |
| F5 | Info | Inherent | ~1 Wh/min box quantization → short-window dkWh noise |

### F1 — SDM-poll-failure misalignment *(latent; 0 occurrences today)*
When `sdm630Poll()` fails, `mqttPublishPaired()` is skipped, so `prevMeterKwh`
is **not** advanced → the next successful `mtr_dkwh` telescopes to cover **2
minutes**. But `prevCumKwhAtMin` **is** advanced every W-event, so the next
stored row's **box** dkWh covers only **1 minute**, and the failed minute's box
energy is discarded (its row was never published). Net per failure: that row
pairs 1 min box vs 2 min SDM, and the daily `Σ box_dkwh` loses one minute while
`Σ mtr_dkwh` keeps it → a small **negative** box-vs-SDM bias (~−0.05% per
failure at ~500 W).
**Recommended fix:** on a failed poll, persist the box-only minute too (don't
discard `dKwhThisMin`), or defer advancing `prevCumKwhAtMin` until the row is
actually published. Not urgent — SDM comms are reliable; verify before relying
on sub-percent precision during periods with SDM `read err` WARNings.

**RESOLVED in v1.0.3 (2026-06-01):** the per-minute box delta and
`prevCumKwhAtMin` now advance only inside the successful-publish branch, so a
skipped minute folds into the next published row symmetrically for both box and
meter (matching the SDM telescoping). Verified by host model test
`arduino/tests/test_energy_accumulator.py` (old logic loses the failed minute;
new logic is aligned + lossless, incl. consecutive failures). Normal path
unchanged; OTA'd to Unit D, stable, paired rows flowing.

### F2 — Reboot/OTA gap *(deviation-safe)*
`prevCumKwhAtMin` and `prevMeterKwh` are loop-static and reset to −1 on boot, so
the first post-reboot minute emits `0` dkWh for **both** box and SDM
symmetrically — the deviation is preserved, but the absolute daily totals
undercount the reboot gap. Box `cumKwh` itself does **not** lose the gap
(`prevBoxKwh` is restored from NVS and the box counts independently during the
ESP reboot). A cross-check window that crosses a reboot will show an apparent
undercount; this is expected.

### F3 — First-sample offset *(report-immune)*
On a fresh unit (`prevBoxKwh == −1`), `parse_min` does `cumKwh += kwh` (the box's
**absolute** counter), inflating `cumKwh` / `/api/data total_kwh`. The loop's
`prevCumKwhAtMin == −1` guard suppresses this from the first emitted dkWh, and
the report sums **dkWh** (not `cumKwh`), so KPIs are unaffected. Only the
absolute `total_kwh` display carries the offset.

### F4 — Dual accumulator *(not a bug, simplify later)*
`cumKwh` (updated in `parse_min`) and `dKwhThisMin = cumKwh − prevCumKwhAtMin`
(loop) are mathematically consistent — `dKwhThisMin` telescopes to the per-minute
box delta. The duplication is a maintainability smell only.

### F5 — Quantization *(inherent)*
Box kWh resolution is ~1 Wh/min, so a single 30-min window's dkWh deviation can
swing several percent (seen as transient ±4–6% blips). Averages out over hours;
**do not** read sub-hourly dkWh deviations as drift.

---

## 4. Cross-check results (clean 13-min window, no reboot)

| Check | Reference | DB sum | Δ | Verdict |
|-------|-----------|--------|---|---------|
| M3 SDM telescoping | `/api/sdm` Δ = +0.116 kWh | `Σ mtr_dkwh` = +0.123 | 0.0070 kWh | OK (≤1-min fuzz) |
| M4 box reconstruction | `/api/data` Δ = +0.36 kWh | `Σ box_dkwh` = +0.36 | **0.0000 kWh** | OK (exact) |

M4 being **exact** proves the box dkWh stored in `cal_min` reconstructs the
firmware `cumKwh` growth with zero loss. Combined with the static proof that
`cumKwh` telescopes the box's own counter, the stored box dkWh ≈ the box counter
delta (modulo F1/F2 edge cases). M3's 7 Wh residual is one-minute boundary fuzz
between the on-demand `/api/sdm` polls and the stored per-minute polls.

Today's data: **1022 min rows, 0 gaps ≥100 s, 0 SDM poll failures.** Daily
deviations CT-R −0.91%, CT-S +0.07%, CT-T −1.35%.

---

## 5. Verdict

**The per-minute box dkWh the report sums is faithful to both the firmware
accumulator (M4 exact) and the meter register (M3 within fuzz).** The
accumulation logic is **sound for the daily-deviation calibration purpose.**
Edge cases (F1 poll-failure bias, F2 reboot-gap) are real but either inactive
today (F1) or deviation-neutral (F2). The reported numbers can be trusted to the
documented precision.

**Recommendations:**
1. ~~Fix **F1**~~ **Done in v1.0.3** — `prevCumKwhAtMin` deferred to successful publish.
2. ~~Collapse/document the **F4** dual accumulator~~ **Documented in source** —
   a structural collapse was evaluated and rejected (equal complexity, would risk
   the F1/F2 behavior); the two accumulators' roles + the publish-baseline
   invariant are now commented at their declaration sites.
3. Leave **F3/F5** as-is (report-immune / inherent).

**Phase 2 (box-counter ground truth) — not required** for confidence in the
report, since M4 is exact. Worth doing only to deliberately exercise the F1 /
rollback / missed-frame edge cases via bench frame-injection.
