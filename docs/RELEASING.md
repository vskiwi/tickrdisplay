# Releasing

Checklist for every release. Copy it into the release issue and tick the
boxes there. Commands assume the repository root; build environments and
version derivation are described in [`DEVELOPMENT.md`](DEVELOPMENT.md).

Releases are driven by annotated tags `vMAJOR.MINOR.PATCH`: bump MAJOR when
the JSON payload or MQTT topic contract changes incompatibly, MINOR for
features, PATCH for fixes. The firmware version string, the asset names and
the release page are all derived from the tag by CI
(`.github/workflows/build.yml`, `.gitlab-ci.yml`).

## Before tagging

- [ ] `main` is green (firmware build, size gate, native tests, cppcheck);
      working tree clean.
- [ ] `CHANGELOG.md`: move *Unreleased* into a new `## [X.Y.Z] – YYYY-MM-DD`
      section and update the compare links at the bottom.
- [ ] `THIRD_PARTY_LICENSES.md` matches the versions pinned in
      `tickr_display/platformio.ini` (`pio pkg list`).
- [ ] Documentation reflects the release: `README.md`, `docs/API.md` and
      `docs/HARDWARE.md` if behaviour, routes or pins changed.
- [ ] Build the release images locally from a clean checkout and pass the
      size gate:
      ```bash
      cd tickr_display
      FIRMWARE_VERSION=vX.Y.Z pio run -e tickr
      python scripts/check_size.py dist/tickrdisplay-vX.Y.Z.bin
      ```
- [ ] Host tests pass: `pio test -e native`.

## Hardware smoke test

Use the locally built `tickrdisplay-vX.Y.Z.bin` on at least one device;
details in [`FLASHING.md`](FLASHING.md).

- [ ] **Upgrade path**: flash from the previous TickrDisplay release via
      `/system` → *Firmware* (device A). Boots, joins Wi-Fi, one payload
      renders, `/api/status` reports the new version.
- [ ] **Update a second device from the first**: drawer → *Updates* →
      *Update all members* (device B), or `scripts/flash_ota.sh`. Boots and
      renders.
- [ ] **Fresh-install path**: from the stock firmware via its `/update`
      page. Set-up portal appears, Wi-Fi connects, the shelf loads.
- [ ] **Return to stock**: `/system` → *Firmware* → *Recovery* → *Boot other
      partition* on a device whose other slot still holds the stock image.
      Stock boots; then back to TickrDisplay through the stock `/update` page.
- [ ] Anything not exercised is listed in the release notes as untested.

## Tag and publish

- [ ] Tag and push:
      ```bash
      git tag -a vX.Y.Z -m "TickrDisplay vX.Y.Z"
      git push origin vX.Y.Z
      ```
- [ ] CI builds the tag and attaches `tickrdisplay-vX.Y.Z.bin` (stock 4 MB
      partition table, OTA-safe) to the GitHub release; the GitLab release
      links the job artifacts.
- [ ] Verify the downloaded asset against the local build:
      ```bash
      shasum -a 256 dist/tickrdisplay-vX.Y.Z.bin
      ```
      and add the SHA-256 line to the release notes.
- [ ] Release notes: the changelog section, tested hardware and what was not
      tested, the disclaimer paragraph, a link to `docs/FLASHING.md`. `LICENSE` and
      `THIRD_PARTY_LICENSES.md` are in the tagged source; the notes name the
      tag as the corresponding source.
- [ ] Mark the release as *pre-release* while the version is below 1.0.
