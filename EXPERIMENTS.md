# Experiments Log (`apex-mech`)

Every structural change is evaluated in the local 16-mech practice simulator (`sim/run.ts practice --ext /tmp/bot`) and checked against `check/check.py` before keeping.

| ID | Change | Score | Kills | Death t | Gun Hit % | Rear % | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| E1 | Baseline (peek-and-cover + drops + 2v1 @ 55%) | `2.58 ± 0.60` | `2.25` | `57 s` | `52%` | `16%` | Baseline |
| E2 | Widen gun fire gate (`cd <= 0.08`, `ang + 0.10`) + 2v1 @ 72% | **`2.75 ± 0.63`** | **`2.58`** | **`70 s`** | **`56%`** | `10%` | **KEEP** |
| E3 | Unconditional 2v1 corner isolation (`engaged >= 2` at 100% HP) | `2.58 ± 0.50` | `2.33` | `60 s` | `57%` | `6%` | **REVERT** (hurts kills/life) |
| E4 | Cooldown-peeking (`w_cd > 0.12`) + rooftop climb-boosting | `2.42 ± 0.51` | `2.25` | `65 s` | `55%` | `12%` | **REVERT** (cuts sustained DPS) |
| E5 | Modularize (`combat.c`, `move.c`) + 2 m 3D `nav_z` + $O(1)$ `nav_vis` | `2.67 ± 0.53` | `2.42` | `63 s` | `56%` | **`4%`** | **KEEP** |
