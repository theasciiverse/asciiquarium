# asciiquarium (Asciiverse C port)

A C + ncurses port of Kirk Baucom's asciiquarium 1.1 (Perl, built on his
Term::Animation module). The first commit (tag `1.1`) is the original
release; the Perl script stays in the repo as `asciiquarium`, the
reference for the port. Goal: the same art and behaviour, exactly. Don't
add features or change the look; list any difference in PATCHES.md.

- Art: `perl tools/extract-art.pl asciiquarium > src/art.h`. Never edit
  art.h by hand; Perl evaluates the q{} literals (backslashes!).
- `src/asciiquarium.c`: an engine mirroring Term::Animation 2.6
  (Animation.pm and Entity.pm, CPAN Term-Animation-2.6), then the
  original's subs in the same order and with the same names.
- Build: `sh asciiverse/build.sh` -> `asciiquarium-bin` (the name
  `asciiquarium` is the Perl original); `sh asciiverse/stage.sh`.
- Testing one visitor: `cc -DAQ_TEST_OBJECT=n` (0 ship, 1 whale,
  2 monster, 3 big fish, 4 shark, 5 fishhook, 6 swan, 7 ducks,
  8 dolphins) makes random_object always pick it. Never in releases.
- Preview: `python3 asciiverse/record-preview.py`; keep a take where
  something big comes by.
