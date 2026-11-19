# pico-w-ds18b20-mqtt — publish DS18B20 temperature readings from a Raspberry Pi Pico W over MQTT

IoT example by [Fluidwire](https://fluidwire.com/).

## What it does

Reads one or more **DS18B20** 1-Wire temperature probes from a Raspberry Pi
Pico W and publishes the readings to an MQTT broker every 30 seconds as a
small JSON payload. The first probe goes to `MQTT_TOPIC`; if you hang several
probes on the same data pin, each is also published to `MQTT_TOPIC/<index>`
tagged with its 64-bit ROM address so you can tell *tank inlet* from *tank
outlet*. A retained `online` / `offline` status topic (MQTT Last Will) lets a
dashboard notice when the node dies.

The DS18B20 is the cheapest waterproof-capable digital thermometer around
(the stainless probe version is PHP 40–80 at any Raya/Lazada shop), and the
Pico W is the cheapest WiFi board with a real Raspberry Pi ecosystem. Together
they make a ~PHP 400 multi-point temperature logger for aquaculture tanks,
fridges and freezers, server closets, compost piles, or a thesis rig that
needs more than one probe.

Three things this sketch gets right that the usual copy-paste example doesn't:

- **Non-blocking conversions.** A 12-bit DS18B20 conversion takes up to
  750 ms. The naive `requestTemperatures()` call blocks for all of it, which
  starves `mqtt.loop()` and makes keepalives and subscriptions flaky. This
  sketch calls `setWaitForConversion(false)`, kicks off the conversion, keeps
  servicing WiFi/MQTT, and only reads the result once the time is up.
- **Address-based reads.** Probes are read by ROM address, not by index, so a
  probe that drops off the bus for a moment doesn't silently shift every other
  reading onto the wrong name.
- **Bad readings are dropped, not published.** `-127 °C` (disconnected) and
  `85 °C` power-on-reset values never reach your database.

Pairs with [`pico-w-mqtt-sensor`](https://github.com/fluidwire/pico-w-mqtt-sensor)
(same board, internal temperature) and any of the Fluidwire MQTT consumers.

## Required hardware

- 1× Raspberry Pi Pico W
- 1–4× DS18B20 temperature sensor (TO-92 part or waterproof stainless probe)
- 1× 4.7 kΩ resistor (one for the whole bus, between DATA and 3V3)
- Breadboard + jumper wires
- MQTT broker (public: `broker.hivemq.com`, `test.mosquitto.org`, or self-hosted)
- WiFi network (2.4 GHz — the Pico W has no 5 GHz radio)

## Board setup (one-time)

1. In Arduino IDE: **File → Preferences → Additional Boards Manager URLs**, add:
   ```
   https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
   ```
2. **Boards Manager** → search **"Raspberry Pi Pico/RP2040"** by earlephilhower → Install.
3. **Tools → Board → Raspberry Pi Pico W**.

## Required libraries

Install via the Arduino IDE Library Manager:

- **OneWire** by Jim Studt, Paul Stoffregen et al.
- **DallasTemperature** by Miles Burton
- **PubSubClient** by Nick O'Leary

`WiFi.h` ships with the arduino-pico core, so no extra install is needed for it.

## Wiring / setup

| DS18B20 wire          | Connect to                          |
|-----------------------|-------------------------------------|
| VDD (red)             | Pico **3V3 (OUT)** — physical pin 36 |
| GND (black)           | Pico **GND** — any GND pin           |
| DATA (yellow / white) | Pico **GP22** — physical pin 29      |

Add **one** 4.7 kΩ resistor between DATA and 3V3. Extra probes wire in
parallel on the same three lines — they share the single pull-up.

```
          3V3 ──┬──────────────┬─── DS18B20 VDD (red)
                │              │
              4.7kΩ            │
                │              │
GP22 (pin 29) ──┴──────────────┼─── DS18B20 DATA (yellow)
                               │
          GND ─────────────────┴─── DS18B20 GND (black)
```

Power the probe from 3V3, not VBUS/5 V — the Pico's GPIO is not 5 V tolerant.
Keep the bus under ~5 m on breadboard wiring; for long runs use cable and drop
the pull-up to 2.2 kΩ.

### Then

1. Copy `config.h.example` to `config.h`.
2. Fill in your WiFi credentials, MQTT broker, and topics.
3. Flash the board and open the Serial Monitor at **115200** baud.

Watch the data arrive from any MQTT client:

```
mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/picow/temp/#" -v
```

## Expected serial output

```
Fluidwire - Pico W DS18B20 temperature node
[1wire] data on GP22, 12-bit resolution
[1wire] devices on bus: 2
[1wire]   #0 = 28FF641E8D1C0123
[1wire]   #1 = 28FF9A2B8D1C0456
[wifi] connecting to your-wifi-ssid ... OK, ip=192.168.1.57
[mqtt] connecting to broker.hivemq.com:1883 as fluidwire-picow-ds18b20 ... OK
[mqtt] publish fluidwire/demo/picow/temp: {"temp_c":27.31,"sensor":"28FF641E8D1C0123","sensors":2,"uptime_s":1}
[mqtt] publish fluidwire/demo/picow/temp/0: {"temp_c":27.31,"sensor":"28FF641E8D1C0123","index":0}
[mqtt] publish fluidwire/demo/picow/temp/1: {"temp_c":26.88,"sensor":"28FF9A2B8D1C0456","index":1}
```

With a single probe you only get the first `publish` line each cycle. The
onboard LED blinks once per publish.

## Troubleshooting

- **`[1wire] devices on bus: 0`.** Nine times out of ten it's the pull-up:
  the 4.7 kΩ must sit between DATA and 3V3. Then check you're on GP22 (pin
  29, not pin 22) and that the probe wires aren't swapped — waterproof probes
  from different sellers use different colours; the datasheet pinout (flat
  side facing you: GND, DATA, VDD) is the only reliable guide.
- **Reads `85.00` on the first cycle.** That's the DS18B20's power-on value
  when it's read before a conversion completes. The sketch filters it; if you
  see it persistently, the probe is browning out — check the 3V3 wiring.
- **`read failed (disconnected? CRC error?)`.** Loose breadboard contact or
  a cable run too long for a 4.7 kΩ pull-up. Try 2.2 kΩ, or shorten the wire.
- **Counterfeit DS18B20s.** Many cheap probes are clones. Most work, but some
  misbehave at 12-bit resolution or in parasite-power mode. Drop
  `DS18B20_RESOLUTION` to `10` if readings are erratic.
- **`[wifi] ... not yet (status=...)`.** The Pico W only does 2.4 GHz. Make
  sure the SSID isn't 5 GHz-only and that there's no captive portal.
- **`[mqtt] ... FAIL rc=-2`.** Can't reach the broker. Check host/port and
  that your network allows outbound port 1883.
- **No serial output at all.** The sketch waits up to 3 s for the USB serial
  port and then carries on. Open the Serial Monitor *before* pressing reset,
  or power-cycle the board after opening it.
- **Readings freeze while WiFi reconnects.** Expected: the WiFi retry blocks
  for up to 8 s. If you need the LED/serial to stay lively during outages,
  shorten the wait in `ensureWiFi()`.

## Security note

This sample uses a plaintext MQTT connection (port 1883) on a public test broker
so it runs out of the box — fine for a demo, but anyone can read a public
topic. For anything real:

- Use a private broker with username/password auth (fill `MQTT_USER` /
  `MQTT_PASSWORD`).
- Use TLS — the Pico W supports `WiFiClientSecure` in the arduino-pico core;
  see the ESP32 companion
  [`esp32-mqtt-tls-secure`](https://github.com/fluidwire/esp32-mqtt-tls-secure)
  for the pattern.
- Choose hard-to-guess topic names.
- Keep your WiFi and broker credentials in `config.h`, which is git-ignored.

## License

MIT — see [LICENSE](LICENSE). Use it, fork it, ship it.

## Got stuck?

We build IoT and embedded hardware for a living. If you're prototyping something
and hit a wall, reach out at [fluidwire.com](https://fluidwire.com/).
