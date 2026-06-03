# EnergyCalibrator — Product Description

_Firmware v1.0.5 · Last updated: 2026-06-03_

---

## What It Does

EnergyCalibrator is an embedded device for evaluating the accuracy of CT (current transformer) sensors. Three CT sensors are clamped simultaneously on the **same single-phase conductor** and their measurements are compared against a calibrated reference meter (Eastron SDM630-M) to quantify individual deviations.

---

## Measurement Principle

### Reference — Eastron SDM630-M

The SDM630-M is a certified revenue-grade energy meter connected via **RS-485 Modbus RTU**. It measures voltage, current, active power, power factor, frequency, and cumulative energy (kWh) on the reference conductor with high precision. It acts as the ground truth for the calibration process.

### Box — CT Channels R / S / T

The measurement box provides real-time readings from the three CT sensors every second — voltage, current, active power, reactive power, power factor, and frequency per channel.

### Synchronisation

The box emits a **per-minute pulse** at the end of each energy accumulation interval. EnergyCalibrator uses this pulse as a trigger: the moment the pulse arrives, it immediately polls the SDM630 via Modbus to capture the reference meter's energy counter. This ensures the box energy delta and the SDM energy delta are measured over the **same time window**, making the comparison valid regardless of load variation during that minute.

### Energy Comparison

The core KPI is based on energy counters, not instantaneous power snapshots. Each minute, the firmware computes:

- **Box energy delta** — how many kWh each CT channel accumulated over the last minute
- **SDM energy delta** — how many kWh the reference meter recorded over the same interval
- **Deviation** — the difference, expressed both in absolute kWh and as a percentage of the SDM reading

Using energy counters rather than power averages eliminates the effect of MQTT packet drops and measurement jitter. Over a full day, even small systematic errors accumulate into a clearly measurable drift.

---

## Data Collection

Measurements are published in real time to an MQTT broker and stored in a local database on the collection server. Two streams are produced:

- **Per-second** — instantaneous V/A/W readings from each CT channel, for live monitoring
- **Per-minute** — paired box + SDM record including energy deltas and computed deviations, for accuracy analysis

A web-based session workflow allows the operator to label each test run with the DUT serial number, start and stop the session, and generate a calibration report for the recorded window.

---

## Calibration Report

The report covers the full session window and includes:

- **KPI summary** — energy deviation per CT channel vs SDM reference (%)
- **Hourly breakdown** — deviation per hour, colour-coded by severity
- **Load band analysis** — deviation split across power ranges (0–200 W, 200–500 W, 500–1000 W, 1000–1500 W, > 1500 W) to identify load-dependent behaviour
- **CT ranking** — channels ordered by accuracy (Best / Good / Needs calibration)
- **Statistical summary** — mean V/A/W/PF and standard deviation per channel over the session

---

## Supported Hardware

The firmware runs on two ESP32-S3 boards:

- **Waveshare ESP32-S3-Zero** — compact form factor, 4 MB flash, 2 MB PSRAM
- **LilyGO T7 S3 WROOM-1** — extended capacity, 16 MB flash, 8 MB PSRAM, LiPo battery connector

Both boards share the same firmware and pinout. The active unit connects to the measurement box via a serial line and to the SDM630 via an auto-direction RS-485 module.

---

## CT Sensors (current bench)

| Channel | Sensor | Nominal |
|---------|--------|---------|
| R | TDK | 30 A |
| S | TDK | 80 A |
| T | YHDC | 120 A |
