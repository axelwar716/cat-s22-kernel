# CAT S22 Flip — kernel `stock-stable-4`

A **stability-focused** kernel for the CAT S22 Flip (firmware v30, `4.9.227-perf`), shipped as a single boot image with Magisk 27.0 and the TT9 S22 keyboard.

## Scope: stable first

The only goal of this build is to be **stable**: fix the keypad and the other basic things that are broken or missing on stock, and nothing more. It stays as close to stock as possible: same kernel version, same config, same toolchain, the stock DTB, header and command line, and CAT's own signed vendor modules loading untouched.

I run a more advanced kernel and vendor on my own phone. This one is not that: it is the conservative subset, published so everyone can have a better experience on the S22 while staying relatively safe.

> **Not tested:** IMS / VoLTE / VoWiFi. I haven't tried them with this kernel, so I can't say whether they work.
> **Not tested on a GSI** either (see [On a GSI](#on-a-gsi)).

## Downloads

From [Releases → `stock-stable-4`](../../releases/tag/stock-stable-4):

| File | What it is |
|---|---|
| `boot-s22-stock-stable-4-tt9en.img` | Kernel + Magisk 27.0 + TT9 S22 (English) — **recommended** |
| `boot-s22-stock-stable-4.img` | Same kernel + Magisk 27.0, without TT9 |
| `TT9S22-en.apk` | The TT9 S22 keyboard on its own |
| `SHA256SUMS` | Checksums |

```
bfb84f2d71a4f804bd03968451f70b33deb6090c54180d083ee65cb2efc26801  boot-s22-stock-stable-4-tt9en.img
c11216b0afcf60379af82027f1ef1f00a90354368850fb8dd0e36af6427d5d3b  boot-s22-stock-stable-4.img
95481d05f6d9ce87a01629fce0ef15ce8724eaf7e4c3e7ab8743f38d94f546ab  TT9S22-en.apk
```

## Install

Only `boot` is replaced: **no wipe**, your ROM and data stay as they are. Same image for LineageOS and GSIs.

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

- Qualcomm 4.9 tree (`LA.UM.9.8.c26`), the stock `/proc/config.gz`, AOSP Clang 10.0.6 (`r377782d`). `uname -r` is still `4.9.227-perf`.
- Stock v30 DTB, boot header and command line. All drivers built in.
- **730/730 vendor symbol CRCs match stock.**
- CAT's module-signing certificate (RSA-4096, SHA-512) was extracted from the stock kernel and is in the keyring, so CAT's **25 signed audio and Wi‑Fi modules load untouched with signature enforcement on.**
- 8 audio exports the stock modules need (FSA4480, AW8155, AW883xx) were reimplemented, with signatures recovered from the stock CRCs, so the stock audio modules link.

### Keypad (`matrix_keypad` rewrite)

**The problem on stock:** the kernel read the keys once, ~10 ms after the first edge (HZ=100 tick + workqueue lag), while the contacts bounce for 9–18 ms. Result: doubled letters, and light taps that never register (worst on the 1/4/7/* row).

- Keys are sampled on the edge by a `SCHED_FIFO` thread. An hrtimer re-samples every 1 ms until the contacts settle, and every 5 ms while a key is held.
- A press is 6 ms of cumulative closure per burst; gaps under 30 ms don't split it; bursts under 6 ms are noise. A release is 30 ms fully open.
- **Electrical change to the key rows, for better sensitivity.** Stock idles the rows low through the SoC pull-down. A light press on a worn dome has high contact resistance, forms a divider with that pull-down and never crosses the input threshold → no IRQ, lost tap. Now the pull-down is off (written straight to TLMM `GPIO_CFG`) and each row is actively driven low for 1 µs before every read and before the IRQ is re-armed → light taps register, no ghost presses. The pull-down is restored in suspend so a floating row can't wake the phone.
- Rows are re-read after the IRQ is re-armed (the pinctrl-msm dual-edge emulation can miss an edge).
- Idle = IRQ only, no polling.
- The gpiolib log spam is gone (3,565 lines per 2 s of typing on stock).
- Tunable in `/sys/module/matrix_keypad/parameters/`: `press_ms`, `release_ms`, `row_pull` (0 none · 1 stock pull-down · 2 keeper), `fall_us`, plus the counters `rejected_taps`, `rearm_rescans`, `follow_fail`, `irq_no_key`.
- **Tested:** 248 fast presses on 4/7 → 0 doubles; ~500 real presses × 62 timing variants → 0 lost, 0 ghosts.

### Lid (Hall sensor)

- On stock, the lid sent key events that Android dropped, so **Android never knew whether the flip was open or closed.**
- Now a real `SW_LID` is reported (1 = closed, the Linux/AOSP meaning) on an input device called `s22_lid`. Because of the new name, LineageOS's S22 hall patch (which inverts `hall_sensor`) can't double-invert it → **the same image works on every ROM.**
- The `switch_hall` / `hall_switch` switch (1 = open) and `/dev/input/hall_dev` are kept for S22Cover. Scripts that looked for the input by the name `hall_sensor` should look for the phys `/dev/input/hall_dev` instead.
- Stock's 300 ms busy-wait inside the IRQ handler → delayed work.

### Outer screen (128×128 SPI ST7789V2, `fb2`, `s22_kclock`)

- **Lid closed:** the kernel owns the outer screen and draws a 24 h clock, the date ("Mon 12 Oct") and the battery ("85%", "85% +" when charging/full), styled like S22Cover (same Roboto metrics, light-grey text). Redrawn every minute by a wake alarm, also in deep sleep. **No app needed.**
- Kernel time zone with EU/US DST until Android sets the zone.
- Backlight on for 8 s on lid close, volume or orange key.
- **Lid open:** the outer screen is held off (no power vote, backlight 0) → no mirroring. Android frames are accepted but never sent over SPI.
- S22Cover-style apps take it over cleanly by writing `s22_kclock/config` (their last frame + backlight). `s22_kclock.autonomous=0` disables the kernel clock.
- **Measured:** 35.7 h with the lid closed → ~10.6 mA average, 0 SPI errors.
- Stock bugs fixed in the SPI panel driver: mmap bug, RGBA8888 → RGB565 conversion, pluggable "dtv" panel for the stock compositor, no waiting for a TE pulse the panel never sends; `lcd-backlight2` writes now reach the outer screen; hang after closing the lid (corrupted SPI queue); lid-closed power-off matched to stock.

### Drivers CAT didn't publish

Ported from public GPL kernels with the same parts, or written for this phone:

- **Touchscreen** Chipsemi CHSC5448 (`chsc5xxx` v3.6.2, from the HelloVolla GPL kernel): all 84 stock symbols, 5 points, glove mode, ESD heartbeat; ignores the outer screen's framebuffer like stock. **The touch firmware is never reflashed** (boot-time update off).
- **Speaker amp** AW881xx v1.9.0: stock ACF tuning selected by chip name like stock V1.8.0.2 (otherwise it stays silent); the first start reloads with I2S running and reads the DSP PID (fixes "dsp check fail"); calibration in `persist/aw_cali.bin`.
- **USB‑C** `wusb3801` + **FSA4480**: 3/4-pole headphone detection over USB‑C.
- **AW8155** hearing-aid coil amp (modes set by EN pulses). **AW883xx:** handshake only (this unit has an AW88194).
- **Keypad backlight** (recovered from the stock disassembly): on only when the lid is open and the main screen is on (`follow_lid`).
- **AOSP 3.18 `switch` class**, needed by the Hall driver.
- **Battery:** `ACC_IBAT_ROWS` 4 → 6 to match the Q2805 cell profile.
- **Not ported:** Goodix GT1X (not fitted on this phone).

### TT9 S22 keyboard

A fork of [TT9](https://github.com/sspanak/tt9) v64.0 (English only, 3.8 MB, package `io.github.sspanak.tt9.s22`, versionCode 1652), tuned for this keypad. It coexists with the regular TT9.

- Key-repeat guard off: it trusts the kernel's debounce.
- In-memory ABC multitap; the 800 ms multitap timer runs from your last real press.
- IME threads keep foreground priority while the keyboard is hidden.
- **Needs this kernel:** on the stock kernel the bouncing comes back.

How it gets installed (see [`ramdisk/overlay.d`](ramdisk/overlay.d), nothing is written to `system` or `vendor`): Magisk's `overlay.d` runs a one-shot script after boot. If TT9 S22 is missing or older, it installs it and, on a fresh install, sets it as the keyboard. It then writes a marker in `/data/adb`, so **if you uninstall it, it stays gone**, and deletes the bundled APK from Magisk's tmpfs so it doesn't stay in RAM. On later boots it does nothing. Log: `logcat -s tt9-s22`.

### Root

Magisk 27.0 is in the image: root from the first boot. Open the Magisk stub once with internet to get the full app, or install the [Magisk 27.0 APK](https://github.com/topjohnwu/Magisk/releases/tag/v27.0).

---

## Status

- Daily driver on LineageOS 20, flashed without wipe. TT9 installs on the first boot and does nothing on the second.
- **Not tested:** IMS / VoLTE / VoWiFi, and any GSI.

### On a GSI

- To make the phone sleep when the lid closes, run once: `adb shell settings put global lid_behavior 1` (AOSP default is "do nothing").
- Digits, `*`, `#`, C, back, green and recents work with AOSP `Generic.kl`. The **orange and speaker keys are unmapped**, and the **red key locks** the phone instead of hanging up.

## Source

- **Kernel source (GPLv2):** branch [`stock-stable-4`](../../tree/stock-stable-4) (tag `stock-stable-4`). Every change from the Qualcomm base is a separate commit with a full description.
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
- **TT9 (Apache-2.0):** [sspanak/tt9](https://github.com/sspanak/tt9).
- The outer-screen font is generated from Roboto Regular (Apache-2.0).

## Disclaimer

Provided as is, without any warranty (see GPLv2). Flashing a boot image always carries some risk: keep a backup of your `boot.img`. Not affiliated with CAT, Caterpillar or Bullitt Group; "CAT S22 Flip" is only used to say which phone this is for.
