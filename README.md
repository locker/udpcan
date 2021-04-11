udpcan
======

udpcan is a simple CAN <-> UDP converter. It takes one or more arguments in the
format `CAN_IFACE:IN_PORT:OUT_HOST:OUT_PORT`. For each argument, udpcan will:

1. Forward all UDP packets sent to UDP port `IN_PORT` to CAN interface
   `CAN_IFACE`.

1. Forward all CAN frames sent to CAN interface `CAN_IFACE` to UDP port
   `OUT_PORT` on host `OUT_HOST`.

For sending over network, a CAN frame is serialized as 4-byte CAN id in network
byte order followed by up to 8 bytes of CAN data, e.g. CAN frame `123#DEADBEEF`
would be sent as a UDP packet with `00000123deadbeef` payload. If the payload
of a UDP packet received over network is longer than than 12 bytes (4 byte CAN
id + 8 byte data), it will be truncated before sending to CAN. If the payload
is shorter than 4 bytes, the packet will be dropped.

udpcan was written solely for educational purposes and should not be used for
any other purposes other than such.

Example usage
-------------

Create one or more virtual CAN device:

```
$ modprobe vcan
$ ip link add dev vcan0 type vcan
$ ip link set up vcan0
$ ip link add dev vcan1 type vcan
$ ip link set up vcan1
```

Start udpcan:

```
$ udpcan vcan0:8880:127.0.0.1:9990 vcan1:8881:127.0.0.1:9991
```

Send some CAN frames while listening on `OUT_PORT`s:

```
$ nc -W1 -u -l 9990 | xxd -p
00000123deadbeef
$ nc -W1 -u -l 9990 | xxd -p
00000111abcd
$ nc -W1 -u -l 9991 | xxd -p
00000aaaaabb
```

```
$ cansend vcan0 123#DEADBEEF
$ cansend vcan0 111#ABCD
$ cansend vcan1 AAA#AABB
```

Send some UDP packets to `IN_PORT`s while monitoring CAN traffic:

```
$ candump vcan0 vcan1
  vcan0  011   [2]  AB AB
  vcan1  012   [2]  34 56
  vcan1  0AA   [1]  BB
```

```
$ echo 00000011ABAB | xxd -r -p | nc -q0 -u 127.0.0.1 8880
$ echo 000000123456 | xxd -r -p | nc -q0 -u 127.0.0.1 8881
$ echo 000000AABB | xxd -r -p | nc -q0 -u 127.0.0.1 8881
```

udpcan will print all forwarded packets to stdout:

```
$ ./udpcan vcan0:8880:127.0.0.1:9990 vcan1:8881:127.0.0.1:9991
vcan0:8880:127.0.0.1:9990: CAN->UDP: 123#DEADBEEF
vcan0:8880:127.0.0.1:9990: CAN->UDP: 111#ABCD
vcan1:8881:127.0.0.1:9991: CAN->UDP: AAA#AABB
vcan0:8880:127.0.0.1:9990: UDP->CAN: 011#ABAB
vcan1:8881:127.0.0.1:9991: UDP->CAN: 012#3456
vcan1:8881:127.0.0.1:9991: UDP->CAN: 0AA#BB
```
