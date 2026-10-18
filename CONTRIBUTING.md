# Contributing

Thank you for helping. A few rules keep the player what it is:

- **The music stays as it came.** A change that alters samples on purpose (a filter, a volume, a resampler) does not
  belong in the free player; one that a device needs to play at all does, and says so in `bitPerfectWhy`.
- **Run the tests** before a pull request: `./build.sh test`, and on Linux `sudo sh tests/player-intact.sh` (no sound
  card is touched: a file stands for the DAC, a fake Roon in its own network namespace).
- **audiolaz Link** changes keep both protocol versions working (docs/LINK-PROTOCOL.md, Compatibility); a change of the
  wire format updates that page in the same pull request.
- C11 (gnu11), `-Wall -Wextra` clean on Linux (gcc) and macOS (clang); no new dependency without a word first.
- The kernel module (`src/linux/kmod/`) is GPL-2.0: contributions there are under that licence; elsewhere MIT.

This repository is published from audiolaz's own tree; pull requests are read and taken in by hand, credited in the
commit. Issues and questions are welcome here, or at contact@audiolaz.com.
