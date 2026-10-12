# CAT S22 Flip kernel — the "i missed school" version

Same info as the [nerd README](README.md), minus the nerd.

## Wtf is this

The kernel is the part of the phone that talks to the keys, screens and battery. CAT's one sucks and they never shared the code. This one replaces it: fixes the basic stuff, stays close to stock, doesn't try to be cool. **Stable > fancy.**

One file. Flash it. Your apps and photos stay.

## What it fixes (and who to blame)

- **Keypad.** Stock gives you "hhello" and ignores soft presses on worn keys. Now one press = one letter. Four suspects, all guilty:
  - **The kernel** checked the key while it was still bouncing and basically flipped a coin.
  - **The circuit** was built for brand-new keys only. Worn key? "Didn't happen."
  - **Android** asks the app what's before the cursor, the app takes a nap, your letter gets bored and leaves.
  - **The keyboard app** was on vacation. TT9 S22 is the one that came back.
- **Lid.** The kernel told Android about the lid in a language Android doesn't speak. Android left it on read. Fixed.
- **Outer screen.** Lid closed = time, date and battery, no app needed. The stock driver waited for a "ready" that never comes, mixed up the colors and could freeze the whole phone when you closed the lid. Classic.
- **Stuff CAT never published:** touchscreen, speaker, USB‑C headphones, hearing-aid coil, keypad light, lid sensor. All work. Dug out of other phones and CAT's own files. Treasure hunt, no map.
- **Battery.** Lid closed, it basically sleeps. No villain here.

## Comes with

- **TT9 S22**, a keyboard made for this keypad. Installs itself on first boot. Hate it? Uninstall it, it won't come back.
- **Magisk** (root). Open it once with internet to finish setup.

## Using another keyboard?

Turn its "key repeat protection" / debounce **off (0)**. The kernel already does that job. Both doing it = eaten letters, worse typing. Can't turn it off? Use TT9 S22.

## Install

You need an **unlocked bootloader** and a PC with `adb` and `fastboot`. Don't know what that is? Google first, flash later.

1. Download `boot-s22-stock-stable-4-tt9en.img` from [Releases](../../releases/tag/stock-stable-4).
2. **Back up your current `boot.img`.** That's your undo button.
3. Run:
   ```
   adb reboot bootloader
   fastboot flash boot boot-s22-stock-stable-4-tt9en.img
   fastboot reboot
   ```

Nothing gets wiped.

**Doesn't boot, or says "data may be corrupt"? DON'T factory reset.** Just flash your backup:
```
fastboot flash boot <your saved boot.img>
```

## Tested?

- **My phone, every day** (my own LineageOS 20, not public, with CAT's vendor): works.
- **Android 16** (TrebleDroid GSI): mostly works. Read the crap below.
- **CAT's stock Android 11:** no idea.
- **Calls over 4G (VoLTE / VoWiFi):** no idea. Carriers are killing 2G/3G, so this one matters. Test it and tell me in [issue #2](../../issues/2), no PC needed.

## Known crap

- **Android 15+:** the outer clock shows the wrong time. Android stopped telling the kernel your time zone. [Issue #1](../../issues/1).
- **Android 16 + CAT's vendor:** freezes unless you run this after every reboot (needs root):
  ```
  resetprop ro.lmk.use_psi true
  setprop ctl.start lmkd
  ```
- **Android 16:** Magisk doesn't start. TT9 S22 still installs.
- **Android 16:** outer screen is clock only, orange and speaker keys do nothing, red key locks the phone instead of hanging up.
- **Android 16:** closing the lid won't put it to sleep until you run this once: `adb shell settings put global lid_behavior 1`
- **Android 16 battery:** not checked yet.

## Support

Main issues only. Hobby, not a product. Really broken? Open an [issue](../../issues).

## Boring legal stuff

- Free and open source. Code in the [`stock-stable-4` branch](../../tree/stock-stable-4), how to build it in the [nerd README](README.md#source).
- Kernel and READMEs: GPL-2.0. TT9 S22: Apache-2.0. Magisk: GPL-3.0. See [`NOTICE`](NOTICE).
- **No warranty.** Flashing can go wrong, keep that backup.
- Not related to CAT, Caterpillar or Bullitt Group.
