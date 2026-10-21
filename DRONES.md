# Drone Detector

A Remote ID drone detector for Wadamesh standalones. App drawer > **Drones**.
It runs on every board that has Flock You (Wi-Fi and NimBLE: T-Deck,
ThinkNode M9, the CrowPanels...) and shares Flock You's radios.

Remote ID (ASTM F3411 / Open Drone ID) is the broadcast most drones made since
2023 send by law. It carries the drone's serial or session ID, its position,
height, speed and heading, and usually where its operator stands. The
standalone listens for it two ways:

- **Wi-Fi:** beacons carrying the ASTM vendor element (OUI `FA:0B:BC`) and NAN
  frames, while Wi-Fi hops channels 1-13.
- **Bluetooth:** legacy adverts with service data `0xFFFA`. These carry one
  message each, so the standalone remembers which ID each address sent and
  matches the drone's position adverts to it.

The decoding is GridDown Secure Messenger's Remote ID module (BlackAtlas LLC,
GPL-3.0), vendored unchanged in `src/ui-touch/remoteid.cpp`. Its own test
suite runs in CI. The Wadamesh glue is `src/ui-touch/DroneCore.h`.

## What it does

1. **Lists the drones.** **Live** shows each drone heard: its ID and type, its
   distance and direction from you, height, speed, its operator's distance, the
   signal strength and how long ago it was heard. A drone not heard for a minute
   goes grey; after five minutes it's dropped. Tap a drone for the details: its
   operator ID, altitude, heading, and whether it was heard over Wi-Fi, Bluetooth
   or both.
2. **Alerts you.** A new drone pops up with its ID, with a sound unless that's
   turned off in **Setup**.
3. **Puts them on the map.** Drones with a position show on the map like Team's
   Watch reports: an orange square with the drone's label, its operator as a
   small grey dot, and its direction of travel. They stay for Watch's time.
4. **Tells the mesh.** In **Setup**, pick an alert channel (for example
   `#dronewatch`; never Public) and turn on **Post drones there**. Each drone is
   posted at most once a minute, and again only after it moves 200 m:

   ```
   DRONE 1581F9DEV26590296G6G h120m 7.0m/s hdg90
   https://maps.google.com/?q=47.60378,-122.34052
   Op https://maps.google.com/?q=47.60531,-122.34016
   ```

   Other standalones (Team > Watch) and MeshCore Open TAK (Watch tab) that watch
   the channel put it on their maps, with the operator. **Show ... drones from
   others on the map** watches the channel from here. A drone's sheet also has
   **Post to the alert channel** for posting one by hand.

## 900 MHz control-link scan (optional, off by default)

Drones without Remote ID still talk to their pilot. **Setup > 900 MHz
control-link scan** looks for those links with the standalone's LoRa radio,
using GridDown's 900 MHz drone scanner (BlackAtlas LLC, GPL-3.0, its logic in
`src/ui-touch/Scan900.cpp`):

- 52 channels, 902-928 MHz in 500 kHz steps. Each gets a channel-activity check
  (SF6/BW500 for ELRS and Crossfire, SF10/BW125 for LoRaWAN-style LoRa) and
  signal-strength readings.
- After every sweep, a classifier sorts what it heard: **LoRa FHSS
  (ELRS/Crossfire)**, **GFSK telemetry (SiK/RFD900)**, **Fixed LoRa**,
  **LoRaWAN infrastructure** or **Unknown ISM**. It looks at which part of the
  band is busy (915.0-923.2 MHz carries no LoRaWAN), the spreading-factor mix,
  how widely the activity spreads, and how often each channel lights up.
- Each detection shows its strongest frequency, signal, a rough distance
  (free-space, assuming a 250 mW ELRS or 100 mW SiK transmitter), how many
  channels it uses, whether it's getting closer or farther, and for ELRS its hop
  rate. A new LoRa FHSS or GFSK telemetry link alerts like a new drone.
- The standalone's own mesh and radio2 frequencies, ±1.25 MHz, are left out, so
  its neighbours don't count. Board noise is measured in a first calibration
  sweep and left out too.

**It affects your mesh reception.** The LoRa radio can't sweep and listen to
the mesh at once. While this is on, the standalone sweeps for about a second in
every four, and for that second it can't hear or send on the mesh: messages
sent to it can be missed, and its own messages wait their turn. That's why it's
off by default, and turning it on asks you to confirm. Turn it off when you're
done.

It only runs on standalones with an SX1262 radio (T-Deck, the SX1262 T-LoRa
Pager, the P4 boards), and only when the mesh is on the US 902-928 MHz band. It
waits while Spectrum is open, a temporary radio is on, or the radio is busy
sending or receiving. These detections have no position, so they aren't posted
to the channel or put on the map.

Changes from GridDown:

- Its saved RF baseline survey isn't carried over, so ambiguous LoRa is always
  treated as infrastructure.
- Detections last 15 seconds instead of 10, since sweeps take turns with the
  mesh.
- Its telemetry rule also needs activity spread over at least 5 MHz, as its
  second rule already did. This stops a gateway's sub-band being called
  telemetry.
- Only drone links alert, not fixed LoRa.

## Limits

- **Remote ID is cooperative.** It hears drones that broadcast it. A drone
  without it (older, home-built, or with it turned off) is only found by the
  optional 900 MHz scan above, and only if its control link is on 902-928 MHz.
- **Wi-Fi is shared.** Wi-Fi listens only while the standalone isn't connected
  to a network and no access point is up, the same rule as Flock You. When a
  network is saved, it steps aside every 45 seconds so the network can
  reconnect. Bluetooth listens in 4-second windows with 2-second rests, and not
  while the Bluetooth keyboard is pairing.
- **Wi-Fi beacons and NAN only.** Bluetooth 5 long-range adverts aren't
  received. Drones that broadcast only that way are missed.
- Remote ID's positions are whatever the drone reports; nothing here verifies
  them.

Keep your local rules about recording and sharing other people's positions in
mind. Remote ID is a public broadcast, but an operator's location is still
someone's location.
