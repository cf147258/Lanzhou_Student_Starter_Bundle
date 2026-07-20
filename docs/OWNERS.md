# Team ownership

| Team | Chapters | Primary module | Daily evidence focus |
|---|---|---|---|
| G1 | 1–2 | state | deterministic twin, calibration, age, uncertainty |
| G2 | 3–4 | transport | frame faults, MQTT-facing timestamps, freshness |
| G3 | 5–6 | runtime | queue location, `poll`, `epoll`, stop latency |
| G4 | 7–8 | safety | arbitration, lock order, gate, audit |
| G5 | 9–10 | gatekeeper | controller faults, integration trace, replay index |

Within every group, the odd-chapter lead, even-chapter lead, and
integration/evidence lead all contribute code and tests. On Days 2 and 4 the two
chapter leads cross-review. Before Day 4, the evidence lead edits one bounded
function linked to each chapter.

The Day 5 review ring is G1→G2→G3→G4→G5→G1. Review does not transfer module
ownership; the owning group remains responsible for the patch and evidence.
