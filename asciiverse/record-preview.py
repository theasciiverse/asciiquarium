#!/usr/bin/env python3
"""Records asciiverse/asciiquarium.cast, the catalog preview, at 100x30
(hub/RECORDING.md): SECONDS (default 40) of the aquarium, then q. The
visitors are random: run it a few times and keep a take where something
big comes by (a shark eating, the whale spouting, the sea monster).
asciiquarium runs in tmux and asciinema 3 records inside it.
Usage, from the repo root after asciiverse/build.sh and stage.sh:
    python3 asciiverse/record-preview.py"""
import os, subprocess, time

s = "asciiquarium-preview"
secs = float(os.environ.get("SECONDS", 40))
subprocess.run(["tmux", "kill-session", "-t", s], stderr=subprocess.DEVNULL)
subprocess.run(["tmux", "new-session", "-d", "-s", s, "-x", "100", "-y", "30",
    "TERM=xterm-256color COLORTERM=truecolor LANG=C.UTF-8 "
    "asciinema rec --overwrite -q -i 2 --window-size 100x30 "
    "--capture-env TERM,COLORTERM,LANG -c ./stage/asciiquarium "
    "asciiverse/asciiquarium.cast"])
time.sleep(secs)
subprocess.run(["tmux", "send-keys", "-t", s, "q"])
time.sleep(1.5)
subprocess.run(["tmux", "kill-session", "-t", s], stderr=subprocess.DEVNULL)
