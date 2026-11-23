# Virtio input

`darwin/device/input/virtio` is the host end of the guest's Virtio-input devices
(device ID 18) over the common vhost-user transport. The guest finds them
through the kernel's `virtio_input` driver, so they are ordinary evdev devices.
`darwin/device/input/vnc` turns a VNC client's keys and pointer into events for
the touchscreen and the side-key keyboard.

## Devices

A device is identified by its name, its serial (the kernel puts it in `uniq`,
which Android's device descriptor uses) and its vendor and product (`1af4:NNNN`,
bus `BUS_VIRTUAL`). Android keys keyboard layouts and settings to that
identity, so every name, serial and product below is permanent once shipped,
and so is each device's bitmaps: only an addition is compatible.

| Kind | Product | Name | Serial | Events |
|---|---|---|---|---|
| touchscreen | 0001 | Kernarium Touchscreen | `kernarium-touch-0` | multitouch, `INPUT_PROP_DIRECT` |
| side keys | 0002 | Kernarium Keyboard | `kernarium-keyboard-0` | Power, the volume keys, Back, Home; `EV_SW` `SW_CAMERA_LENS_COVER` when the host states it |
| hardware keyboard | 0003 | Kernarium Hardware Keyboard | `kernarium-hardware-keyboard-0` | `EV_KEY` exactly the codes `hid_keyboard.hpp` can produce (not `KEY_UNKNOWN`), `EV_LED` Num, Caps and Scroll Lock |
| mouse | 0004 | Kernarium Mouse | `kernarium-mouse-0` | `EV_REL` X, Y and both wheels in steps and in high-resolution units, `EV_KEY` the five mouse buttons, `EV_LED` `LED_MISC` |
| pointer | 0005 | Kernarium Pointer | `kernarium-pointer-0` | `EV_KEY` `BTN_TOUCH`, `BTN_TOOL_MOUSE` and the five buttons, `EV_ABS` slot (0 to 9), tracking id and position 0 to 32767; no input property |

The pointer is the mouse's absolute half: Android takes `BTN_TOOL_MOUSE` with
absolute positions as an absolute mouse and draws its own cursor. A host input
is the position over the guest display on the touchscreen's range, and
`pointerRawPosition` converts it so that Android's one uniform scale of the
raw range (the larger side of the display over 32768) lands on the pixel a
touch there would touch.

The hardware devices send no key repeat (Android repeats keys itself), send a
key by its position (HID usage, through the table that transcribes the Linux
HID driver's `hid_keyboard[]`), and take the layout from Android.
`LED_MISC` on the mouse is how an Android application tells the host it
captured the pointer.

The side keys carry one switch when the host states it before the guest boots
(`EventQueue::setSwitch`): `SW_CAMERA_LENS_COVER`, on in a session whose cameras
are blocked, which Android reads as its hardware camera privacy switch
([`darwin/device/camera`](../camera/README.md)). It is advertised only then, and
reported as a report of its own ahead of every other event and again after a
reset, so the guest's input core holds it from the moment it creates the device.

## Connected state

The keyboard, the mouse and the pointer exist from boot and the host plugs and
unplugs them (`EventQueue::Attachment::pluggable`). The mouse and the pointer
are one mouse to Android and are plugged together.

- A device answers its own event bitmaps only while it is plugged and its
  driver has probed, which is when the driver posts its first event buffer.
  Otherwise it answers empty bitmaps, which is a device with no events. A
  plug before the probe therefore changes nothing the guest has seen.
- A configuration change is raised for each plug and each unplug that the guest
  can see and for the first buffer of a driver that finds the device plugged,
  and for nothing else.
- Before an unplug the host sends the releases of every key and button it
  holds and ends the pointer's contact, so nothing is left down.
- Input for an unplugged device is dropped.
- Input for a plugged device whose guest input device is not yet registered
  waits in a bounded buffer and goes to the guest when the driver announces the
  registration with a status event
  ([`kernel/common/input`](../../../kernel/common/input/README.md)). While it
  waits only the pointer's latest position is kept, and no relative motion.
  Input that does not fit is dropped, and so is what follows until the
  registration; the host forgets what it holds of that device, so the release
  of a key that was dropped is nothing and its next press goes down afresh.
- A queue that has no room for a report drops it whole, and a lift or a
  release may be what was lost. The queue follows what the guest was told is
  held (`HeldInput`: the `EV_KEY` codes and the multitouch contacts) and sends
  one report that ends all of it ahead of the next report it accepts, or when
  the guest reads the queue empty first. A device that is unplugged, or whose
  guest input device is not registered, owes none, and a reset, an unplug or a
  registration or unregistration starts the tracking afresh.
- Each input returns whether it wakes a guest that suspended itself: a key or a
  button going down, or a wheel taking a whole step; never motion or a
  release.
- `HardwareInput::setStatusCallback` receives Android's LED writes as Num, Caps
  and Scroll Lock and as pointer capture on or off, and each registration and
  unregistration the driver announces, the latter also for an unplugged
  device. `RegistrationSummary` turns the three devices' registrations into the
  application's two (the keyboard, and the pointer while the mouse and the
  absolute pointer both are registered) and takes both away when a boot ends;
  the runtime publishes them as `KERNARIUM_RUNTIME_INPUT_STATUS_KEYBOARD_REGISTERED`
  and `_POINTER_REGISTERED`.

A kernel without the driver's change never registers a plugged device: it sees
an input device with no events, which Android drops, and the host drops what it
held.

## macOS launcher

`--hardware-input` of the macOS launchers gives the guest the input devices an
Andarium guest has: the touchscreen and the side keys, which nothing feeds, and
the keyboard, mouse and pointer after every other device, unplugged. The guest
has no VNC input device. The display's keys and pointer go to the hardware
devices. It requires `--gpu` with a VNC or native display. The first key of a
client plugs the keyboard and the first pointer event plugs the mouse and the
pointer; when the last client of a VNC display has gone both are unplugged, and
a native window keeps what it plugged. No key is reserved: every key goes to
the guest. The launcher prints each plug and unplug, each registration the
guest announces, and Android's lock LEDs and pointer capture, on stderr as
`Hardware keyboard: ...`, `Hardware mouse: ...` and `Hardware pointer: ...`.

## Testing

`ctest -R '^virtio-input-'` drives each device through its queues and its event
descriptor with no guest, behind the real vhost-user transport for its
configuration changes, and the launcher's bridge from a display's events to
them. `run_launcher_refusals.py` (`tests/integration/boot`) covers the
launcher option's refusals.
`guest-hardware-input` (`tests/integration/input`) boots a Linux guest with the
option and plays a VNC client: the guest's driver must register and unregister
each device's input device as the client plugs and unplugs it.
