# Contributing to TickrDisplay

Thanks for your interest. This is a small hobby project; the rules are short.

## Before you start

* Read the [Disclaimer](README.md#disclaimer). You are working on firmware for
  a device you can brick.
* Search existing issues. For anything larger than a bug fix, open an issue
  first so we can agree on the approach.
* **Never commit vendor firmware, flash dumps or `strings` output of them.**
  See [`docs/LEGAL.md`](docs/LEGAL.md) and [`docs/HARDWARE.md`](docs/HARDWARE.md) → *Stock
  firmware*. Flash dumps contain your Wi-Fi credentials.

## Development setup

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for toolchain, build
environments, native tests and CI.

## Pull requests

* One topic per PR, small and reviewable.
* Target `main`. Rebase on the latest `main` before requesting review.
* Describe **what you tested on real hardware** and what you did not. State
  which board/flash size you have.
* Keep the existing code style (4-space indent, `snake_case` for C functions,
  `PascalCase` for classes, HAL / managers / logic layering under
  `tickr_display/src/`).
* Update `README.md`/`docs/API.md`/`docs/HARDWARE.md`/`CHANGELOG.md` when
  behaviour, pins or the API change. Add an entry under *Unreleased* in the
  changelog. Bench addresses, tokens and other private data never go into
  the docs – use neutral placeholders (`192.0.2.40`, `<device-ip>`,
  `tickr-A1B2C3`).
* Hardware scenarios that cannot be covered by host tests are listed in
  [`docs/MANUAL_TESTS.md`](docs/MANUAL_TESTS.md); releases follow
  [`docs/RELEASING.md`](docs/RELEASING.md).
* Commit messages in English, imperative mood ("Add MQTT auth", not "Added").
* Do not add dependencies without discussing licence compatibility – the
  project is GPL-3.0-or-later and everything linked into the image must be
  compatible. Update `THIRD_PARTY_LICENSES.md` when adding or bumping a
  library.

## Licensing of contributions

By submitting a pull request you agree that your contribution is licensed
under the project licence (GPL-3.0-or-later). You keep your copyright. No CLA.

## Reporting hardware findings

Different board revisions may exist. If your pin-out, flash size or partition
table differs from `docs/HARDWARE.md`, open an issue with `esptool.py flash_id`
output, photos of the PCB (your own) and what you measured – but **redact
MAC addresses and never attach a flash dump**.
