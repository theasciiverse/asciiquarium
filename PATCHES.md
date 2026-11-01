# Asciiverse patches

Asciiquarium 1.1 by Kirk Baucom (2013; first released 2005), from
<https://robobunny.com/projects/asciiquarium>, packaged for the
Asciiverse **as a C port**. The first commit is the original release,
unchanged; the original Perl script is still in the repo as
`asciiquarium`.

## Why a port

The original is a Perl script built on the Perl modules Term::Animation
and Curses. Shipping it would mean shipping Perl and both modules for
every platform. The port is one small C program with ncurses, a static
binary on Linux.

## What is the same

Everything you see, on purpose:

- **The art.** `tools/extract-art.pl` reads every picture and colour
  mask out of the original and lets Perl itself evaluate them, so
  `src/art.h` is byte for byte what the original draws (most of the art
  is Joan Stark's).
- **The behaviour.** `src/asciiquarium.c` follows the original sub by
  sub, on top of a small engine that does what Term::Animation 2.6 does:
  transparency (leading spaces and `?`), colour masks with bold for
  capitals, fractional speeds and frame rates, depth order, collisions,
  death callbacks and one frame per tenth of a second.
- The fish, bubbles, seaweed (which lives 8 to 12 minutes), castle,
  and the nine visitors: ship, whale, sea monster, big fish, shark (who
  eats fish), fishhook (which catches one), swan, ducks and dolphins.
- Keys: **q** quits, **p** pauses, **r** redraws. A resize rebuilds the
  aquarium, as in 1.1.
- The original's quirks are kept: the dolphins never switch to their
  second frame (their path gives frame 0.5), the shark's default colour
  is the later of the two it is given (cyan), and the fishhook's
  `trans_char` setting has no effect.

## What is different

- `-v` / `--version` prints the version; any other argument prints a
  short usage line. The original takes no arguments.
- Signals (Ctrl+C, termination, hang-up) restore the terminal and exit,
  as the original's signal handler does.
- Entities at the same depth are drawn in the order they were created;
  the original's order there came from Perl's hash ordering, which is
  random.

## Building

`sh asciiverse/build.sh` (needs a C compiler and ncurses; no Perl). To
regenerate the art from the original: `perl tools/extract-art.pl
asciiquarium > src/art.h`.
