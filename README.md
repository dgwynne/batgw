# Battery Gateway

This software acts as a "gateway" that translates messages between an
EV battery and a solar inverter.

This currently only supports translating between a BYD Atto 3 Extended
Range (60KWh) battery pack and the BYD "Battery-Box Premium HVS"
protocols.

*WARNING* this probably only works for the author.

## Platform

This uses a Raspberry Pi running Raspberry Pi OS as the software platform
for building and running the `batgw` software.

A variety of peripherals allow an RPi to be connected to CAN or RS485
interfaces, eg:

- https://www.waveshare.com/rs232-rs485-can-board.htm
- https://www.waveshare.com/2-ch-can-fd-hat.htm
- https://www.waveshare.com/rs485-can-hat-b.htm

In particular, the SocketCAN subsystem in Linux provides a standardised
programming interface for talking to CAN devices, and tools that assist
in the observation and debugging of this communication.

## Software Design

The program prioritises making communication between the battery and
inverter as robust as possible.

`batgw` uses libevent to handle multiplexing of communication between
the two CAN interfaces.

The program can be configured to connect to an MQTT broker and report
metrics from the battery and inverter.

## Building

This requires:

- `bmake`
- `libevent-dev`
- `libbsd-dev`

## Configuration

Something like this in `/etc/batgw.conf`

```
# the mqtt config is optional
mqtt {
	host "mqtt.local"
	# teleperiod 60
}

battery {
	protocol byd
	interface can0
}

inverter {
	protocol byd
	interface can1
}
```

## Usage

```
usage: batgw [-nv] [-f batgw.conf]
```

## But why?

I wanted to understand how this stuff worked.
