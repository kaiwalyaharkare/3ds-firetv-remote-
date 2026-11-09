# Development log

How this project went from "my Fire TV remote went through the washing machine" to a working 3DS app, including the problems hit along the way. If you're debugging something similar, the fixes below may save you an evening.

---

## The idea

The Fire TV remote got left in a pocket and went through a full wash cycle. It came out clean, and it never worked again. A modded original 3DS was lying around, and it has Wi-Fi, buttons and a touch screen. Fire TVs ship with **ADB** (Android Debug Bridge), which accepts `input keyevent` commands over TCP port 5555 once *ADB debugging* is turned on. So the 3DS just needs to speak ADB.

## Design decisions

| Decision | Why |
|---|---|
| **The 3DS talks ADB directly to the Fire TV** | The first design used a Raspberry Pi relay. Dropping it means one device and nothing else to keep running |
| ADB auth implemented on the 3DS with **mbedtls 2.28** (devkitPro's `3ds-mbedtls`) | The Fire TV needs RSA signing (PKCS#1 v1.5 / SHA-1) and Android's custom `RSAPublicKey` format. No root or extra software on the TV |
| **One persistent `shell:` stream**, with consecutive key events merged | Opening a new ADB stream per button press is slow, and batching keeps D-pad scrolling responsive |
| **citro2d** UI on both screens | Looks like a real app, with status, feedback and a touch panel for extra buttons |
| Ship both **`.cia`** and **`.3dsx`** | The `.cia` gives a HOME Menu icon, and the `.3dsx` is quick to iterate on |
| **GitHub Actions** builds both on every push | Anyone can get a build without installing a toolchain |
| A **fake Fire TV** (`tests/mock_adbd.py`) for host tests | Protocol bugs can be caught on a PC before touching hardware |

## Timeline

### 1. Writing the code without a toolchain

The first version was written in a cloud workspace that couldn't reach devkitPro's servers. So `adb.c` was compiled for Linux and tested against the fake Fire TV, covering the new-key flow, the known-key flow, key batching, long press, text input and the network scan. `main.c` was only syntax-checked against the real libctru, citro2d and mbedtls headers.

### 2. First real build (Windows)

devkitPro was installed on Windows through MSYS2 (see [BUILDING.md](BUILDING.md#windows-msys2--devkitpro)). Two problems came up:

- **The silent `devkitProUpdater.exe /S` install did nothing.** Installing MSYS2 with winget and adding the devkitPro pacman repos worked instead.
- **`pkg.devkitpro.org` returned 403** to `curl`, so the "keyring package" downloaded was actually a Cloudflare HTML page (`Unrecognized archive format`). Sending a pacman user-agent fixed it.

After that, `make -C 3ds` **compiled and linked on the first try**, with no source changes.

### 3. Uploads arrived as 0-byte files

Copying the `.3dsx` to the 3DS with ftpd failed with `curl: (18) server did not report OK, got 426`. The file on the 3DS was **0 bytes**, and so was a 10-byte test file. All the bytes reached the 3DS, but ftpd couldn't write them to the card.

**Cause: the SD card was full.** It was a 2 GB card with about 1.9 GB used, and **1.88 GB of that was in a hidden `/.Trashes` folder**. macOS creates that folder on removable drives: files "deleted" in Finder go there and keep using space until the Trash is emptied *while the card is plugged in*. The card looked nearly empty from the Mac, but it wasn't.

**Fix:** delete the contents of `/.Trashes` (over FTP or with a card reader). After that, uploads succeeded and the hashes matched.

> **Tip:** if you manage your 3DS SD card from a Mac, empty the Trash before ejecting. Better still, run `dot_clean` or delete with `rm` in Terminal.

### 4. ".3dsx doesn't show on the HOME Menu"

That's expected: `.3dsx` apps only appear inside the Homebrew Launcher. The `.cia` was packaged locally with prebuilt `makerom` v0.19.0 and `bannertool` 1.2.0 for Windows and installed with FBI. FireTV Remote then appeared on the HOME Menu with its icon and banner.

### 5. "Can't reach 192.168.1.x:5555" on real hardware

The app launched and the UI looked right, but it couldn't connect to the TV. A PC on the same network connected to the TV's port 5555 in under 100 ms, and the TV answered with an ADB `AUTH` request. So the network and the TV were fine, and the bug was on the 3DS side.

**Cause:** the app used the standard POSIX pattern for a connect with a timeout. That means a non-blocking `connect()`, then `poll()` for `POLLOUT`, then check `getsockopt(SO_ERROR)`. The 3DS `soc` service doesn't report completion through `poll()` revents and `SO_ERROR` the way POSIX systems do, so a working connection was judged as failed. The network scan used the same check, so it could never find the TV either.

**Fix** (`3ds/source/adb.c`):
1. Confirm completion by **calling `connect()` again**. A return of `0` or `EISCONN` means connected, and `EINPROGRESS`/`EALREADY` means keep waiting. This works on both the 3DS and POSIX.
2. If that fails, **fall back to a plain blocking `connect()`**.
3. The error message now includes the **errno** (`Can't reach … (err N)`), so any future failure can be diagnosed from a photo of the screen.
4. The scan checks each pending socket directly instead of relying on `poll()` revents.

After reinstalling, the 3DS showed **Connected**, accepted the TV's "Allow USB debugging?" prompt, and controlled the Fire TV.

### 6. Lag: repeated presses replayed slowly and couldn't be cancelled

Pressing a button several times quickly worked, but the TV fell behind and then slowly caught up on every press, even after the buttons were released.

**Cause:** each `input keyevent` starts a small Java process on the Fire TV, which takes roughly 0.5–1 s. adbd acknowledges a write (`OKAY`) as soon as it *receives* the bytes, not when the command has *finished*. So the app thought the TV was ready and sent every press as its own command. The commands piled up in the TV's shell, and nothing could merge or cancel them.

**Fix** (`3ds/source/adb.c`, `main.c`):
1. Every command ends with `; echo D$((6*7))N`. The shell prints `D42N` when the command is done, and only then is the next command sent. The echoed command line contains the unexpanded `D$((6*7))N`, so the terminal echo can't be mistaken for "done".
2. Presses made while the TV is busy wait on the 3DS and are **merged** into one `input keyevent a b c` call, which costs a single process start.
3. **At most 6 taps** can wait. Extra mashing is dropped instead of replayed later.
4. **Auto-repeat from a held button only fires when the TV has caught up**, so holding a direction scrolls as fast as the TV can go and stops the moment the button is released.
5. If the marker never arrives, the app carries on after 4 s instead of freezing.

The fake TV in `tests/` now behaves like the real one. It echoes the command line, takes time to run each command, and fails the test if the client sends another command before the previous one finishes.

## Status

| | |
|---|---|
| Host tests against the fake Fire TV (including the lag fix) | ✅ |
| Builds with real devkitARM (Windows/MSYS2) | ✅ |
| `.cia` packaged and installed with FBI | ✅ |
| Connects to and controls a real Fire TV from an original 3DS | ✅ |
| CI build on GitHub Actions | runs on every push |

## Known issues

- The **Menu** (☰) button doesn't seem to do anything on some screens. The app sends `KEYCODE_MENU` (82), which Amazon documents as the remote's menu key; still investigating.

## Ideas for later

- A second page of app shortcuts (Prime Video, Disney+, Crave…)
- Auto-reconnect when the TV wakes from sleep
- Several Fire TVs with a switcher
