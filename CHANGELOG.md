# Changelog

What's been added to this repository, newest first.

## 2026-11-12: first public release

Published after the 2026 season closed, as a portfolio of how I approach systems and as
something the home haunt community can reuse.

**The project**
- The show code from the 2026 season: show logic, AI pipeline, and firmware for every prop,
  curated from my working repository with network details, device IDs and all visitor data removed.
- Season results in the README: detection, show and AI figures from the show's own logs, plus
  seven charts built from one aggregate data file ([`docs/charts/`](docs/charts/)).
- A diagram of one visit as a state machine.
- "AI in this project": what AI did in the live show, what helped build it, and the controls around each.

**Try it and reuse it**
- A simulated show night that runs without hardware: `python3 -m showsim.demo`.
- [`examples/first-follower`](examples/first-follower): the smallest prop that joins the cue
  network, plus a test sender for trying it with two boards.
- [Remote flashing over a Raspberry Pi](docs/remote-flashing-over-a-pi.md) and a safe flash
  script ([`tools/bridge_flash.sh`](tools/bridge_flash.sh)).
- Two standalone tools that came out of the build, both at 1.0:
  [projection-fit](https://github.com/joe-pero/projection-fit) and
  [show-power](https://github.com/joe-pero/show-power).

**What I learned**
- [17 incident reviews](docs/incident-reviews.md) from the season: root cause, fix, and the
  pattern behind them.
- [The garage display as a control panel](docs/control-dashboard.md).
- [Hardware gotchas](docs/hardware-gotchas.md).

**Quality and safety**
- Tests on every push: show logic, the AI content screen, the simulated night, and firmware
  builds for all props and the example (from a clean copy, with templates only).
- Secrets live only in gitignored files with committed templates; GitHub secret scanning and
  push protection are on.
- Fixed while preparing the release: the credentials template was missing the radio-channel
  setting, so a fresh clone couldn't build most props. It now builds out of the box.

## Next season

Open items are listed in the [roadmap](ROADMAP.md) and at the top of the
[incident reviews](docs/incident-reviews.md).
