# TT9 S22 source

TT9 S22 (`TT9S22-en.apk` in the release, also inside the boot image) is [Traditional T9](https://github.com/sspanak/tt9) v64.0 with these patches:

| Patch | What it does |
|---|---|
| `0001-reliable-input-on-the-S22-Flip.patch` | TT9's own debounce set to 0 (the kernel already debounces the keypad), ABC multitap without editor queries, synchronous ABC suggestions, auto-accept timer counted from the last real key press |
| `0002-lighter-ABC-multitap.patch` | Less repainting and fewer editor round trips per letter |
| `0003-keyboard-thread-priority.patch` | Keyboard threads at foreground-app priority even while the keyboard is hidden |
| `0004-public-package-name.patch` | Package `io.github.sspanak.tt9.s22`, name "TT9 S22", ABC letter suggestions on by default |
| `0005-English-only-build.patch` | English only (T9 + ABC); the 145 files of the other 54 languages are listed in `english-only-delete.txt` |

Rebuild (from the root of this repo):

```
git clone https://github.com/sspanak/tt9 tt9-src && cd tt9-src && git checkout v64.0
git am ../tt9/patches/000[1-4]-*.patch
xargs rm -f < ../tt9/english-only-delete.txt
git apply ../tt9/patches/0005-English-only-build.patch
./gradlew assembleFullRelease
```

The result is the exact source tree of the published APK (version `64.0-s22.5-pub-en`, code 1652). The APK in the release is signed with the cat-s22 key (`CN=cat-s22`), so a self-built APK will not update it in place.

License: Apache-2.0 (see `LICENSE` and `NOTICE`).
