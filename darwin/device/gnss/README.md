# Virtio GNSS

`UML::GnssDevice` is the host end of the guest's Kernarium GNSS receiver, a
local experimental Virtio device (ID 65534, the backwards-growing ID after the
battery's 65535). The guest kernel registers it in Linux's GNSS class, so the
guest sees `/dev/gnssN` of type NMEA, which Android's GNSS HAL and gpsd read as
they read any receiver. The device carries fixes only: there are no satellites
to report, and the host's location never reaches the guest except through it.
It reuses the common vhost-user transport and builds on it alone.

The guest driver is `drivers/gnss/kernarium.c` in the shared kernel series
([kernel/common](../../../kernel/common/README.md)).

## Layout

| File | Owns |
|---|---|
| `protocol.hpp` | the device ID, the queues and the control message |
| `fix.hpp` | one position, its hold limits, and the fix source a test or the launcher supplies |
| `nmea_writer.{hpp,cpp}` | fix to sentences: the epoch |
| `virtio_gnss_device.{hpp,cpp}` | the `VhostUserDevice`: power, the epoch timer, delivery |
| `endpoint.{hpp,cpp}` | the thread-safe host side: `report`, `clear`, the power observer, the wake hook, and the kqueue the transport polls |
| `in_process_service.{hpp,cpp}` | the service on a thread named `uml-gnss`, and its endpoint |

`UML::GnssSyntheticSource` (`synthetic_source.{hpp,cpp}`) is a separate target
with the launcher's `--location` test sources: fixed, approximate, and a timed
route. Only the launcher and its unit test link it. `Endpoint` and
`makeInProcessService` take any `FixSource`.

## Wire contract

Version 1 has no feature bits and no configuration space, and two queues.

- Queue 0, receive: the driver posts empty buffers and the device fills them
  with the NMEA byte stream, a sentence or a part of one at a time.
- Queue 1, control: the driver sends `{le32 version = 1; le32 power}`. Power 1
  asks the host to start the receiver and 0 to stop it. A message of another
  size or version is ignored.

The receiver is powered from the first open of the node to the last close. The
host sends nothing while it is off, and a buffer that arrives at the driver
while the node is closed is dropped. A guest reset powers it off.

## The epoch

While the receiver is powered, the device writes one epoch each period (one
second) and at once when the host reports or clears a fix; a report restarts
the period. An epoch is six sentences of talker `GN`, each ended by CR LF, in
this order: `GGA`, `RMC`, `GSA`, `GST`, `VTG` and the proprietary `$PKRNF`. A
field the host does not know is empty, never zero. Coordinates are
`ddmm.mmmmmmm` and `dddmm.mmmmmmm` (1e-7 minute). A fix's sentences carry its
own UTC time, so an epoch that repeats a fix repeats its time; with no fix they
carry the current UTC time.

| | precise | approximate | no fix |
|---|---|---|---|
| GGA quality | 1 | 6 | 0 |
| RMC status / mode | `A` / `A` | `A` / `E` | `V` / `N` |
| GSA fix type | 3 with an altitude, else 2 | 2 | 1 |
| PKRNF class | `P` | `A` | `N` |

An approximate fix is the host's reduced-accuracy position (iOS): it is never a
satellite fix, and the stream says so. GGA carries the altitude above sea level
and the geoid separation (ellipsoidal altitude minus altitude). GST carries the
horizontal accuracy as a radial error divided by the square root of two on each
axis, and the vertical accuracy. The satellite count and the dilutions are
empty. `$PKRNF,<class>,<hacc>,<vacc>,<sacc>,<cacc>` carries what NMEA cannot:
the class, and the horizontal, vertical, speed and course accuracies (meters,
meters, meters per second, degrees) as the host reported them.

A precise fix is repeated for 10 seconds and an approximate one for 20 minutes
(the host delivers it up to that old); after that the epoch reports no fix.
`Endpoint::clear()` drops the held fix at once.

## Host side

```cpp
auto started = uml::gnss::makeInProcessService();
// started.service goes to the guest as virtio_uml.device=<name>:65534
started.endpoint->setPowerObserver([](bool on) { /* start or stop locating */ });
started.endpoint->report(fix);   // any thread
```

The power observer is called on the device's thread with true when the guest
powers its receiver and false when it stops or the guest resets, and it must
return quickly. `makeInProcessService` takes a `FixSource` that replaces the
host's reports; the launcher's `--location` builds one
(`fixed:LAT,LON[,ALT[,ACC]]`, `approximate:LAT,LON` or `route:PATH`, where a
route file holds `SECONDS,LAT,LON[,ALT[,ACC]]` lines and is played from the
guest's power-on). Nothing here reads a real location.

## Suspend

A guest suspend disables the device's rings, and a disabled ring is given no
data: the receive buffers wait and are filled after the resume with the newest
epoch. The control queue keeps its requests on a disabled ring, so a power
message sent just before the suspend is not lost. The guest driver is not a
wake source. A guest that suspended itself (Android once its display is off)
takes none of an epoch for a whole period, and the device then calls the
endpoint's wake hook once per period; the runtime sends the same wake as a
phone's power key (`uml_emulator_wake`). A guest the host quiesced stays
suspended until the host resumes it.

## Testing

`tests/unit/gnss` drives the device through its queues and its event descriptor
with synthetic fixes (`ctest -R '^(gnss|virtio-gnss)'`). `guest-gnss-receiver`
(`tests/integration/gnss`) checks the whole path on a Linux guest, including a
suspend the guest enters by itself with the receiver closed and open.
