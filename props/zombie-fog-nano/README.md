# zombie-fog-nano

Standalone Arduino Nano (CH340 clone, **new bootloader** -> `pio run -e nano -t upload`)
that fires the Zombie Container fog machine through a relay on **D3**.

- **D4 = second relay for the fog machine's red light: ON from power-up, stays on.**
- Runs as soon as it gets power; no network, no trigger input.
- Loop: pause, fog burst, pause, fog burst... Pauses {9..14 s} and bursts
  {3..8 s, 8 s = hard max on} are each dealt from a Fisher-Yates-shuffled deck (reshuffled when
  empty), so bursts stay evenly spread: avg one burst per ~17 s, never
  closer than 9 s end-to-start. First fog 9-14 s after power-up.
  (v1 picked 2 of six 5 s slots per 30 s window; Joe found bursts clumped
  together with long dead pauses, so it was replaced 2026-09-30.)
- Relay board jumper = HIGH trigger (`RELAY_ACTIVE_LOW = false`); level set before `pinMode` so it
  never blips on at boot.
- Serial 115200 logs each burst: `wait N ms  fog N ms`.

Idea (Joe, 2026-09-30): power it from the red-alert light so it only fogs
while that light is on. Not decided/wired yet.

Bench 2026-09-30: flashed + verified; 60 s serial capture showed windows on a
30 s cadence, 2 bursts per window, 1.7-4.0 s long. Relay confirmed clicking by Joe after the D3 + high-trigger fix; fogger not yet tested.
