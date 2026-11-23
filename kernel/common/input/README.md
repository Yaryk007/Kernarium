# Private Virtio-input registration notice

`virtio_input_uml.h` owns the one private extension of the Virtio-input device
that the host's hardware keyboard, mouse and pointer devices
([`darwin/device/input`](../../../darwin/device/input/README.md)) use. It is
independent of the public API and the host/kernel bridge ABI, and the wire is
the same for every maintained kernel slice.

These devices are plugged and unplugged by the host while the guest runs. The
host raises a Virtio configuration change and the driver reads the device's
event bitmaps again: a device that reports no event type has no input device,
and one that reports events has one. A device reports events only once its
driver has posted its first event buffer, so a probe reads no events from it
whenever the host plugged it, and the configuration change for a device that
is plugged follows that first buffer.

The driver therefore posts its event buffers at probe even when it registers no
input device: the host answers a device as unplugged until its driver has
posted one, so a driver that waited for an input device to post them would
never see a plug.

After the driver registers or unregisters the input device it sends one event
on the status queue, `VIRTIO_INPUT_UML_EV_NOTICE` with code
`VIRTIO_INPUT_UML_NOTICE_REGISTRATION` and value 1 (registered) or 0, so that
the host can hold the first events after a plug until the input device exists
and can tell the application when it is gone. Every configuration change that
leaves the driver with a registered input device is answered, including the one
that finds it registered already; a probe that registers one sends the notice
too; and the unregistration that answers an unplug is sent.

A driver's obligations follow from the transport. A configuration read answers
the device's state when it is made, with no generation to tie the reads of one
change together, so the driver re-reads every bitmap on every change and
registers again when any of them differs from what it registered. A notice
that answers one plug can arrive after a later plug and be taken for its answer,
which is harmless because the next change is answered in turn.

A kernel without the driver change reads every device as an input device with
no events, which Android drops, and never answers a plug; the host drops what it
held after a bound.

Edit the canonical header, then use `sync-protocol.py --kernel-source PATH` to
copy it to `drivers/virtio/virtio_input_uml.h` in each patch-producing kernel
checkout, amend the owning topic and regenerate each line's series; `--verify`
is the ordinary check. The tool also refuses a kernel whose
`input-event-codes.h` already defines the header's event type. Never edit
generated patch output.
