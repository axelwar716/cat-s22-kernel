# CAT S22 Flip kernel — the simple version

This is the easy-mode explanation of the [main README](README.md). No tech talk. If you want the details, read that one.

## What is this?

The **kernel** is the bottom layer of the phone's software: the part that talks to the keys, the screens, the battery and so on. CAT shipped the S22 Flip with a kernel that has a bunch of annoying bugs, and they never shared its code.

This is a **replacement kernel** that fixes the basic stuff and stays as close to the original as possible. The goal is **stable**, not fancy.

You get one file. You put it on the phone. Your apps, photos and settings stay where they are.

## What it fixes

- **Keypad.** On stock, letters come out doubled ("hhello") and soft taps get ignored, especially on the 1 / 4 / 7 / * keys. Now every press counts once, even on worn keys. Why it was broken? Four suspects, all guilty:
  - **The kernel driver** looked at the key exactly once, ~10 ms after you touched it, while the contact was still bouncing for up to 18 ms. It basically asked "one press or two?" mid-bounce and guessed with a coin. Hello "hhello".
  - **The circuit** was designed for brand-new keys only. A worn key sends a weaker signal and the stock wiring just shrugs: "too quiet, didn't happen". Apparently nobody pictured a keypad older than six months.
  - **Android** asks the text box "hey, what's before the cursor?" and some apps take a little nap before answering. Your letter waits, gets bored and leaves.
  - **The keyboard app** was on vacation: an extra delay stacked on top of everything else, letters dropped when it got busy, and the odd random space when a timer ran out. TT9 S22 is the one that came back from holidays.
- **Lid.** On stock, Android never really knew if the flip was open or closed. Now it does. Why: the kernel announced the lid as a mystery key that Android had never heard of, so Android left it on read.
- **Outer screen (the small one on the outside).** With the lid closed it shows the **time, the date and the battery %**, by itself, no app needed. It lights up for 8 seconds when you close the lid or press volume / the orange key. With the lid open it stays off. Why it was a mess: the stock driver waited for a "ready" signal the screen never sends (patience of a saint, memory of a goldfish), mixed up its colors, and could jam its own queue and freeze the phone when you closed the lid.
- **Missing parts CAT never published:** touchscreen, loudspeaker, headphones over USB‑C, the hearing-aid coil, the keypad backlight and the lid sensor. All work. Why: CAT never shared their kernel code, so the missing bits were dug out of other phones' public kernels and CAT's own binaries. Treasure hunt, no map.
- **Battery:** with the lid closed it barely uses anything (measured over 35.7 hours with the lid closed, on my LineageOS). No villain here: the outside clock wakes up once a minute, draws, and goes straight back to sleep.

## What else comes in the file

- **TT9 S22**, a keyboard app made for this keypad. It installs itself the first time the phone starts. Don't want it? Uninstall it like any app; it won't come back.
- **Magisk**, which gives you root (admin access to the phone). Open the Magisk app once with internet to finish setting it up.

## Using a different keyboard app?

Fine, but go to its settings and turn its **"key repeat protection" / debounce to 0 ms or off**. The kernel already does that job. If the app does it too, fast double presses (like "oo") get eaten and typing gets **worse**, not better. If your keyboard app doesn't let you turn it off, use TT9 S22 instead.

## How to install

You need an **unlocked bootloader** (if you don't know what that is, stop here and look it up first) and a PC with `adb` and `fastboot`.

1. Download `boot-s22-stock-stable-4-tt9en.img` from [Releases](../../releases/tag/stock-stable-4).
2. **Save a copy of your current `boot.img` first.** This is your undo button.
3. Plug the phone in and run:
   ```
   adb reboot bootloader
   fastboot flash boot boot-s22-stock-stable-4-tt9en.img
   fastboot reboot
   ```
4. **Nothing is wiped.** Your ROM and your stuff stay.

**If it doesn't start, or it says "data may be corrupt": DON'T factory reset.** Just put your old one back:
```
fastboot flash boot <your saved boot.img>
```

## What has been tested

- **Works:** on my own phone every day (my own LineageOS 20, not public, with CAT's original v30 vendor).
- **Also works on Android 16** (a "GSI" called TrebleDroid): Wi‑Fi, Bluetooth, sound, both cameras, sensors, lid and the outer-screen clock. But read the problems below.
- **Not tested:** CAT's original Android 11.
- **Not tested: calls over 4G (VoLTE / VoWiFi).** I don't know if they work. Lots of carriers have shut down old 2G/3G, so without VoLTE your phone might not be able to call at all. **Please test it and tell me**: [issue #2](../../issues/2) explains how, even without a PC.

## Known problems

- **On Android 15 or newer, the outer clock shows the wrong time.** Android stopped telling the kernel your time zone, so the clock runs on UK time. In Spain, for example, it's 1 hour behind. On Android 13 (like my LineageOS) it's fine. Details: [issue #1](../../issues/1).
- **On Android 16 with CAT's original vendor, the phone freezes** unless you apply a small fix after every restart (you need root):
  ```
  resetprop ro.lmk.use_psi true
  setprop ctl.start lmkd
  ```
  It's lost every time you reboot.
- **On Android 16, Magisk doesn't start**, so no Magisk root there (that build has its own root through `adb root`). TT9 S22 still installs fine.
- **On Android 16, the outer screen only shows the clock**: no notifications or who's calling.
- **On Android 16, the orange and speaker keys do nothing**, and the red key locks the phone instead of hanging up.
- **On Android 16, to make the phone sleep when you close the lid**, run this once: `adb shell settings put global lid_behavior 1`
- **Battery on Android 16 hasn't been measured yet.**

## Support

I fix main issues only. This is my hobby, not a product. If something is badly broken, open an [issue](../../issues).

## The boring legal bit

- Free and open source. The code is in the [`stock-stable-4` branch](../../tree/stock-stable-4); how to build it is in the [main README](README.md#source).
- Kernel and READMEs: GPL-2.0. TT9 S22: Apache-2.0. Magisk: GPL-3.0. See [`NOTICE`](NOTICE).
- **No warranty.** Flashing always has some risk, so keep that `boot.img` backup.
- Not related to CAT, Caterpillar or Bullitt Group.
