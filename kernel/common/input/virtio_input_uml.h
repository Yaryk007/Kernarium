/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private Virtio-input registration notice.
 *
 * The Kernarium hardware keyboard, mouse and pointer are Virtio-input devices
 * that the host plugs and unplugs while the guest runs. The host raises a
 * configuration change and the driver reads the device's event bitmaps again:
 * a device that reports no event type has no input device, and one that
 * reports events has one. The host has to know when the input device exists,
 * because the first events after a plug must not reach the guest before it
 * does, so after the driver registers or unregisters the input device it
 * sends one event on the status queue, in the wire format of every other
 * status event (struct virtio_input_event).
 *
 * The status queue carries only what the input core sends a device: EV_LED and
 * EV_SND. A notice is in neither, so a host that does not know it cannot take
 * it for a LED or a sound. Its type is one no evdev driver defines (EV_MAX is
 * 0x1f, and the highest type input-event-codes.h defines is 0x17).
 */
#ifndef VIRTIO_INPUT_UML_H
#define VIRTIO_INPUT_UML_H

/* The type of a notice, in the type field of struct virtio_input_event. */
#define VIRTIO_INPUT_UML_EV_NOTICE 0x1e

/*
 * The notice codes. A notice is sent in answer to every configuration change
 * that leaves the driver with a registered input device, including the change
 * that finds it registered already, so that every plug is answered, and once
 * at probe when the probe registers an input device.
 */
/* value 1: the input device is registered; value 0: it is not. */
#define VIRTIO_INPUT_UML_NOTICE_REGISTRATION 0x00

#endif /* VIRTIO_INPUT_UML_H */
