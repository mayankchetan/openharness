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
| Key refused: reset dial | This computer does not know this dial, or holds a different key. | Plug the cable in. The dial restarts on USB after about 5 seconds, once it has run for a minute. Run `harness dial wifi set` again. |

When the link is up the line goes away.

## The Link screen on the dial

Swipe up from the very bottom of the glass (start on the lowest part of the circle and drag up a thumb's width) to open the Link screen. It works on
the "Harness" screen as well as on the agent face, so you can reach it even when the dial cannot find this computer. The same swipe, a sideways
swipe or the arrow at the top closes it. It shows two rows when the dial has a Wi-Fi network:

| Row | Tap | Meaning |
|---|---|---|
| Link: Wi-Fi, Link: USB only | Switch | Wi-Fi: the dial joins the network when the cable is unplugged. USB only: it never leaves USB. If the dial is on Wi-Fi when you pick USB only, it restarts into USB (it waits while you are in the middle of something). |
| Keep Wi-Fi: on, Keep Wi-Fi: off | Switch | On: the dial stays on Wi-Fi even with the cable plugged in (the cable then only charges it). Off: plugging in the cable brings it back to USB. |

Under the rows it shows one line of link state: Wi-Fi: off, joining, no address, daemon not reachable, key refused, or connected. A dial with no
Wi-Fi network shows how to set one up instead of the rows.

If you turned Keep Wi-Fi on and want USB back, open the Link screen and turn it off. The dial does not look at the cable while Keep Wi-Fi is on.

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
