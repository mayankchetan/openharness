# The dial on Wi-Fi

The dial normally talks to this computer through its USB cable. It can also talk over your home Wi-Fi network. You give it the
network once, with the cable in. After that, when you unplug the cable, the dial joins the network and connects to this computer by itself.
When you plug the cable back in, it returns to USB.

The wire format is in [the cable link specification](specs/cable-wifi-link.md). This page is for the person at the desk.

## Before you start

- The dial must use a 2.4 GHz network. It cannot join a 5 GHz network. If your router gives one name to both bands, that is fine: the dial
  can only use the 2.4 GHz side.
- This computer and the dial must be on the same local network.
- This computer's address on the network must not change. Reserve it in your router: look for "DHCP reservation" or "static lease"
  and give this computer's address to this computer. If the address changes while the dial is on Wi-Fi, the dial cannot find this computer.
  Plug the cable in and run `harness dial wifi set` again.
- On Windows, set the network to "Private", and allow the connection through the firewall once (below).

## Set it up

1. Plug the dial in with its USB cable and start Harness (`harness start`).
2. Run `harness dial wifi set`. It asks for the network name and the password. The password is typed with nothing shown. It is never an
   argument of the command, and this computer does not keep it. The dial keeps it.
3. On Windows, run the firewall command once, in a terminal opened as an administrator. `harness dial wifi firewall` prints it. Harness does
   not run it for you:

   ```
   netsh advfirewall firewall add rule name="Harness dial" dir=in action=allow protocol=TCP localport=18480 profile=private remoteip=LocalSubnet
   ```

4. Unplug the cable. After about 10 seconds the dial joins the network and connects.

## What the dial shows while it is not connected

While the dial has no link to this computer it shows the word "Harness". On Wi-Fi it adds one line under it:

| Line | Meaning | What to do |
|---|---|---|
| Joining Wi-Fi | It is working on it: finding the network, getting an address, or reaching this computer. | Wait. |
| Wi-Fi: no address | It joined the network but the router gave it no address. | Check the router. |
| Daemon not reachable | It has an address, but this computer did not answer. | Is Harness running here? Is the firewall rule in place? Did this computer's address change? |
| Plug in USB | This computer refused the dial's key: it does not know this dial, or holds a different key. Keep Wi-Fi is off. | Plug the cable in. The dial restarts on USB after about 5 seconds, once it has run for a minute. Run `harness dial wifi set` again. |
| Open Link screen | The same, with Keep Wi-Fi on: the dial does not look at the cable. | This version has no on-dial control for Keep Wi-Fi, so this line shows only on a dial that an earlier test build set to Keep Wi-Fi on. Hold BOOT while the dial starts, until it says "Settings reset" (this erases its Wi-Fi settings), then plug the cable in and run `harness dial wifi set` again. |

When the link is up the line goes away.

## Choosing Wi-Fi or USB

The dial has no screen for this in this version, and no gesture on the glass opens one (an earlier build opened a Link screen with a swipe up from the
bottom edge; that was removed). Choose from this computer, with the dial plugged in:

- `harness dial wifi on`: the dial joins the network when you unplug the cable.
- `harness dial wifi off`: the dial stays on USB, whatever the cable does.

A dial set up for the phone (`harness dial phone on`) is in the phone mode, not in Wi-Fi mode: only one of
them is on at a time, and `harness dial wifi on` or a new `harness dial wifi set` turns the phone mode off.

## Long idle on the battery

In the image built with `DEVICE_BATTERY_POWEROFF`, a dial that has been idle in light sleep for 30 minutes on its battery closes its link to this computer
on purpose (it says goodbye, then closes the TLS connection, so the daemon log shows "said goodbye (power-off)" and a clean close), and then the power
chip switches the dial off. It also switches off at 5 percent or when the battery falls fast. It never does this on a charger or with a cable in. Hold the
PWR button to bring it back: it starts in Wi-Fi mode as before and reconnects in about 5 seconds.

## When the cable goes in

With Keep Wi-Fi off, a Wi-Fi dial that is plugged into a computer that runs Harness goes back to USB after about 5 seconds, then restarts. It never restarts in the middle of a voice message, a draft, a firmware update or a pet upload: it waits until they end. If only a page, a question or a picker is open, it shows "Switching in a moment" and restarts 30 seconds after your last touch (each touch starts the 30 seconds again). A dial whose screen is off restarts without the notice.

## Other commands

| Command | What it does |
|---|---|
| `harness dial wifi show` | What the dial holds and what this computer knows. It never shows a password or a key. |
| `harness dial wifi off` | The dial stays on USB when the cable is unplugged. The network stays saved. |
| `harness dial wifi on` | The dial joins Wi-Fi when the cable is unplugged. |
| `harness dial wifi rotate` | A new key for the dial. The old key stops working. |
| `harness dial wifi forget` | The dial and this computer forget the network and the key. |
| `harness dial wifi firewall` | Print the Windows firewall command. |

Each command takes `--dial <MAC>` to name the dial. The commands that change the dial need it plugged in with the cable.

## Limits

- Firmware updates never go over Wi-Fi. They need the cable.
- Holding the BOOT button while the dial powers on is a factory reset. It removes the network and the key.
