# Yale Smart Door Lock Component
This component implements a custom lock component that uses a UART bus and an additional output to interact with a Yale smart lock by emulating a "smart" module. Such modules come in a few flavours including ZigBee, ZWave, and RF and all communicate with the lock electronics with a serial protocol. I reverse engineered enough of this protocol (write-up coming one day) to monitor events and send a few commands; lock/unlock being the most useful.


## Pinout
The pinout of my lock is shown below. This is seen from the header on the lock electronics (i.e. any connection into this should be mirrored). Locate `GND` by testing for continuity with battery negative and that should give you the correct orientation.

```
 VIN  (1) (2)  TX
  RX  (3) (4)  RST
BUSY  (5) (6)  WAKE
 ENA  (7) (8)  GND
```

I'll double check these pins at some point. My notes are sketchy (and anything I'm unsure about is noted with a (?)), but here's a breif summary:
1. `VIN` is 3.3V supply. Note that you will probably drain the batteries if drawing power from here.
2. `TX` I *think* this is transmitting from the **lock to the module**.
3. `RX` I *think* this is transmitting from the **module to the lock**.
4. `RST` unused (?).
5. `BUSY` pulsed lock by the lock to wake the module (?).
6. `WAKE` pulsed low by the module to wake the lock, must be done before transmission. Pulse duration seems pretty flexible.
7. `ENA` unused (?).
8. `GND` 0V.


## Configuration Variables
The compoment is implemented as a `lock` with the platform `yale` and inherits all configuration options listed in the [documentation for that component](https://esphome.io/components/lock/index.html).

The following additional options are available:
- **uart_id (Required, [ID](https://esphome.io/guides/configuration-types.html#config-id)):** Component ID of the UART busused for communication with the lock electronics.
- **output (Required, [ID](https://esphome.io/guides/configuration-types.html#config-id)):** Component ID of the output used to send pulses to the `WAKE` pin of the lock electronics.
- **event (Optional, [text_sensor](https://esphome.io/components/text_sensor/index.html))** If supplied an extra `text_sensor` component will be created that publishes event data when received, detailed below.
- **low_battery (Optional, [binary_sensor](https://esphome.io/components/binary_sensor/index.html))** If supplied an extra `binary_sensor` component will be created that turns on when a low-battery event/flag is observed.


## Event Text Sensor
The lock sends a packet when certain events occur, such as locking and unlocking using different mechanisms. Check the code for all the events, here are a few examples:

### Locked Manually
PIN-pad touched while unlocked.

```json
{"state":"LOCKED","source":"self"}
```

### Unlocked via PIN
`slot` matches the user/slot number configured when adding the PIN. `0xF0` is the PIN configured for a *temporary* user and `0xFB` is the master PIN.

```json
{"state":"UNLOCKED","source":"pin","slot":1}
```

### Unlocked via NFC
```json
{"state":"UNLOCKED","source":"nfc","slot":1}
```


## Limitations
This implementation is far from complete and only implements functions I was able to observe. There are a few limitation and limits of the component as currently implemented:

- The discovery sequence of modules is time-based and only seems to work within the first 10 seconds or so after poweron. If for watever reason the module is disconnected (such as via a hardware reset of the ESP) then the lock may discide the module has been removed and stop responding to commands, you can tell this has happened if the lock plays its startup "tune" again.
- The `low_battery` binary sensor is only ever turned on when a low-battery packeted is observed. Since chenging the battery requires power cycling anyway this probably isn't a big deal.
- It's possible for the lock and this component to get out of sync with oneanother. I beleive this occurs if the ESP resets for some reason (such as loss of Wi-Fi or API access). There is a provision for occasional polling of the lock status, but if the discovery process is missed this won't work and will require everything to be power cycled.


## Example Implementation
```yaml
# Import this component
external_components:
  - source: github://ravngr/esphome-components
    components:
      - yale

# GPIO to wake Yale lock
output:
  - platform: gpio
    id: wake_out
    pin: 22
    inverted: true

# ESP32 can emaulate UART on any pin pair, ESP82xx should probably use hardware UART pins (untested)
uart:
  id: uart_bus
  tx_pin: 25
  rx_pin: 21
  baud_rate: 19200

# The actual lock component
lock:
  - platform: yale
    id: yale_lock
    name: "Yale Smart Door Lock"
    uart_id: uart_bus
    output: wake_out
    event:
      name: "Yale Lock Event JSON"
    low_battery:
      name: "Yale Lock Low-Battery"
```


## Bonus: Home-Assistant Template Sensor for JSON Events
This translates the JSON events from the `event` sensor into readable text. I use this with the [logbook-card](https://github.com/royto/logbook-card) to present a nice event log.

```yaml
template:
  - sensor:
        - name: "Door Lock Event"
            unique_id: "xxx"
            icon: "mdi:account-check"
            availability: >
              {{
                not is_state('sensor.door_json', ['unavailable', 'unknown'])
                and states('sensor.door_json').startswith('{')
                and states('sensor.door_json').endswith('}')
              }}
            state: >-
                {%- set event = states('sensor.door_json') | from_json -%}
                {%- set slot_pin = {
                  1: 'user PIN',
                  2: 'guest PIN',
                  0xF0: 'temporary PIN',
                  0xFB: 'master PIN'
                } -%}
                {%- set slot_nfc = {
                  1: 'user NFC',
                  2: 'guest NFC'
                } -%}
                {%- set event_state = event.get('state', 'unknown').title() -%}
                {%- set event_source = event.get('source', 'unknown').lower() -%}
                {%- set event_slot = event.get('slot', 0) -%}

                {%- if event_source == 'nfc' -%}
                {{ event_state }} by {{ slot_nfc.get(event_slot, 'unknown NFC') }}
                {%- elif event_source == 'pin' -%}
                {{ event_state }} by {{ slot_pin.get(event_slot, 'unknown PIN') }}
                {%- elif event_source == 'user' -%}
                {{ event_state }} manually
                {%- elif event_source == 'timeout' -%}
                {{ event_state }} automatically
                {%- else -%}
                {{ this.state }}
                {%- endif -%}
```
