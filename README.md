# jctool WinUI 3

Modern WinUI 3 frontend and native HID bridge for Joy-Con and Pro Controller
tools. This repository is an independent community modification of
[CTCaer's Joy-Con Toolkit](https://github.com/CTCaer/jc_toolkit), produced with
the assistance of AI coding agents. It is not an official Nintendo or CTCaer
release.

The original MIT license and copyright notice are preserved in `LICENSE`.

## Included

- WinUI 3 frontend with English and Simplified Chinese resources.
- Up to four stable controller slots.
- Colors, protected SPI access, calibration, verified 512 KiB backups and
  same-controller restore checks.
- Continuous input, button visualization, IR camera, NFC/NTAG reading and HD
  Rumble including `.bnvib` conversion.
- Hardware-safe Debug/Custom/Internal command allowlists.
- Isolated Joy-Con simulator support for development and automated checks.
- Release x86 self-contained build published as a GitHub Release asset.

## Build

Build the solution with Visual Studio Build Tools and the Windows 10/11 SDK:

```text
MSBuild jctool.vs2017-net4.7.1.sln /t:JcTool_WinUI /p:Configuration=Release /p:Platform=x86
```

The WinUI project targets .NET 8 for Windows and Windows App SDK 1.7. The
native bridge is built for x86. The source tree intentionally keeps the
original `jctool` directory because the WinUI and native projects reuse its
HID API and simulator sources.

The self-contained package includes the .NET and Windows App SDK runtime. It
still requires the Microsoft Visual C++ x86 Redistributable for the native
HID bridge.

## Hardware preflight

Run this before any write operation. It checks controller input, SPI read,
safe diagnostic traffic, native allowlist rejection, IR start/stop and NFC
cancellation. It does not write SPI or change pairing state.

```text
JcTool.WinUI.exe --hardware-preflight
```

The same check is available from the Diagnostics page.

## Safety

Create and keep a verified SPI backup before changing controller data. Normal
SPI writes are restricted to known configuration/calibration ranges and are
read back for verification. A multi-block restore still has physical power
and cable interruption risk. Do not disconnect the controller or terminate
the program during a restore. Internal shipment, pairing-clear and reboot
commands are exposed only as separately confirmed actions.

## Provenance

- Original project: https://github.com/CTCaer/jc_toolkit
- Protocol references are listed in the original project documentation.
- The WinUI migration status is tracked in `JCTOOL_WINUI_MIGRATION_STATUS.md`.
