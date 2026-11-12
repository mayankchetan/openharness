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
   the two numbers match, then tap [yes] on both. The app waits up to 60 seconds. If you refuse, or the time runs out, the app stops and
   the status says "Pairing failed - press Start to try again". It does not retry by itself, so a refused pairing does not prompt you
   again. Check the dial, then tap Start bridge.

The Android pairing (bond) with the dial belongs to the phone's Bluetooth stack, not to the app. It survives the uninstall and the
reinstall, so a dial that was paired before needs no new pairing.

Four actions erase the dial's bonds. Use any of them and the phone still holds the old bond:

- Forget on the dial (Settings > Phone > Forget).
- `harness dial phone forget`.
- `harness dial wifi forget`.
- The BOOT reset (the factory reset on the dial).

After any of them, remove the dial in Android Settings > Connected devices (Bluetooth) too, so the two sides agree, then pair again.
Android does not pair a device it still holds as bonded, so the dial's new pairing request never comes. If the bridge keeps failing
with a bonded phone, it stops and the status says "The dial does not know this phone any more - remove it in Android Bluetooth settings,
then press Start".

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

To remove the pairing, open Settings > Phone on the dial and use Forget. Then remove the dial in Android Bluetooth settings as well
(see the four actions under "Install the release build").

## Wi-Fi first, then the phone (auto mode)

If the dial also holds Wi-Fi settings, it can try your Wi-Fi first and use the phone only when Wi-Fi does not work. This is the mode for a
dial that is mostly at home but sometimes away.

1. Set up the phone and pair it first, as above. Pair it in phone-only mode (`harness dial phone on`). In auto mode the phone link starts only
   after a Wi-Fi attempt fails, and the dial cannot say how many phones it is paired with while Bluetooth is off, so Harness cannot check it
   for you.
2. Give the dial its Wi-Fi network (`harness dial wifi set`, see [the Wi-Fi note](dial-wifi.md)). The Wi-Fi settings and the phone settings
   share one key.
3. With the dial plugged in, run `harness dial net auto`. It refuses a dial that lacks one of the two and names the command that is missing.
   `harness dial wifi show` and `harness dial phone show` then say "auto: Wi-Fi first, then phone".
4. Unplug the cable.

What the dial does, in this order:

- Every start tries Wi-Fi first: after you unplug the cable, after the power chip switched the dial off and you pressed PWR, and after a
  power-on. A restart that Harness makes by itself during a switch does not change this rule.
- If the Wi-Fi network is not found twice in a row, or the dial is not connected to this computer after 30 seconds, it restarts on the phone.
- If this computer answers the dial's handshake with a refusal (a wrong key), the dial stays on Wi-Fi and shows "Plug in USB". The phone
  uses the same key, so it would be refused too.
- On the phone: if the phone link drops, the dial waits 30 seconds and then restarts to try Wi-Fi again. The second drop waits 60 seconds and
  the third 120 seconds. If the phone never connects, it tries Wi-Fi again 2 minutes after the phone link started. That is at most 3 returns to
  Wi-Fi; after the third the dial stays on the phone until the next start. A phone link that holds for 10 minutes clears the count.
- On Wi-Fi: if the link was up and then stays down for 2 minutes, the dial restarts on the phone. The time the radio is switched off to save the battery
  (after 5 minutes with the screen dark) does not count; the 2 minutes start when the radio is back on and the link is still not up. It does not go back to Wi-Fi by itself
  later, except by the rules above.
- Plugging the cable in works as before: after about 5 seconds, and 30 seconds after your last touch if a page, a question or a picker is open.
  It never restarts in the middle of a voice message, a draft, a firmware update or a pet upload. While a pairing number is on the glass it waits.

What you see on the dial:

- A small link mark at the lower right of the glass on every screen, whenever the dial is not on USB. A Wi-Fi mark means the Wi-Fi link
  runs. A phone mark means the phone link runs. It is dim while the dial is connected to this computer, and amber while it is not.
- Under the word "Harness", "Using phone" while the phone link is not up yet ("Joining Wi-Fi" is the Wi-Fi line).
- On the Link screen (this version has no gesture that opens it), "Link: Auto - Wi-Fi" or "Link: Auto - Phone".

Voice starts with the BOOT button on the dial only. Touching the glass does not start it.

To leave auto mode run `harness dial wifi on` (Wi-Fi only), `harness dial phone on` (phone only) or `harness dial wifi off` (USB only).
`harness dial phone off` is refused while the dial is in auto mode.

## Check that it works

- The app status line says "tunnel up" when the dial has a session with this computer.
- `harness dial phone show` tells you what the dial and this computer hold for the phone mode. It never shows the key.
- If the status says the tunnel refused the service token, fix the Client ID and Client Secret in Settings, then tap Start bridge again.

## Turn it off

- `harness dial phone off` makes the dial start on USB only. `harness dial phone on` turns it back on.
- `harness dial phone forget` clears the phone settings and the key on the dial and on this computer. The dial must be plugged in.
  It erases the dial's bonds too: remove the dial in Android Bluetooth settings afterwards.
- Tap Stop in the app (or in its notification) to stop the bridge. Set `CABLE_TUNNEL_ENABLE=false` to close the endpoint.
