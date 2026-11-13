"""
Harvest of Shadows — show-logic simulator / orchestration skeleton.

This package is the no-cost, no-hardware proof of the show sequencing logic
(theme lock, cooldown, tone rules, sequential narrator handoff). It is meant to
grow into the real orchestration layer: the trigger source (`showsim_cli`) is a
stand-in for Frigate zone events, and the narrator output is a stand-in for the
A1S nodes + a vision/TTS pipeline. The state machine, deck, and constants are
the parts intended to stay.

Provenance of the ported logic:
  - constants.ShowMode / ShowTheme  ← lib/Shared_Constants/ProjectConstants.h
  - theme_deck.ThemeDeck.next()     ← nodes/main_switch_controller/src/main.cpp  nextTheme()
  (both in ~/Documents/Halloween-Dev/PlatformIO/Master Halloween Show 2026/)
"""
