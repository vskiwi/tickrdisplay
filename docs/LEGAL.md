# Legal notes

> **This is not legal advice.** It explains the position the project takes
> and why. If you plan to distribute TickrDisplay widely or commercially,
> consult a lawyer who knows the law of your jurisdiction.

TickrDisplay is an independent, alternative firmware for the TickrMeter
e-ink ticker. It was written from scratch after studying the hardware and
the observable behaviour of the stock firmware.

## Reverse engineering for interoperability

The stock firmware was inspected only to learn which GPIOs drive which
peripherals, how the flash is partitioned and how an image can be installed
without opening the case. That is interoperability research:

* In the **EU**, Directive 2009/24/EC permits observing, studying and
  testing a program to determine its underlying ideas and principles
  (Art. 5(3)) and decompilation where indispensable for the interoperability
  of an independently created program (Art. 6); contractual terms to the
  contrary are void (Art. 8).
* In the **US**, 17 U.S.C. § 1201(f) permits circumvention for the purpose
  of interoperability of an independently created program. In this case
  nothing was circumvented at all: the device has no flash encryption and no
  secure boot, the UART bootloader is the standard Espressif one, and the
  OTA upload page (`/update`) is a feature the vendor documents for its own
  customers.

The wording used in this repository is therefore *studied* and
*documented*, not *cracked*.

## What is and is not in this repository

* **No vendor code, binaries, flash dumps or string listings.** The
  vendor's recovery image is referenced by its public URL and a SHA-256
  hash only; how to take your own flash dump is described in
  [`HARDWARE.md`](HARDWARE.md) → *Stock firmware*. Contributors must never
  add such material (see [`../CONTRIBUTING.md`](../CONTRIBUTING.md)); a
  flash dump also contains your Wi-Fi credentials.
* **Own implementation.** Every line of firmware and web UI is the
  project's own work or comes from the open-source libraries listed in
  [`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md).
* **Interoperability facts only.** Pin assignments, the partition table,
  power thresholds, host names and endpoint paths of the stock firmware are
  recorded because they are needed to run and to return to the stock
  firmware. Facts, interfaces and version numbers are not protected by
  copyright.

## Trademark

*TickrMeter* is the vendor's brand. This project uses the name only to
identify the hardware the firmware runs on (nominative use): TickrDisplay
is an *alternative firmware for TickrMeter devices*, never *TickrMeter
firmware*. The project is **not affiliated with, endorsed by or supported
by** the vendor. The repository name, the set-up access point
(`TickrDisplay`) and the MQTT client id do not contain the mark; the
vendor's logo, product photos and boot bitmap are not used.

## Licence

TickrDisplay is licensed **GPL-3.0-or-later** (see [`../LICENSE`](../LICENSE)).
The firmware statically links [GxEPD2](https://github.com/ZinggJM/GxEPD2),
which is GPL-3.0; a compiled image is therefore a work based on GxEPD2 and
can only be distributed under GPL-3.0 terms with the corresponding source
available. Licensing the whole project the same way keeps one licence for
source and binaries. The other linked libraries (LGPL-2.1 / LGPL-3.0 /
MIT / Apache-2.0) are compatible; their notices and obligations are listed
in [`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md). Every
release is built from a tagged commit of this repository, which is the
corresponding source for the published images and contains both licence
files. The software is provided **as is** without warranty (GPL-3.0
§§ 15–16); the [README](../README.md) → *Disclaimer* spells out the
practical consequences of flashing.
