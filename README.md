cec-mqtt-bridge generates MQTT events on state changes from CEC events

## Requirements

- [libcec](https://github.com/Pulse-Eight/libcec). In addition to
  the library, you will need to clone the project, check out the right
  version tag, and set the value of LIBCEC_INCLUDE_DIR correctly, at least
  until [libcec#397](https://github.com/Pulse-Eight/libcec/pull/397) is
  fixed.
- [libmosquitto](https://mosquitto.org/)
- [jansson](https://github.com/akheron/jansson)

## Building

    $ make LIBCEC_INCLUDE_DIR=$HOME/src/libcec/include
    $ sudo make LIBCEC_INCLUDE_DIR=$HOME/src/libcec/include install



## Use

There are two important command line arguments:

    --mqtt-broker: The IP address of your MQTT broker (hostnames are not currently supported)
    --mqtt-topic: The MQTT topic to publish to

A simple invocation would be:

    cec-mqtt-brdige --mqtt-broker 192.168.1.5 --mqtt-topic media/living_room_tv


## Payload

The basic payload of the MQTT topic is a JSON object with two parameters:
`power_state` and `hdmi_input`. `power_state` will either be `on` or `standby`,
in the convention used by libcec. `hdmi_input` will be an integer, and will be 0
if the input is not currently known.

```javascript
{
  "power_state": "standby",
  "hdmi_input": 0
}
```

## Caveats

This is pretty much only useful if you have a Sony Bravia TV right now. It makes
some assumptions about state transitions that are unlikely to hold for other TVs.
For example, Bravia TVs seem to broadcast a Vendor Command on on to standby transitions
that is the most reliable method of detecting when the TV goes to standby. Users interested
in adding support for other TVs should run with debug on (`-d/--debug`) and watch the
messages sniffed by the CEC library while turning the TV off and on, switching inputs, etc,
to find the best CEC message(s) to determine state transitions for their TV model. 
