# vhost-user test devices

`UML::DelayedAnswerDevice` wraps any `VhostUserDevice` and holds back the
answers a policy chooses, then delivers them through the transport after a
delay or drops them at the next device reset. It lets an in-process test
reach a driver's late-answer and lost-answer paths, which a production
device that answers a request before the guest's kick returns never takes. An
optional answerer also answers a chosen request in the device's place, which
reaches the answers a device gives only when its own work fails, and an
optional postponer keeps a chosen request from the device until a delay has
passed, which reaches the paths for a device that is late to act. Tests link
it beside the production device and transport; no production target links it.

Its contract is in `delayed_answer_device.hpp`; `delayed-answer-device` tests
the wrapper itself, `guest-video-late-answer` and `guest-video-error-answer`
use it on the Virtio video decoder, and `guest-video-camera-reset` and
`guest-video-camera-slow-clear` on the Virtio camera.
