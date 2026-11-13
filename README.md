# Harvest of Shadows

**A fraud program leader's system build: a 14-controller, AI-assisted haunted yard, designed, built and run end to end.**

I've spent 20+ years in investigations and fraud, most recently as Senior Manager,
Investigations & eCommerce Fraud. My work there was building the detection, rules,
case-management and reporting a fraud program runs on, and being accountable for its
results.

Harvest of Shadows is that same discipline pointed at a different problem. I designed the
architecture, wrote the rules, set the governance, built the hardware and owned the
outcome on the night. It detects an event, decides with rules, uses AI only where it
earns its place, caps vendor spend, logs every outcome and never lets a failure go
silent. This repo is the code behind the 2026 show.

![System map](docs/linkedin/system-map.png)

## The 2026 season at a glance

| Measure | 2026 result |
|---|---|
| Visits handled by the show | **183** across the season, **72** on Halloween night |
| Controllers in the yard | **14** (ESP32, ESP32-A1S, ESP-01S), one leader + 13 followers |
| Pre-recorded narrator lines | **~3,100**, two voices cloned locally (no cloud voice service) |
| Live AI calls on Halloween | **35 / 35** placed, **0** errors, **0** fallbacks |
| Total AI cost for the season | **$1.72**, under a hard **$18** lifetime cap |
| Missed cues, final show | **0** |
| Visitor photos stored | **None.** The frame lives in memory for one call, then it's gone |

## How it works

1. **Detect.** Yard cameras feed an object-detection server. People are tracked through
   zones drawn on the camera image; a visit starts when a group reaches the viewing spot.
2. **Decide.** A Python service ([`orchestration/livebridge`](orchestration/livebridge))
   runs each visit: theme, tone (friendly for little kids, scary for everyone else), and
   what the narrators say.
3. **Lead.** The Scarecrow narrator board receives the visit over Wi-Fi and broadcasts every
   cue to the yard over **ESP-NOW** (device-to-device radio, no router in the path).
4. **Follow.** Every other prop listens to the Scarecrow and does one job: follow the cue.
   Duplicates are ignored, cues from anyone else are ignored *and counted*, and a prop that
   loses its leader finishes its track and returns to idle on its own.

## Same discipline, different domain

| How I run a fraud program | How it shows up in this build |
|---|---|
| **Own the architecture** | One leader, defined follower behavior, one message contract across 14 devices. Every part has a clear owner and a failure plan. |
| **Rules before models** | The AI gets one narrow job (headcount + a short costume description). Everything around it is pre-recorded and rule-driven. |
| **Screening and rejection** | Every AI phrase passes an interpretation layer, then a screen that rejects anything about bodies, faces, skin or protected traits. A rejected phrase is never "fixed up"; the visit falls back to a recorded line. See [`orchestration/vision/middle_pipeline.py`](orchestration/vision/middle_pipeline.py). |
| **Vendor spend controls** | Cooldown between calls, daily cap, hard lifetime cap that shuts AI off automatically, and a schedule limited to trick-or-treat hours. See [`orchestration/livebridge/middle_audio.py`](orchestration/livebridge/middle_audio.py). |
| **False-positive tuning** | Camera zones and motion masks were audited against real walk-throughs. Reported "skipped intros" on Halloween were investigated: 3 of 4 had actually played. |
| **No silent failures** | Every cue is logged with an outcome. Narrator-to-narrator handoffs use a confirm/ACK/timeout handshake. An audio module that doesn't confirm playback is logged immediately. |
| **Privacy by design** | No visitor images are saved. Visitor data stays local and is never published. |
| **Measure it, report it** | Every visit is written to a JSON event log; [`tools/show_report.py`](tools/show_report.py) turns a night into the numbers above. |
| **Root cause, then fix the class** | When something failed on a live night, it got a written incident review and a fix applied to every device with the same weakness, not just the one that broke. |

## What's in this repo

| Folder | What it is |
|---|---|
| [`orchestration/livebridge`](orchestration/livebridge) | The live show service: camera events in, visit decisions, AI insert, MQTT cues out |
| [`orchestration/showsim`](orchestration/showsim) | The show state machine, runnable as a simulator without hardware |
| [`orchestration/vision`](orchestration/vision) | The narrow AI call, interpretation layer and content screen |
| [`props/scarecrow-narrator-a1s`](props/scarecrow-narrator-a1s) | Show leader: ESP32-A1S audio board, Wi-Fi + ESP-NOW broadcaster |
| [`props/skeleton-narrator-a1s`](props/skeleton-narrator-a1s) | Second narrator, handshakes with the Scarecrow |
| `props/*` | One folder per follower prop: spatial-audio speakers, crows, projectors, DMX, IR lights, fog, relays |
| [`shared`](shared) | The common ESP-NOW message contract, follower logic, diagnostics and a hardened MP3 driver |
| [`docs/hardware-gotchas.md`](docs/hardware-gotchas.md) | Every hardware lesson I paid for, so you don't have to |

## Build it yourself

Firmware uses [PlatformIO](https://platformio.org/). Secrets are never in the code:

1. Copy `shared/Shared_Constants/wifi_credentials.h.template` to `wifi_credentials.h` and
   fill in your own Wi-Fi and MQTT details.
2. Copy `orchestration/livebridge/.env.template` to `.env` for the Python service.
3. Device MAC addresses in `shared/Shared_Constants/ProjectConstants.h` are zeroed; set
   your own boards' MACs.
4. All radios are locked to Wi-Fi channel 11. Change it in one place if your network differs.

Not included: voice clones, music, video and show audio (third-party or licensed material),
camera layouts and anything about the people who visited.

## License

MIT. See [LICENSE](LICENSE).
