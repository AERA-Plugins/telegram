# AERA Telegram Plugin

A recovery-native Telegram client for AERA Recovery Project. It uses TDLib and
a native LVGL surface; it is not an Android APK and does not require Binder,
ART, AndroidX, Google services, or the Android application framework.

The signed plugin contains only the isolated ARM64 musl TDLib worker and its
runtime libraries. AERA verifies all payload metadata before expansion, runs it
as UID 99091 inside a chroot/minijail, and exposes only a bounded packet channel,
network client sockets, temporary memory, and `/data/recovery/AERA/telegram`.
The TDLib database encryption key is entered in the trusted recovery UI and is
never logged or stored as plaintext by the AERA host.

Version 1.0.0 supports phone/code/two-step login, email verification,
registration, chat listing, text history, receiving messages, and sending text.
Media rendering and calls are intentionally outside this first release.

Users must create their own Telegram application at `my.telegram.org` and enter
its API ID/hash during setup. AERA does not embed credentials from another app.

## Upstream

- TDLib commit `d1085f9cebc5a62379991ae1652673954f229c1f`
- TDLib is distributed under the Boost Software License 1.0.

## Build

Cross-build the pinned TDLib revision for ARM64 musl with OpenSSL, zlib, C++17,
LTO and `MinSizeRel`, then set `TDLIB_SOURCE`, `TDLIB_BUILD`, `AERA_STRIP`, and
run:

```sh
source/build-runtime.sh stage
python3 source/pack.py stage build
```

Update `plugin.json` from `build/metadata.json`, sign the exact bytes with the
AERA Ed25519 release key, and attach `runtime.xz` to the matching GitHub release.
Never commit the private signing key.
