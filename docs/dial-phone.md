# The dial on your phone

The dial normally talks to this computer through its USB cable. In phone mode it talks to your phone by Bluetooth. The phone app
passes the bytes on to this computer through a Cloudflare Tunnel. The phone cannot read them: they are encrypted from the dial to this
computer.

The wire format is in [the BLE link specification](specs/cable-ble-link.md). This page is for the person at the desk.

## Before you start

- You need a Cloudflare account with a domain, a tunnel, an Access application and a service token. The steps are in the runbook
  `briefs/research/2026-11-01-cloudflare-runbook.md` in the workbench repository. Do those steps first. At the end you have a
  host name, an Access Client ID and an Access Client Secret.
- You need a dial with a firmware build that has the Bluetooth link, and an Android phone (Android 12 or newer).
- The tunnel endpoint on this computer is off by default. Turn it on (below).

## Set it up

1. On this computer, set `CABLE_TUNNEL_ENABLE=true` and restart Harness (`harness start`). The endpoint listens on this computer only.
   Cloudflare reaches it through the tunnel.
2. Plug the dial in with its USB cable.
3. Run `harness dial phone set`. No password is asked. This computer makes a key, sends it to the dial over the cable and keeps its own
   copy. The key is never printed. The same command turns the phone mode on. To get a new key later, run it again: the old key stops working.
4. Unplug the cable.
5. Install the phone app: `adb install phone-bridge-debug.apk` (the file is in `devices/harness-device/phone-bridge/build/outputs/apk/debug/`
   after `gradlew assembleDebug`).

   For daily use install the signed release build instead (see "Install the release build" below). The debug build allows an unbonded
   channel and talks to `127.0.0.1` for development; the release build always pairs first and talks to the Cloudflare host.
6. Open the app and tap Settings. Fill in three fields, then tap Save:
   - Relay URL: `wss://<your host name>/relay`
   - Cloudflare Access Client ID
   - Client Secret

   The app keeps the secret encrypted on the phone. Never put it in the URL, and never type it anywhere else.
7. Go back and tap Start bridge. Allow "Nearby devices" and notifications when Android asks.

## Install the release build

The release build is signed with a personal key. The key file and its password stay outside the repository, in a folder you keep private
(here `C:\Users\<you>\.harness\bench\android-release\`). Back that folder up: a later update must use the same key.

1. Build it. In `devices/harness-device/phone-bridge` run, with JDK 21:
   `set PHONEBRIDGE_SIGNING=<path to keystore.properties>` and then `gradlew assembleRelease`. The properties file has
   `storeFile`, `storePassword`, `keyAlias` and `keyPassword`. Without `PHONEBRIDGE_SIGNING` the build still works but gives an unsigned
   APK, which Android will not install. The file is `build/outputs/apk/release/phone-bridge-release.apk`.
2. Uninstall the debug app first (`adb uninstall dev.openharness.phonebridge`, or remove it in Android Settings). The debug build and the
   release build have different signatures, so Android will not update one with the other.
3. Install the release APK: `adb install phone-bridge-release.apk`.
4. The uninstall wiped the app's stored Cloudflare Access token. Push it again from this computer with the token script in the
   workbench repository: `scripts\dial\push-access-token.ps1`. Or open Settings in the app and type the three fields again.
5. Tap Start bridge. If the phone is not bonded with the dial yet, the app starts the pairing itself: Android shows a 6-digit number, the dial
   shows one too. Open the dial's Phone screen first so its pairing window is open (it opens by itself when the dial has no bond). Check that
   the two numbers match, then tap [yes] on both. The app waits up to 60 seconds. If you refuse, or the time runs out, the status says
   "Pairing refused or timed out - check the dial", and the app tries again after a pause.

The Android pairing (bond) with the dial belongs to the phone's Bluetooth stack, not to the app. It survives the uninstall and the
reinstall, so a dial that was paired before needs no new pairing. If you used Forget on the dial, remove the dial in Android Settings >
Connected devices too, so the two sides agree, then pair again.

## Pair the dial with the phone

Pairing starts on the dial, not on the phone. You do it once.

1. On the dial open Settings > Phone > Pair. The dial shows "Waiting for phone" and a countdown of 2 minutes.
2. The bridge must be running on the phone (Start bridge). Android asks to pair with the dial.
3. The dial shows "Phone code" and a 6-digit number, and asks "Same on the phone?". Android shows a number too.
4. If the two numbers match, tap [yes] on the dial and accept on the phone. If they differ, or your phone shows no number, tap [no]. The dial says
   "Press No unless your phone shows this number" under the number, because a dial with no paired phone opens the pairing window by itself when it
   starts the phone mode, and a number can come from a phone that is not yours. After a [no] the window stays open to the end of its 2 minutes, so you
   can start again from the phone.
5. The dial now shows "Paired". When the phone has the link it shows "Connected".

To remove the pairing, open Settings > Phone on the dial and use Forget.

## Check that it works

- The app status line says "tunnel up" when the dial has a session with this computer.
- `harness dial phone show` tells you what the dial and this computer hold for the phone mode. It never shows the key.
- If the status says the tunnel refused the service token, fix the Client ID and Client Secret in Settings, then tap Start bridge again.

## Turn it off

- `harness dial phone off` makes the dial start on USB only. `harness dial phone on` turns it back on.
- `harness dial phone forget` clears the phone settings and the key on the dial and on this computer. The dial must be plugged in.
- Tap Stop in the app (or in its notification) to stop the bridge. Set `CABLE_TUNNEL_ENABLE=false` to close the endpoint.
