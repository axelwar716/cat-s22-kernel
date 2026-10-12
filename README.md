# CAT S22 Flip — kernel `stock-stable-4`

**Coded and vibe coded bruh. Won't support anything that isn't a main issue. Cuz i'm dev with custom android, not basic GSI support. Anything in stock sucks**

A **stability-focused** kernel for the CAT S22 Flip (firmware v30, `4.9.227-perf`), shipped as a single boot image with Magisk 27.0 and the TT9 S22 keyboard.

## Scope: stable first

The only goal of this build is to be **stable**: fix the keypad and the other basic things (was complex asf) that are broken or missing on stock, and nothing more. It stays as close to stock as possible: same kernel version, the stock config and a Clang 10 compiler like stock (small differences listed [below](#kernel-base)), the stock DTB, header and command line, and CAT's own signed vendor modules loading untouched.

I run a more advanced kernel and vendor on my own phone. This one is not that: it is the conservative subset, published so everyone can have a better experience on the S22 while staying relatively safe.

> **Not tested:** IMS / VoLTE / VoWiFi. I haven't tried them with this kernel, so I can't say whether they work. Reports and easy debug steps: [#2](../../issues/2).
> **Tested on a GSI:** Android 16 from TrebleDroid works with this same kernel, even camera and the 2nd screen clock (see [On a GSI](#on-a-gsi)). On the stock v30 vendor it needs one prop fix or it hangs. On Android 15+ the outer-screen clock shows the wrong local time ([#1](../../issues/1)).
> **Not tested on CAT's stock Android 11 system.** My daily setup is my own LineageOS 20 build with the stock v30 vendor.

## Downloads

From [Releases → `stock-stable-4`](../../releases/tag/stock-stable-4):

| File | What it is |
|---|---|
| `boot-s22-stock-stable-4-tt9en.img` | Kernel + Magisk 27.0 + TT9 S22 (English) — **the image to flash** |
| `TT9S22-en.apk` | The same TT9 S22 keyboard on its own, to reinstall it after uninstalling |
| `SHA256SUMS` | Checksums |

```
bfb84f2d71a4f804bd03968451f70b33deb6090c54180d083ee65cb2efc26801  boot-s22-stock-stable-4-tt9en.img
95481d05f6d9ce87a01629fce0ef15ce8724eaf7e4c3e7ab8743f38d94f546ab  TT9S22-en.apk
```

Don't want TT9? Uninstall it like any app: it won't be reinstalled on the next boot.

## Install

Only `boot` is replaced: **no wipe**, your ROM and data stay as they are. One image for every ROM. Needs an **unlocked bootloader**.

1. **Back up your current `boot.img` first.**
2. Flash:
   ```
   adb reboot bootloader
   fastboot flash boot boot-s22-stock-stable-4-tt9en.img
   fastboot reboot
   ```
3. If it doesn't boot, or recovery says "data may be corrupt", **don't factory reset**. Flash your backup back: `fastboot flash boot <your saved boot.img>`.

---

## What changed compared to stock

### Kernel base

CAT never released the kernel source for the S22 Flip, so it was rebuilt to match stock:

- Base: Qualcomm 4.9 tree `LA.UM.9.8.c26` (`msm89x7-mainline/msm-4.9` at `c348797f`). `uname -r` is still `4.9.227-perf`: version and `-perf` come from the tree and the stock config, not set by hand.
- Config `s22flip_defconfig`, derived from the stock `/proc/config.gz`. Differences:
  - `SYSTEM_TRUSTED_KEYS` points at CAT's module-signing certificate (below).
  - The OEM drivers CAT didn't publish are provided by this tree (all built in, like stock). `LEDS_MSM_GPIO_KEYBOARD_LIGHT` is on: the stock config lists it as off, but the stock kernel contains the driver.
  - Off because the code doesn't exist in the public tree or the part isn't fitted: Goodix GT1X (`GTP_*`) and 3 Cypress CYTTSP5 loader options. Touch firmware online update stays off.
- Compiler: AOSP Clang 10.0.6 (`r377782d`). Stock was built with a non-public Clang 10.0.7; only a Clang build reproduces the stock module CRCs. The Snapdragon-LLVM-only warning flags in the `Makefile`s are passed only when the compiler supports them, so the tree builds with AOSP Clang.
- Stock v30 DTB, boot header and command line. All drivers built in.
- **The 25 stock vendor modules resolve all 730 of their imports with matching CRCs: 666 from the kernel and 64 between the modules themselves.**
- Outside drivers, only two core lines change: `kernel/time/time.c` records when Android last set the time zone (used by the outer-screen clock's DST), and `ACC_IBAT_ROWS` (battery, below).
- CAT's module-signing certificate (RSA-4096, SHA-512) was extracted from the stock kernel and is in the keyring, so CAT's **25 signed audio and Wi‑Fi modules load untouched with signature enforcement on.**
- 8 audio exports the stock modules need (FSA4480, AW8155, AW883xx) were reimplemented, with signatures recovered from the stock CRCs, so the stock audio modules link.

### Keypad (`matrix_keypad` rewrite)

**The problem on stock:** the kernel read the keys once, ~10 ms after the first edge (HZ=100 tick + workqueue lag), while the contacts bounce for 9–18 ms. Result: doubled letters, and light taps that never register (worst on the 1/4/7/* row).

- Keys are sampled on the edge by a `SCHED_FIFO` thread. An hrtimer re-samples every 1 ms until the contacts settle, and every 5 ms while a key is held.
- A press is 6 ms of cumulative closure per burst; gaps under 30 ms don't split it; bursts under 6 ms are noise. A release is 30 ms fully open.
- **Electrical change to the key rows, for better sensitivity.** Stock idles the rows low through the SoC pull-down. A light press on a worn dome has high contact resistance, forms a divider with that pull-down and never crosses the input threshold → no IRQ, lost tap. Now the pull-down is off (written straight to TLMM `GPIO_CFG`) and each row is actively driven low for 1 µs before every read and before the IRQ is re-armed → light taps register, no ghost presses. The pull-down is restored in suspend so a floating row can't wake the phone.
- Rows are re-read after the IRQ is re-armed (the pinctrl-msm dual-edge emulation can miss an edge).
- Idle = IRQ only, no polling.
- The vendor driver's row "discharge" that gpiolib always refused is gone, and with it the log spam ("tried to set a GPIO tied to an IRQ as output", 3,565 lines per 2 s of typing).
- Works with the stock DTBO (no DT change needed); the vendor matrix read (double read per row, discard of all-columns samples) is kept.
- Tunable in `/sys/module/matrix_keypad/parameters/`: `press_ms`, `release_ms`, `row_pull` (0 none · 1 stock pull-down · 2 keeper), `fall_us`, plus the counters `rejected_taps`, `rearm_rescans`, `follow_fail`, `irq_no_key`.
- **Tested on the phone:** 248 fast presses on 4/7 → 0 doubled or extra letters, 0 row IRQs while idle.
- **Desk test:** ~490 real presses captured on the phone, each replayed in 62 phase/latency/jitter variants → 0 lost, 0 ghosts.

### Lid (Hall sensor)

- On stock, the lid sent key events that Android dropped, so **Android never knew whether the flip was open or closed.**
- Now a real `SW_LID` is reported with the standard Linux/AOSP meaning (1 = closed) on an input device called `s22_lid`, so an AOSP framework or a GSI reads it with no patch. The new name also keeps an old framework patch that inverted `hall_sensor` (used in my LineageOS build) from inverting it twice → **the same image for every ROM.**
- The `switch_hall` / `hall_switch` switch (1 = open) and `/dev/input/hall_dev` are kept for cover-screen apps (like S22Cover, my cover app, not part of this release). Scripts that looked for the input by the name `hall_sensor` should look for the phys `/dev/input/hall_dev` instead.
- Stock's 300 ms busy-wait inside the IRQ handler → delayed work.

### Outer screen (128×128 SPI ST7789V2, `fb2`, `s22_kclock`)

- **Lid closed:** the kernel owns the outer screen and draws a 24 h clock, the date ("Mon 12 Oct") and the battery ("85%", "85% +" when charging/full), styled like S22Cover (same Roboto metrics, light-grey text). Redrawn every minute by a wake alarm, also in deep sleep. **No app needed.**
- Kernel time zone with EU/US DST until Android sets the zone. **Android 15+ never sets it**, so there the clock shows UTC (+ the DST hour), not local time: see [#1](../../issues/1).
- Backlight on for 8 s on lid close, volume or orange key.
- **Lid open:** the outer screen is held off (no power vote, backlight 0) → no mirroring. Android frames are accepted but never sent over SPI.
- S22Cover-style apps take it over cleanly by writing `s22_kclock/config` (their last frame + backlight). `s22_kclock.autonomous=0` disables the kernel clock.
- With a cover app in control, the outer-screen backlight follows `lcd-backlight2` (not the lid), like stock.
- Tunable in `/sys/module/s22_kclock/parameters/`: `autonomous`, `auto_h24` (24/12 h), `auto_bl_secs` (0 never, <0 always), `dst` (-1 auto · 0 off · 1 EU · 2 US), `config` (cover-app protocol). Also `mdss_spi_panel.s22_cover_stock_off` (on by default: lid-closed panel power-off like stock) and the counter `mdss_spi_display.s22_frames_dropped` (frames dropped by the SPI queue fix).
- **Measured** (my LineageOS 20 build, stock vendor): 35.7 h with the lid closed, on battery → ~10.6 mA average, 0 SPI errors.
- Stock bugs fixed in the SPI panel driver: mmap bug, RGBA8888 → RGB565 conversion, pluggable "dtv" panel for the stock compositor, no waiting for a TE pulse the panel never sends; `lcd-backlight2` writes now reach the outer screen; hang after closing the lid (corrupted SPI queue); lid-closed power-off matched to stock.

### Drivers CAT didn't publish

Ported from public GPL kernels with the same parts, or written for this phone:

- **Touchscreen** Chipsemi CHSC5448 (`chsc5xxx` v3.6.2, from the HelloVolla GPL kernel): all 84 stock symbols, 5 points, glove mode, ESD heartbeat; ignores the outer screen's framebuffer like stock. **The touch firmware is never reflashed** (boot-time and online update off; the only firmware bytes in the source, `fw_code_bin.h` from the reference driver, are for a different chip, CHSC5472, and are compiled out). Differences from stock: the `vdd`/`vio` supplies are enabled at probe (stock relied on other drivers), `/proc/touchscreen` nodes are 0666 instead of 0777, and the log is no longer at emergency level.
- **Speaker amp** AW881xx v1.9.0 (AWINIC driver, unchanged import + S22 commits): stock ACF tuning selected by chip name (`aw88194` / `aw88194A`) like stock V1.8.0.2 (otherwise it stays silent); the first start reloads with I2S running and reads the DSP PID (fixes "dsp check fail"); calibration in `persist/aw_cali.bin`; hooked to the stock audio machine driver through `aw881xx_record_dai()`.
- **USB‑C** `wusb3801` (WillSemi reference driver from the Crosscall Core-X4 GPL kernel, aligned with stock: same sysfs, reports the analog audio adapter to the `usb` supply) + **FSA4480** (CodeAurora msm-4.14 driver, plus the two CAT exports the stock audio modules call; probe returns "absent" if the chip doesn't answer): 3/4-pole headphone detection over USB‑C.
- **AW8155** hearing-aid coil amp (modes set by EN pulses). **AW883xx:** handshake only (this unit has an AW88194).
- **Hall sensor** `hall_sensor` (Qualcomm driver from the Nokia 6300 4G / Tinno GPL kernels, aligned with stock): see [Lid](#lid-hall-sensor).
- **Keypad backlight** (recovered from the stock disassembly): on only when the lid is open and the main screen is on (`follow_lid`; `follow_lid=N` leaves it to userspace).
- **AOSP 3.18 `switch` class**, needed by the Hall driver; the only change is the class name `switch_hall`, as on stock.
- **Battery:** `ACC_IBAT_ROWS` 4 → 6 to match the Q2805 cell profile.
- **Not ported:** Goodix GT1X (not fitted on this phone).

### TT9 S22 keyboard

A fork of [TT9](https://github.com/sspanak/tt9) v64.0 (English only, 3.8 MB, package `io.github.sspanak.tt9.s22`, versionCode 1652), tuned for this keypad. It coexists with the regular TT9.

- TT9's own key debounce set to 0: it trusts the kernel's.
- In-memory ABC multitap; the 800 ms multitap timer runs from your last real press.
- IME threads keep foreground priority while the keyboard is hidden.
- **Needs this kernel:** on the stock kernel the bouncing comes back.
- **Using another keyboard app?** The kernel already debounces the keypad. Set the app's own key debounce ("accidental key repeat protection") to **0 ms / off**. If the app doesn't let you, its debounce stacks on top of the kernel's: fast presses of the same key get dropped, which undoes part of the kernel fixes and makes typing worse than with TT9 S22. (Regular TT9: *Accidental Key Repeat Protection* → *Off*.)

How it gets installed (see [`ramdisk/overlay.d`](ramdisk/overlay.d), nothing is written to `system` or `vendor`): Magisk's `overlay.d` runs a one-shot script after boot. If TT9 S22 is missing or older, it installs it and, on a fresh install, sets it as the keyboard. It then writes a marker in `/data/adb`, so **if you uninstall it, it stays gone**, and deletes the bundled APK from Magisk's tmpfs so it doesn't stay in RAM. On later boots it does nothing. Log: `logcat -s tt9-s22`. The copies here only add a license header comment; otherwise they are identical to the ones in the image.

### Root

Magisk 27.0 is in the image: root from the first boot. Open the Magisk stub once with internet to get the full app, or install the [Magisk 27.0 APK](https://github.com/topjohnwu/Magisk/releases/tag/v27.0).

---

## Status

- Running on my phone since 11 Oct 2026: my own LineageOS 20 build (not public) on the stock v30 vendor, flashed without wipe. Lid close/open and the outer-screen clock checked on the device; the keypad driver is the one I've typed on every day since 3 Oct. TT9 installs on the first boot and does nothing on the second.
- 12 Oct 2026: same kernel tried on Android 16 from TrebleDroid (see [On a GSI](#on-a-gsi)).
- **Not tested:** IMS / VoLTE / VoWiFi ([#2](../../issues/2)) and CAT's stock Android 11 system.

### On a GSI

Just tried the same kernel I posted on Android 16 from TrebleDroid: [`ci-20250617`](https://github.com/TrebleDroid/treble_experimentations/releases/tag/ci-20250617), `system-td-arm32_binder64-vanilla.img.xz` (booted as a DSU, stock v30 vendor; only the `arm32_binder64` vanilla variant tried, not installed to `system`). All works, even camera and 2nd screen.

- Checked on the device: Wi‑Fi (connected, internet), Bluetooth on, speaker audio, both cameras, all 23 sensors, battery, GPS service up (no fix tested), lid open/close, TT9 S22 installed by the image and set as keyboard.
- **2nd screen:** the kernel draws the clock by itself with the lid closed. Android doesn't drive the outer screen on a GSI (it only sees the inner 480×640 panel), so no notifications or caller ID there. **On Android 15+ the clock shows the wrong local time** (UTC + DST hour; 1 h behind in Spain): [#1](../../issues/1).
- **Hang fix (needed on the stock v30 vendor):** the vendor sets `ro.lmk.use_psi=false`, Android 16 has no memcg v1 and this kernel has no in-kernel LMK, so `lmkd` exits in a loop and `system_server` blocks waiting for it (SystemUI / phone ANRs). The kernel has PSI, so the fix is `ro.lmk.use_psi=true`. Hot fix as root: `resetprop ro.lmk.use_psi true` then `setprop ctl.start lmkd` (TrebleDroid's `phh-on-boot.sh` stops services that crash-loop, so `lmkd` won't come back alone). It's lost on reboot.
- Magisk 27.0 from the image doesn't start on that GSI; root there comes from the `userdebug` build (`adb root`). Magisk's early init still runs `overlay.d` (that's how TT9 S22 got installed); it's the Magisk daemon that doesn't start.
- To make the phone sleep when the lid closes, run once: `adb shell settings put global lid_behavior 1` (AOSP default is "do nothing").
- Digits, `*`, `#`, C, back, green and recents work with AOSP `Generic.kl`. The **orange and speaker keys are unmapped**, and the **red key locks** the phone instead of hanging up.
- **Not measured on the GSI:** battery drain (it doesn't suspend with USB plugged in) and calls (no SIM). IMS / VoLTE: [#2](../../issues/2).

## Source

- **Kernel source (GPLv2):** branch [`stock-stable-4`](../../tree/stock-stable-4). Every change from the Qualcomm base is a separate commit with a full description. The tag `stock-stable-4` is the exact source of the release image; the branch only adds copyright notices on top (comments).
- Defconfig: `arch/arm/configs/s22flip_defconfig`.
- Build: AOSP Clang 10.0.6 `r377782d` with the GCC 4.9 `arm-linux-androideabi` binutils:
  ```
  make ARCH=arm CC=clang CLANG_TRIPLE=arm-linux-gnueabi- \
       CROSS_COMPILE=arm-linux-androideabi- LOCALVERSION= s22flip_defconfig
  make ARCH=arm CC=clang CLANG_TRIPLE=arm-linux-gnueabi- \
       CROSS_COMPILE=arm-linux-androideabi- LOCALVERSION= -j$(nproc) zImage dtbs modules
  ```
  `LOCALVERSION=` must be empty so `vermagic` stays `4.9.227-perf` and the stock vendor modules load. The `zImage` is then repacked into the stock v30 boot image (stock DTB, header and cmdline).
- **Magisk 27.0 (GPLv3):** [topjohnwu/Magisk v27.0](https://github.com/topjohnwu/Magisk/tree/v27.0).
- **TT9 S22 (Apache-2.0):** patches on top of [sspanak/tt9](https://github.com/sspanak/tt9) v64.0 in [`tt9/`](tt9), with the steps to rebuild the published APK.
- The outer-screen font is generated from Roboto Regular (Apache-2.0).

## License

Copyright (C) 2026 cat-s22.

- Kernel, this README and `ramdisk/overlay.d`: **GPL-2.0** ([`LICENSE`](LICENSE)). The kernel keeps the copyright of Linux, Qualcomm and the driver authors; the S22 Flip work is marked `Copyright (C) 2026 cat-s22` in the files it wrote or changed.
- TT9 S22: **Apache-2.0** ([`tt9/LICENSE`](tt9/LICENSE), [`tt9/NOTICE`](tt9/NOTICE)), based on TT9 by Dimo Karaivanov and contributors.
- Magisk 27.0: GPL-3.0, by John Wu.

Details in [`NOTICE`](NOTICE).

## Disclaimer

Provided as is, without any warranty (see GPLv2). Flashing a boot image always carries some risk: keep a backup of your `boot.img`. Not affiliated with CAT, Caterpillar or Bullitt Group; "CAT S22 Flip" is only used to say which phone this is for.
