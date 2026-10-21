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

## Limits

- **Cooperative only.** It hears drones that broadcast Remote ID. A drone
  without it (older, home-built, or with it turned off) isn't detected.
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
