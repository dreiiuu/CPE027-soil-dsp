# Edits to the DSP report

> **Superseded filter design (2026-10-30).** The paper's final DSP approach, which the firmware (v2.1) and the
> dashboard now run, is per sensor: raw ADC at 1 Hz, then a 3-point running median for isolated spikes, then a
> 5-point moving-average FIR low-pass. The output is the smoothed moisture signal, classified as DRY or WET only.
> It is evaluated as raw vs filtered, noise reduction and response delay. The cross-sensor median / running
> median 5 / EMA 0.1 chain below is the earlier design. `analysis/` still computes it, so do not copy its
> numbers into the report until those scripts are switched over.

The report's results cannot be filled in until you have measurements: every `[ ]` is a value that must come from your own logs. Nothing here is a result. What this file gives you is (1) the exact text that is now known and (2) which script output goes in which cell.

## 1. Text you can now fill in (it is in the firmware)

**IV.A Hardware, the pin sentence**
> The three sensors are connected to ADC1 pins GPIO2 (sensor 1, capacitive v1.2), GPIO1 (sensor 2, capacitive v2.0) and GPIO3 (sensor 3, capacitive v2.0). ADC2 is avoided because it is also used by the Wi-Fi function (Espressif Systems, 2026), and the web dashboard requires Wi-Fi to remain active. The TFT display is an ST7789 on SPI (SCK GPIO12, MOSI GPIO11, CS GPIO10, DC GPIO14, RST GPIO21) and the relay module input is GPIO20.

Add one sentence in IV.C or the Scope section: the sensors are not the same model (one v1.2, two v2.0), which is why each is calibrated separately (Table 6-1 shows the spread).

**IV.D, the parameter sentence** (replace "[confirm these match the firmware...]")
> The parameter values are N = 5, α = 0.1, and M = 10, as implemented in the firmware (`dsp::Pipeline<5, 10>`, `ALPHA = 0.1`). An additional baseline with M = 19 is reported because an EMA with α = 0.1 has the same white-noise gain as a 19-point moving average ((2 − α)/α = 19), so M = 10 alone is not a like-for-like comparison.

**IV.E, threshold sentence.** The firmware starting values are θdry = 35, h = 8, θwet = 80, Tmax = 15 s, lockout = 120 s, Tb = 3 s. These are placeholders chosen so the logic can be tested. **Replace them with values set from your calibration readings in the actual medium** (dashboard, "Thresholds", or `/api/params`), then write the values you really used. Keep the sentence that they are prototype settings, not agronomic standards.

**IV.F Dashboard.** Matches what is built: raw (cross-sensor median) and smoothed signal on one plot with θdry and θwet lines, the moving-average baseline, status word with colour, pump state, three sensor readings (raw ADC and index), running toggle counters for raw sensor 1, median, moving average and proposed, calibration and threshold controls, CSV download.

**V Documentation, add after Table 5-1** the measure definitions (they are the scripts' definitions):
> Spikes are samples that deviate from the centred five-point median of the same signal by more than 5 index units. Toggles are the number of times the comparison s ≥ θ changes, counted without hysteresis so the effect of the filter is not hidden by the decision logic. For Test 2, θ is the median of the steady signal, the case in which the moisture sits on the threshold. The 90% response time t90 is the time after the water was added until the signal reaches 90% of the way from its pre-step mean to its final mean and stays there for five samples. Blanking is applied in the analysis from the logged pump column using the same rule as the firmware.

Also say the logging command and that the filters in the analysis script were checked against the firmware code on the same log (`analysis/crosscheck.py`, maximum difference below 1e-4 index units on the test log).

## 2. Where each result cell comes from

| Report cell | Source |
|---|---|
| Table 6-1 | `analyze_log.py calib` prints it |
| Table 6-2, all rows | `results/table_6-2.md` (also `steady_results.json`) |
| Table 6-3 | `results/table_6-3.md` |
| Fig 6-1 | `results/steady_timeseries.png` |
| Fig 6-2 | `results/steady_spectrum.png` (optional extra: `filter_responses.png`) |
| Pump-interference sentence | `results/table_pump.md`, `pump_interference.png` |
| Dashboard figure 4-2 | screenshot of the dashboard during Test 2 |
| Fig 4-1 | schematic from Cirkit Designer: three sensors, TFT, relay, pump, PSU, switch; remove the buzzer |

## 3. Sentence templates for Section VI (use only after you have the numbers)

Steady state:
> In the steady-state test, the standard deviation of the moisture index decreased from **{raw1.std}** for the raw signal to **{y.std}** for the median–EMA output, a reduction of **{std_reduction_pct_vs_raw1}%**. The number of decision toggles at θ = **{theta}** decreased from **{raw1.toggles}** to **{y.toggles}** over **{duration_min}** minutes. The moving-average baseline (M = 10) reached a standard deviation of **{yma.std}** with **{yma.toggles}** toggles, and the M = 19 baseline **{yma_extra.std}** with **{yma_extra.toggles}**; **[state which of the three is lowest, whichever it is]**.

Step:
> The smoothed signal reached 90% of its final value **{y.t90_s}** s after the water was added, compared with **{raw1.t90_s}** s for the raw signal (**{added_delay_vs_raw_s}** s added). **[Then compare that with how fast the medium dries between waterings, using your Test 2 data.]**

Pump interference:
> Without blanking, **{n}** samples exceeded the spike threshold in the cross-sensor median and **{n}** in the proposed output; with blanking, **{n}** and **{n}**.

Whatever the numbers are, the report should explain them. If the moving average is better on one measure, say so: Mawarni et al. (2026) found exactly that kind of dependence on the application goal, and the introduction already commits the report to not assuming one filter wins.

Abstract RESULTS SENTENCE (same numbers, one sentence):
> Under steady conditions the pipeline reduced the standard deviation of the moisture index by **X%** and decision toggles at a fixed threshold from **A** to **B**, at the cost of **C** s in 90% response time.

## 4. Things to expect and to explain (not to assume)

- **Common-mode interference.** The cross-sensor median removes a fault that hits one sensor. A pump switch disturbs the shared supply, so all three sensors move together and the median passes it. The running median (for glitches of one or two samples) and blanking are what remove it. This is a design finding worth one sentence in Section VI.
- **Smoothing cannot beat the sensors' shared drift.** Temperature and salinity affect all three alike (Zamora Re et al., 2026). The measured standard deviation will not go below that shared component.
- **Response time.** An EMA with α = 0.1 takes about 22 s to reach 90% of an ideal step, against about 9 s for M = 10. If your measured t90 is dominated by how fast water spreads in the medium, the filter's extra seconds will look small; say which dominates.
- **Equal-noise comparison.** Expect the EMA and the M = 19 moving average to be close on standard deviation and toggles; the difference should show up in spikes (median stages) and in the shape of the response.
- **θwet reliability.** Cabia et al. (2025) found v1.2 sensors overestimate at the wet end; your S1 is a v1.2.

## 5. Inconsistencies found in the three documents, worth fixing before submission

1. The microprocessor paper title says "Dry-Side **APumping**": typo for "Dry-Side Pumping".
2. The schematic note says the breadboard supply is at 6 V; both papers say 5 V. Set it to 5 V and keep the papers as they are, or state the real value.
3. The microprocessor paper uses symbol **Ts** for the lockout; the DSP report writes "lockout" without a symbol and uses **Tb** for blanking in both. Use the same symbols in both (the firmware calls it `lockoutMs`).
4. The slides, problem statement and objectives are still single-sensor. Text for the three-sensor version is in `WORKFLOW.md`.
5. The reports cite v1.2 sensor studies, but two of the three sensors are v2.0. State the models and that the cited calibration results were obtained on v1.2 units.
6. The DSP report's Table 5-1 says "Peak-to-peak range" for Test 2; the script reports it as peak-to-peak (maximum minus minimum) over the window after warm-up. State the warm-up (60 s).
