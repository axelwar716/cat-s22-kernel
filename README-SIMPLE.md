# CAT S22 Flip kernel — the "i missed school" version

Same info as the [nerd README](README.md), minus the nerd.

> **tl;dr:** the stock kernel reads ur keys while the contacts are still bouncing, holds the key rows to ground so a worn key can't win, and tells android about the lid in a language android doesn't speak. this one fixes that. **stable > fancy.**

## Wtf is this

The kernel is the thing between android and the hardware: keys, screens, battery. CAT's one sucks asf and they never shared the code, so i rebuilt it to match stock and fixed the basic stuff. not trying to be cool, just trying to not be broken.

One file. Flash it. Ur apps and photos stay.

## What it fixes (and who to blame)

**Keypad.** honestly a pain in the a\*\*. "hhello", eaten letters, soft presses on worn keys just ignored. multiple root causes, all guilty:

- **Kernel (driver):** looked at the key once, while the contact was still bouncing, and basically flipped a coin → double letters. now it watches the key until it settles. one press = one letter.
- **The circuit:** stock keeps every key row pulled down to ground. a worn key makes a weak contact and can't pull the row far enough to count as "pressed", so the tap dies. now the pull-down is off and the row gets driven low right before every read → soft taps count, no ghost presses.
- **Android layer:** the keyboard asks the text box "what's before the cursor?" and some apps take a nap before answering. ur letter waits, gets bored, leaves.
- **The app:** stacked its own delay on top of all that, dropped letters when busy, and threw random spaces when its timer ran out. **TT9 S22** trusts the kernel (its own debounce off), keeps the multitap in its own memory and starts the timer from ur last real press.

**Lid.** stock sent the lid as some random key android doesn't know, so android left it on read: it never knew if the flip was open or closed. now it's a real lid switch (`SW_LID`, the standard linux one), so any rom gets it with no patch. same image for every rom.

**Outer screen.** lid closed = time, date and battery, drawn by the kernel itself, no app needed. the stock driver waited for a "frame done" signal the panel never sends, sent the colors in the wrong format, and could corrupt its own queue and freeze the whole phone when u closed the lid. classic.

**Stuff CAT never published:** touchscreen, speaker amp, USB‑C headphones, hearing-aid coil, keypad light, lid sensor. all work. dug out of open kernels from other phones with the same chips, plus CAT's own binaries. treasure hunt, no map. and CAT's own signed audio and wifi modules still load untouched, cuz the kernel was rebuilt to match stock.

**Battery.** lid closed, it basically sleeps. the outer clock wakes up just to redraw, then back to sleep. no villain here.

## Comes with

- **TT9 S22**, a keyboard made for this keypad. installs itself on first boot. hate it? uninstall it, it won't come back.
- **Magisk** (root). open it once with internet to finish setup.

## Using another keyboard?

Turn its "key repeat protection" / debounce **off (0)**. the kernel already does that job. both doing it = eaten letters, worse typing. can't turn it off? use TT9 S22.

## Install

U need an **unlocked bootloader** and a PC with `adb` and `fastboot`. don't know what that is? google first, flash later.

1. Download `boot-s22-stock-stable-4-tt9en.img` from [Releases](../../releases/tag/stock-stable-4).
2. **Back up ur current `boot.img`.** that's ur undo button.
3. Run:
   ```
   adb reboot bootloader
   fastboot flash boot boot-s22-stock-stable-4-tt9en.img
   fastboot reboot
   ```

Nothing gets wiped.

**Doesn't boot, or says "data may be corrupt"? DON'T factory reset.** just flash ur backup:
```
fastboot flash boot <your saved boot.img>
```

## Tested?

- **My phone, every day** (my own LineageOS 20, not public, on CAT's vendor): works.
- **Android 16** (TrebleDroid GSI): mostly works. read the crap below.
- **CAT's stock Android 11:** no idea.
- **Calls over 4G (VoLTE / VoWiFi):** no idea. carriers are killing 2G/3G, so this one matters. test it and tell me in [issue #2](../../issues/2), no PC needed.

## Known crap

- **Android 15+:** the outer clock shows the wrong time. android stopped telling the kernel ur time zone. [Issue #1](../../issues/1).
- **Android 16 + CAT's vendor:** freezes cuz the memory killer (`lmkd`) keeps crashing, unless u run this after every reboot (needs root):
  ```
  resetprop ro.lmk.use_psi true
  setprop ctl.start lmkd
  ```
- **Android 16:** Magisk doesn't start. TT9 S22 still installs.
- **Android 16:** outer screen is clock only, orange and speaker keys do nothing, red key locks the phone instead of hanging up.
- **Android 16:** closing the lid won't put it to sleep until u run this once: `adb shell settings put global lid_behavior 1`
- **Android 16 battery:** not checked yet.

## Support

Main issues only. hobby, not a product. really broken? open an [issue](../../issues).

## Boring legal stuff

- Free and open source. code in the [`stock-stable-4` branch](../../tree/stock-stable-4), how to build it in the [nerd README](README.md#source).
- Kernel and READMEs: GPL-2.0. TT9 S22: Apache-2.0. Magisk: GPL-3.0. See [`NOTICE`](NOTICE).
- **No warranty.** flashing can go wrong, keep that backup.
- Not related to CAT, Caterpillar or Bullitt Group.
