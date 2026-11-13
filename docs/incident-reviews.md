# Incident reviews: the 2026 season

Everything here broke at least once. I kept a dated log every session, and every failure that
reached the show (or nearly did) got a root cause, a fix, and a change that stops the same
*class* of failure, not just the one instance.

The reviews are blameless and honest, including the mistakes made while fixing things. Some of
those were made by the AI coding assistant I worked with; they're listed the same way as mine,
because the lesson is the same either way.

## At a glance

| # | Date | What broke | Root cause | Class |
|---|---|---|---|---|
| 1 | Sep 23 | Narrator intros quietly got longer | An old copy of a generator script reused a stale setting | Process |
| 2 | Sep 23 | The workshop PC hung mid-render | Three voice models in memory at once; auto-restart made it worse | Capacity |
| 3 | Sep 24 | Dashboard "AI Show" switch did nothing | A library update changed a callback; its thread died silently | Software |
| 4 | Sep 24 | Voice rendering stalled twice (~50 min) | A "wait for it to finish" check matched its own command | Process (AI) |
| 5 | Sep 25 | Scarecrow silent during a live walk-through | Three bugs: no Wi-Fi retry, short keepalive, oversized heartbeat dropped silently | Firmware |
| 6 | Sep 26 | Theme music cut out 30 s in | Counter wrap-around made the MP3 driver think the module had died | Firmware |
| 7 | Sep 27 | One sidewalk never triggered a show | A camera motion mask hid people's upper bodies | Configuration |
| 8 | Sep 28 | Scarecrow silent after one show, with visitors | Live service ran on the simulator's clock | Software + process |
| 9 | Sep 29 | Spooky Tree silent through power cycles | Thin wire to the amplifier; voltage sag | Power |
| 10 | Sep 30 | DMX stage light ignored every command | Module pins labeled from the other side; vendor tutorial wrong | Hardware |
| 11 | Sep 30 | Media player boot-looped | A variable and a function differed only by capital letters | Software |
| 12 | Oct 2 | A narrator line ended in gibberish | A phrase that makes the voice model babble; QA couldn't hear it | Content QA |
| 13 | Oct 20 | Yard controllers dropping off Wi-Fi | An access-point feature known to upset small IoT radios | Network |
| 14 | Oct 27 | A prop wouldn't boot after a "verified" flash | Firmware file zero-filled by a power cut after copying | Process |
| 15 | Oct 27 | Zombie Container dark on show night | A smart plug that boots OFF behind another switch | Power control |
| 16 | Oct 28 | A controller board died | Cut USB cable with reversed, unmarked wire colors | Hardware |
| 17 | Nov 5 | Season over, lights came back on by themselves | Board reboots default to ON; the OFF command wasn't remembered | Firmware design |

Plus one still open: on **Oct 30** one camera saw no one for 23 hours with its video stream up,
so that night logged zero visits. The cause isn't known yet; it's on next season's list.

---

## 1. Narrator intros quietly got longer (Sep 23)

**What happened:** While fixing an unrelated wording bug, I regenerated the intro scripts and
the intros grew from an approved 30-35 seconds back to an old 40-45 second length.
**Root cause:** two scripts both claimed to be "the intro generator". The older one still held
a setting from a design that had been rolled back.
**Fix:** retired the old script entirely, one source of truth.
**Lesson:** a duplicate tool is a regression waiting to happen. Delete it, don't just stop using it.

## 2. The workshop PC hung mid-render (Sep 23)

**What happened:** the PC froze and needed a hard power-off.
**Root cause:** the kernel log showed an out-of-memory event: the live show service's voice
model, two render jobs' voice models and the usual desktop apps, all at once. The system killed
the show service, its auto-restart loaded the model again five seconds later, and the machine
thrashed for 20 minutes. An earlier "stop the show service before rendering" fix lived in one
script only, and the jobs that day used a different path.
**Fix:** a lock and a free-memory check *inside the shared voice loader*, so every caller is
covered. A second loader is refused (it names who holds the lock), as is a load when memory is
low. One wrapper script for all rendering.
**Lesson:** put the safety check where every path goes through it, not in one of the paths.

## 3. The dashboard switch did nothing (Sep 24)

**What happened:** the dashboard's AI readout stopped updating. Digging in showed its MQTT
connection had been dead since the first network hiccup, possibly for days, while the dashboard
still reported "connected". The AI Show switch's messages were probably never sent.
**Root cause:** a messaging-library update changed a callback's arguments. The old handler
threw an error inside the library's background thread, which died quietly.
**Fix:** corrected the handler. The switch now sends with delivery confirmation and returns an
error unless the broker confirms. The tile shows "SWITCH FAILED" or "dashboard lost MQTT"
instead of stale data.
**Lesson:** "connected" is a claim; data arriving is evidence.

## 4. Rendering stalled twice (Sep 24), an AI assistant's mistake

**What happened:** two overnight render queues sat idle for about 7 and then 45 minutes.
**Root cause:** the assistant's "wait until the other job finishes" loop searched running
processes by text, and the search text was in its own command line. It waited on itself
forever, and it also blocked the real queue.
**Fix:** wait on process IDs only, never on text matches. Recorded as a standing rule.
**Lesson:** a monitoring check must never be able to see itself.

## 5. Scarecrow silent during a live walk-through (Sep 25)

**What happened:** I walked the yard with everything armed; nothing played.
**Root cause, three bugs:**
- If Wi-Fi failed at boot, the board never tried again.
- The broker's keepalive (15 s) was shorter than an intro (20 s), and the board doesn't read
  the network while speaking, so it was dropped during every intro.
- A heartbeat message grew past its 256-byte buffer after new features were added, so *every*
  heartbeat was silently thrown away and the show service paused cues as "leader offline".

**Fix:** non-blocking Wi-Fi retry, a 90 s keepalive, a bigger buffer, and **a loud error when a
message doesn't fit**.
**Also wrong that day:** the troubleshooting blamed the other narrator's SD card (already
replaced), trusted a stale "online" message as proof of life, and reset the board by opening its
serial port. That led to a health check that reads real liveness.

## 6. Theme music cut out 30 seconds in (Sep 26)

**Root cause, three bugs in the shared MP3 driver:** a counter wrap-around made the driver
decide the module had gone silent and reset it mid-track; a serial framing error was treated as
"file not found" and killed a theme at 1:18; and an unsigned elapsed-time calculation made every
visit time out instantly.
**Fix:** the driver never resets a module mid-track on silence alone, error codes are read
correctly, and host-side tests now cover the timing math. The fix landed once, in the shared
driver, so every prop that uses it got it.

## 7. One sidewalk never started a show (Sep 27)

**Root cause:** the camera's street motion mask covered the upper half of anyone walking on
that sidewalk, so the detector never started tracking them. Found with a walk-audit tool that
replays a real walk against the zones without calling the AI.
**Fix:** redrew the mask; a daylight walk confirmed the sidewalk now triggers.
**Lesson:** test detection with real walks, not just the zone drawing.

## 8. Silent after one show, with real visitors (Sep 28)

**Root cause:** the show's state machine can run in a simulator, where time only moves when
the simulation says so. The live service started it with **the simulator's clock**, so time froze
after the first show. Every later arrival looked like "the same group, 8 seconds later".
**Found by:** every log line carried the same two timestamps all evening.
**Made worse by (the AI assistant):** a serial self-test during show time (it reboots the
narrator), a service restart on a guess (which switches the paid AI off), a fake camera event
injected while visitors were present, and calling the frozen 8.0 s "normal".
**Fix:** a real clock for live mode with tests that catch a regression, a watchdog, a 4-minute
visit limit, per-cue play confirmation and a liveness health check. Plus operating rules for
anyone, human or AI: while a show switch is on, no serial, no restarts and no fake events
without the owner's OK.
**Verified:** a live walk the same evening, 8 of 8 cues played.

## 9. Spooky Tree silent through power cycles (Sep 29, and again in Oct)

**Root cause: power.** About 10 feet of thin wire to a 3-5 A amplifier lost about a volt; the
prop browned out. Confusingly, on the bench the USB bridge powered the controller and MP3
module (which reported "playing") while the amplifier had no power at all.
**Fix:** heavier wire, brown-out reason logged at boot, and a loop watchdog. When it recurred
weeks later, the pattern (always fine on USB, failing on its own supply) pointed at the
supply, and a loose data wire inside the sealed housing turned up too. The controller moved to
its own USB charger.
**Lesson:** when software says "playing" and the speaker is silent, measure the power.

## 10. The DMX light ignored everything (Sep 30), about 2 hours

**Root cause:** the RS-485 module's TX/RX labels are named from the *microcontroller's* side.
The vendor's own tutorial wires it backwards. Its LED blinked with the signal, so the wiring
*looked* right.
**Fix and lesson:** before debugging protocol, polarity or code, prove a module's pinout with
a two-minute static test: drive the pin high and low, and meter the output. Now the first step
for any new breakout board.

## 11. A media player boot-looped (Sep 30)

**Root cause:** the player's scripting language ignores capitalization, so a variable `txt`
collided with a function `Txt()`. The script never started, and nothing was logged.
**Fix:** renamed, and a static checker now runs before every card build. The Python
simulator couldn't have caught it, because the bug only exists in the player's language.

## 12. A narrator line ended in gibberish (Oct 2)

**What happened:** I heard a Scarecrow line trail off into babble.
**Root cause:** the earlier quality check only identified the *language* of each clip, so a
correct English sentence followed by babble passed. One phrase ("…still deciding what it
is…") made the voice model babble **every time** (19 of 19 intros).
**Fix:** a full transcript check: speech-to-text on every clip, aligned to its script, with the
tail checked separately. Scanned **2,486 clips**; about 50 real defects were re-rendered (51 of
51 clean) or swapped, and writing rules were added for phrasings the model handles badly.
**Lesson:** check every item, not a sample, when the failure is rare and audible.

## 13. Yard controllers dropping off Wi-Fi (Oct 20)

**Evidence:** home automation history showed one yard controller dropping **65 times in 4.5
hours** and another every 30-60 seconds, while a third on the same broker never dropped. So the
broker was fine, and the problem was at the radio level.
**Root cause (probable):** the front-yard access point had a Wi-Fi 6 feature on its 2.4 GHz
band that's a known cause of drops for small IoT radios. The channel was correct.
**Change:** turned that one feature off (logged, reversible), with a defined next step if drops
continue. Halloween still saw brief narrator dropouts, so this remains an open item.
**Lesson:** compare devices against each other before blaming any one of them.

## 14. "Verified" flash, dead prop (Oct 27)

**Root cause:** the USB bridge Pi lost power three minutes after a firmware copy, and the file
on its card was all zeros. The flash tool flashed the zeros and still said "hash verified",
because it checks chip-against-file, not file-against-build.
**Fix:** `sync` after copying, and compare checksums on the Pi before flashing.
[`tools/bridge_flash.sh`](../tools/bridge_flash.sh) refuses to flash otherwise.

## 15. Zombie Container dark on show night (Oct 27)

**Root cause:** its smart plug sits behind another switch (an extension cord) and **boots OFF**
whenever that cord powers it. Nothing turned it on afterward.
**Fix:** the power-up now waits for that plug to come online, turns it on, confirms it's on,
and includes it in the "didn't come on" alert. Now a built-in option in
[show-power](https://github.com/joe-pero/show-power).

## 16. A controller board died (Oct 28)

**Root cause:** a cut USB cable with only red and white wires. Red was assumed to be +5 V. It
wasn't, and the reversed supply killed the board.
**Fix and lesson:** meter cut cables before use, or better, don't cut them: power a board
through an intact cable into its own USB port. Mark known-bad cables.

## 17. The season was over, and the lights came back on (Nov 5)

**Root cause:** the eave light controller is designed to fail safe to **ON** at boot (lights on
is the safe state during the season). After the season, its OFF command came from home
automation as a one-time message, so each time the board rebooted, the lights came back on.
**Fix:** a *retained* OFF message on the broker, which the board reads on every reconnect. Noted
for next season: clear it first, or ON won't stick.
**Lesson:** a fail-safe default needs an off-season state too.

---

## The patterns

Looking across all seventeen:

- **Silent failures were the most expensive kind.** A dead thread, a dropped heartbeat, a
  "hash verified" on a zero-filled file. The fix was always the same: make the failure loud.
- **Fix the class, not the instance.** Safety checks went into the one shared path (the voice
  loader, the MP3 driver, the flash script) so every caller got them.
- **Power and wiring caused more "software" bugs than software did.** Measure first.
- **Production needs a change freeze.** The worst night (#8) was made worse by well-meant changes
  during the show. Rules now apply to everyone, human or AI.
- **Write it down the same day.** Every one of these reviews was rebuilt from that day's log
  entry. Without the log, most would be "it was flaky for a while".
