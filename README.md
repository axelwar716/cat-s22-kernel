# CAT S22 Flip — custom kernel `stock-stable-5`

> **Are u dumb like me? Read the [i missed school](README-SIMPLE.md) version.**

**Coded and vibe coded bruh. Won't support anything that isn't a main issue. Cuz i'm developing with custom android, experimental custom kernel & custom vendor, not base GSI. Anything in stock sucks**

A **stability-focused** kernel for the CAT S22 Flip (firmware v30, `4.9.227-perf`), shipped as a single boot image with Magisk 27.0 and the TT9 S22 keyboard.

## Scope: stable first

The only goal of this build is to be **stable**: fix the keypad and the other basic things (was complex asf) that are broken or missing on stock, and nothing more. It stays as close to stock as possible: same kernel version, the stock config and a Clang 10 compiler like stock (small differences listed [below](#kernel-base)), the stock DTB, header and command line, and CAT's own signed vendor modules loading untouched.

I run a more advanced kernel and vendor on my own phone. This one is not that: it is the conservative subset, published so everyone can have a better experience on the S22 while staying relatively safe.

> **Not tested:** IMS / VoLTE / VoWiFi. I haven't tried them with this kernel, so I can't say whether they work. Reports and easy debug steps: [#2](../../issues/2).
>
> **Tested on 3 GSIs:** Android 13 (LineageOS 20), Android 15 (VoltageOS 4.5) and Android 16 (TrebleDroid) boot with this kernel, and video recording + flashlight were checked on all three. What I checked on each one is in [Tested on](#tested-on). On the stock v30 vendor, Android 16 needs one prop fix or it hangs. On Android 15+ the outer-screen clock shows the wrong local time ([#1](../../issues/1)).
> **CAT's stock Android 11:** `stock-stable-4` was checked.

## Downloads

From [Releases → `stock-stable-5`](../../releases/tag/stock-stable-5):

| File | What it is |
|---|---|
| `boot-s22-stock-stable-5-tt9en.img` | Kernel + Magisk 27.0 + TT9 S22 (English) — **the image to flash** |
| `TT9S22-en.apk` | The same TT9 S22 keyboard on its own, to reinstall it after uninstalling |
| `SHA256SUMS` | Checksums |

```
125421ea9dd91b97e77aa0f2b7067dd405c434e27fc4d2893549c75f02eff196  boot-s22-stock-stable-5-tt9en.img
95481d05f6d9ce87a01629fce0ef15ce8724eaf7e4c3e7ab8743f38d94f546ab  TT9S22-en.apk
```

Don't want TT9? Uninstall it like any app: it won't be reinstalled on the next boot. Using another keyboard app instead? Set its own key debounce ("accidental key repeat protection") to **0 ms / off**, or it stacks on top of the kernel's and fast presses get dropped (details in [TT9 S22 keyboard](#tt9-s22-keyboard)).

## Install

Only `boot` is replaced: **no wipe**, your ROM and data stay as they are. One image for every ROM. Needs an **unlocked bootloader**.

Never unlocked the bootloader or backed up `boot` before? Read the XDA thread [CAT S22 Flip: installing GSI ROM](https://xdaforums.com/t/cat-s22-flip-installing-gsi-rom.4660690/), any page: people there explain unlocking, backing up and flashing step by step.

1. **Back up your current `boot.img` first.**
2. Flash:
   ```
   adb reboot bootloader
   fastboot flash boot boot-s22-stock-stable-5-tt9en.img
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

### Flashlight and video recording (new in `stock-stable-5`, [#3](../../issues/3))

Both were broken with every earlier build of this kernel, on every ROM (my own LineageOS 20 too). CAT's stock kernel was fine: the bugs come from the Qualcomm base code this kernel is built on.

- **Flashlight went off after ~3 s.** The flash LED chip has two control pins: GPIO 33 (`FL_EN`) and GPIO 34 (`FL_NOW`). Measured on the phone (battery voltage with charging off): 33 alone = torch, steady; 34 alone = nothing; 33 + 34 = flash mode, which the LED chip cuts by itself after ~1.5 s to protect the LED. The Qualcomm `qm215_gpio_flash` driver drove **both** pins for the flashlight. Now flashlight = 33 only, camera flash = 33 + 34.
- **Video recording stopped at once, no file saved.** The video encoder driver (`msm_vidc`) asked for camera frames 16 KiB bigger than the ones the stock v30 vendor allocates (1080p: 3,153,920 vs 3,137,536 bytes), because `VENUS_BUFFER_SIZE` added an extradata margin inside the frame. So the kernel rejected the first camera frame (`Failed to qbuf, -22`) and the camera app gave up. The encoder firmware itself only needs 3,133,568 bytes, so the margin is gone. Screen recording always worked: only camera → encoder was hit.
- **Checked with the release image** on Android 13, 15 and 16 ([Tested on](#tested-on)): 1080p and 720p video at ~30 fps with sound, no encoder errors, the whole file decodes cleanly. The flashlight stays on for the whole 14 s test (steady LED draw on Android 15 and 16; on Android 13 checked from Android's side only, no root there).
- **Checked with the test build** (same code, Android 15): video playback with the hardware decoder still works, and the photo flash fires (1/10,040 s at ISO 114 with flash, 1/10 s at ISO 2,976 without).

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

How it gets installed (see [`ramdisk/overlay.d`](ramdisk/overlay.d), nothing is written to `system` or `vendor`): Magisk's `overlay.d` runs a one-shot script after boot. If TT9 S22 is missing or older, it installs it and, on a fresh install, sets it as the keyboard. It then writes a marker in `/data/adb`, so **if you uninstall it, it stays gone**, and deletes the bundled APK from Magisk's tmpfs so it doesn't stay in RAM. On later boots it does nothing, and a later image with a newer TT9 S22 updates it only if you still have it (it never comes back after an uninstall, and is never set as the keyboard again). Log: `logcat -s tt9-s22`. The copies here are the ones in the `stock-stable-5` image. `stock-stable-4` had the same scripts without the license header and without the "never comes back after an uninstall, even with a newer TT9 S22" check (it bundled the same TT9 S22 version, so it behaved the same). If a ROM's setup wizard sets its own keyboard after TT9 S22 is installed, pick TT9 S22 once in Settings → Languages & input (seen on the LineageOS 20 GSI after a wipe).

### Root

Magisk 27.0 is in the image: root from the first boot. Open the Magisk stub once with internet to get the full app, or install the [Magisk 27.0 APK](https://github.com/topjohnwu/Magisk/releases/tag/v27.0).

---

## Tested on

### On a GSI (i don't think higher android versions would bring any useful value, but spend more battery, not tested fully)

All on the stock v30 vendor, Android installed to `system` (`/data` wiped when switching), boot image flashed as is:

| ROM | Android | Kernel | Result |
|---|---|---|---|
| [LineageOS 20 GSI](https://sourceforge.net/projects/andyyan-gsi/files/lineage-20-td/) `a64_bvN` (AndyYan, 21 Oct 2025, `userdebug`) | 13 | `stock-stable-4` + `stock-stable-5` | Boots in ~2 min, SELinux enforcing. No `lmkd` fix needed. Magisk works. 1080p video and flashlight OK with `stock-stable-5` |
| [VoltageOS 4.5](https://github.com/cawilliamson/treble_voltage/releases/tag/4.5-20250717.114637) vanilla `arm32_binder64` (17 Jul 2025) | 15 | `stock-stable-4` + `stock-stable-5` | Boots in ~5 min, SELinux enforcing. No `lmkd` fix needed. Magisk works. Video, flashlight and photo flash OK with `stock-stable-5` (broken with `stock-stable-4`, that's how [#3](../../issues/3) was found) |
| [TrebleDroid `ci-20250617`](https://github.com/TrebleDroid/treble_experimentations/releases/tag/ci-20250617) `system-td-arm32_binder64-vanilla` | 16 | `stock-stable-4` (as a DSU) + `stock-stable-5` (in `system`) | Boots, **needs the `lmkd` fix below or it hangs**. Magisk daemon doesn't start. 1080p video and flashlight OK with `stock-stable-5` |

Only `a64` / `arm32_binder64` GSIs work on this phone (32-bit userspace); `arm64` ones won't boot.

How I installed them (stock v30 `super` has no room for a GSI next to `product`):
```
fastboot --disable-verity --disable-verification flash vbmeta vbmeta.img   # bootloader, stock v30 vbmeta
fastboot reboot fastboot                                                    # fastbootd
fastboot delete-logical-partition product
fastboot flash system <gsi>.img
fastboot reboot bootloader
fastboot erase userdata
fastboot reboot
```

**Same on all three:** the lid is detected (`s22_lid`) and TT9 S22 is installed by the image. Android doesn't see the outer screen as a display on a GSI: only the kernel clock is there.

**Android 13 (LineageOS 20 GSI):**
- TT9 S22 gets installed but the setup wizard keeps the stock keyboard: pick TT9 S22 once in Settings → Languages & input.
- `adb root` is off by default (Developer options → Rooted debugging). Wi‑Fi can't be joined from `adb` without root: do it on the phone.

**Android 15 (VoltageOS 4.5):**
- TT9 S22 is the keyboard from the first boot, and the lid already puts the phone to sleep (`lid_behavior=1`).
- The build reports as `user`, so no `adb root`; Magisk's `su` lives in `/debug_ramdisk/su` there (not in the `PATH`).

**Android 16 (TrebleDroid):**
- Wi‑Fi (connected, internet), Bluetooth on, speaker audio, both cameras, all 23 sensors, battery, GPS service up (no fix tested), lid open/close, TT9 S22 installed by the image and set as keyboard (checked as a DSU with `stock-stable-4`).
- **2nd screen:** the kernel draws the clock by itself with the lid closed. **On Android 15+ the clock shows the wrong local time** (UTC + DST hour; 1 h behind in Spain): [#1](../../issues/1).
- **Hang fix (needed on the stock v30 vendor):** the vendor sets `ro.lmk.use_psi=false`, Android 16 has no memcg v1 and this kernel has no in-kernel LMK, so `lmkd` exits in a loop and `system_server` blocks waiting for it (SystemUI / phone ANRs). The kernel has PSI, so the fix is `ro.lmk.use_psi=true`. Hot fix as root (`adb root`, see the Magisk point below): `adb shell /debug_ramdisk/resetprop ro.lmk.use_psi true` then `adb shell setprop ctl.start lmkd` (TrebleDroid's `phh-on-boot.sh` stops services that crash-loop, so `lmkd` won't come back alone). Plain `resetprop` isn't in the `PATH` there, cuz the Magisk daemon doesn't start; Magisk's early init still leaves it in `/debug_ramdisk`. It's lost on reboot.
- Magisk 27.0 from the image doesn't start on that GSI; root there comes from the `userdebug` build (`adb root`). Magisk's early init still runs `overlay.d` (that's how TT9 S22 got installed); it's the Magisk daemon that doesn't start.
- To make the phone sleep when the lid closes, run once: `adb shell settings put global lid_behavior 1` (AOSP default is "do nothing").
- Digits, `*`, `#`, C, back, green and recents work with AOSP `Generic.kl`.

### CAT's stock Android 11 (v30)

`stock-stable-4` on the stock v30 system (`/data` wiped): outer screen, audio, cameras, keypad, lid (sleep/wake) and Wi‑Fi checked by hand. Magisk is there (the app has to be installed). `stock-stable-5` not tried on stock yet.

### My own LineageOS 20

My daily build (not public), stock v30 vendor: everything above the line in this README was developed and measured there. Video recording was broken there too before `stock-stable-5`.

### Not tested anywhere

Calls with a SIM, IMS / VoLTE / VoWiFi: [#2](../../issues/2).

## Source

- **Kernel source (GPLv2):** branch and tag [`stock-stable-5`](../../tree/stock-stable-5), the exact source of the release image. Every change from the Qualcomm base is a separate commit with a full description. Older: [`stock-stable-4`](../../tree/stock-stable-4) (the tag `stock-stable-4` is the exact source of that image; the branch adds copyright notices on top, comments only).
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

- Kernel, both READMEs (`README.md`, `README-SIMPLE.md`) and `ramdisk/overlay.d`: **GPL-2.0** ([`LICENSE`](LICENSE)). The kernel keeps the copyright of Linux, Qualcomm and the driver authors; the S22 Flip work is marked `Copyright (C) 2026 cat-s22` in the files it wrote or changed.
- TT9 S22: **Apache-2.0** ([`tt9/LICENSE`](tt9/LICENSE), [`tt9/NOTICE`](tt9/NOTICE)), based on TT9 by Dimo Karaivanov and contributors.
- Magisk 27.0: GPL-3.0, by John Wu.

Details in [`NOTICE`](NOTICE).

## Disclaimer

Provided as is, without any warranty (see GPLv2). Flashing a boot image always carries some risk: keep a backup of your `boot.img`. Not affiliated with CAT, Caterpillar or Bullitt Group; "CAT S22 Flip" is only used to say which phone this is for.
