---
name: Bug report
about: Something does not work as documented
title: "[bug] "
labels: bug
assignees: ''
---

<!--
Before filing: read README.md → Known limitations, and SECURITY.md if this is
a security issue (report those privately instead).
NEVER attach a flash dump or NVS contents – they contain your Wi-Fi password.
Redact MAC addresses and IPs you consider private.
-->

## What happened

## What you expected

## Steps to reproduce

1.
2.
3.

## Environment

- TickrDisplay version / commit:
- Build environment (`tickr` / `tickr_dev` / custom):
- How it was flashed (stock `/update` page / UART / TickrDisplay OTA):
- Power source when it happened (USB / battery):
- `esptool.py flash_id` output (flash size line only):
- Board revision / anything unusual on your PCB:

## Logs

<!-- Serial output at 115200 baud around the failure, or the HTTP response. -->

```text

```

## Payload / request that triggered it (if any)

```json

```
