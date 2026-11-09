# Building FireTV Remote

You don't need to build anything to *use* the app: download it from the [Releases](https://github.com/kaiwalyaharkare/3ds-firetv-remote-/releases) page. This guide is for changing the code or building it yourself.

Every push to GitHub also builds both files automatically (`.github/workflows/build.yml`). Download them from the run on the **Actions** tab, under the artifacts `firetv-remote-3dsx` and `firetv-remote-cia`. Pushing a tag like `v1.2.0` publishes a Release with both files attached.

**Contents**
- [What you need](#what-you-need)
- [Windows](#windows-msys2--devkitpro)
- [Linux / macOS](#linux--macos)
- [Docker (any OS)](#docker-any-os)
- [Packaging the .cia](#packaging-the-cia)
- [Sending a build to the 3DS](#sending-a-build-to-the-3ds)
- [Running the tests](#running-the-tests)

---

## What you need

| Tool | Needed for | Where it comes from |
|---|---|---|
| devkitARM, libctru, citro2d, citro3d, 3ds-mbedtls | `.3dsx` | [devkitPro](https://devkitpro.org/wiki/Getting_Started) |
| `makerom` | `.cia` | [3DSGuy/Project_CTR releases](https://github.com/3DSGuy/Project_CTR/releases) |
| `bannertool` | `.cia` | [diasurgical/bannertool releases](https://github.com/diasurgical/bannertool/releases) |

---

## Windows (MSYS2 + devkitPro)

The official `devkitProUpdater` installer may install nothing when run silently. This route is reliable and fully scriptable.

**1. Install MSYS2** (PowerShell):

```powershell
winget install --id MSYS2.MSYS2 -e
```

**2. Add the devkitPro package repos.** Open **MSYS2 MSYS** from the Start menu and run:

```bash
pacman-key --init
pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0

# pkg.devkitpro.org is behind Cloudflare and answers plain curl with 403.
# A pacman user-agent gets the real file instead of an HTML error page.
curl -fsSL -A "pacman/6.1.0 (MSYS_NT-10.0 x86_64) libalpm/15.0.0" \
     -o /tmp/devkitpro-keyring.pkg.tar.zst \
     https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
pacman -U --noconfirm /tmp/devkitpro-keyring.pkg.tar.zst

cat >> /etc/pacman.conf <<'EOF'

[dkp-libs]
Server = https://pkg.devkitpro.org/packages

[dkp-windows]
Server = https://pkg.devkitpro.org/packages/windows/$arch/
EOF
```

**3. Install the 3DS toolchain and libraries:**

```bash
pacman -Sy
pacman -S --needed make 3ds-dev citro2d citro3d 3ds-mbedtls
```

Everything lands in `/opt/devkitpro` (which is `C:\msys64\opt\devkitpro`).

**4. Build.** Close and reopen the MSYS2 window so the environment variables load, then:

```bash
cd /c/path/to/3ds-firetv-remote-
make -C 3ds
```

The result is `3ds/firetv-remote.3dsx`. If you get `DEVKITARM not set`, run `source /etc/profile.d/devkit-env.sh` first.

---

## Linux / macOS

Install devkitPro pacman by following [devkitPro's Getting Started guide](https://devkitpro.org/wiki/Getting_Started), then:

```bash
sudo dkp-pacman -S --needed 3ds-dev citro2d citro3d 3ds-mbedtls
make -C 3ds
```

## Docker (any OS)

This is the same container the CI uses:

```bash
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm:latest \
  sh -c "dkp-pacman -Sy --noconfirm --needed citro2d citro3d 3ds-mbedtls && make -C 3ds"
```

---

## Packaging the .cia

The `.cia` adds a HOME Menu icon and banner. Build the `.3dsx` first (that also produces `3ds/firetv-remote.elf`), then download the two tools:

- **makerom:** from [Project_CTR releases](https://github.com/3DSGuy/Project_CTR/releases), get the zip for your OS (tested with `makerom-v0.19.0-win_x86_64.zip`)
- **bannertool:** from [diasurgical/bannertool releases](https://github.com/diasurgical/bannertool/releases), get `bannertool.zip` and use the folder for your OS (e.g. `windows-x86_64/`)

Either put both on your `PATH` and run `make -C 3ds cia`, or point the script at them:

```bash
MAKEROM=/path/to/makerom BANNERTOOL=/path/to/bannertool \
  bash 3ds/cia/make_cia.sh 3ds/firetv-remote.elf 3ds/firetv-remote.cia
```

Set `APP_VERSION=1.2.0` to stamp a version into the CIA.

---

## Sending a build to the 3DS

**3dslink (fastest while developing).** In the Homebrew Launcher press **Y**, then on the PC:

```bash
3dslink 3ds/firetv-remote.3dsx
```

**ftpd.** Open ftpd on the 3DS and note the address on the top screen, then:

```bash
curl -T 3ds/firetv-remote.cia  "ftp://<3DS-IP>:5000/cias/firetv-remote.cia"
curl -T 3ds/firetv-remote.3dsx "ftp://<3DS-IP>:5000/3ds/firetv-remote.3dsx"
```

To check that the upload is intact, download it back and compare hashes (`sha256sum` on Linux/macOS, `Get-FileHash` in PowerShell). **A 426 error or a 0-byte file on the 3DS means the SD card is full.** See [DEVLOG.md](DEVLOG.md#3-uploads-arrived-as-0-byte-files).

---

## Running the tests

`tests/run.sh` compiles the same `adb.c` the 3DS uses for your PC and drives it against `tests/mock_adbd.py`. That's a fake Fire TV that performs real RSA verification of Android's public-key format and enforces ADB flow control. It needs Linux or WSL:

```bash
sudo apt install gcc python3-pip libmbedtls-dev   # mbedtls 2.28
pip install cryptography
./tests/run.sh                                    # prints PASS
```
