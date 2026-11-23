// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "kernarium.gamepad"

#include <log/log.h>

#include "Relay.h"

// The gamepad relay of the guest (Relay.h): the host application's game
// controllers become HID devices through /dev/uhid. It serves every device and
// waits for the host with no timeout, so it costs nothing while no controller
// is connected.
int main() {
    andarium::gamepad::Relay relay;
    const int status = relay.run();
    ALOGE("the relay stopped (status %d)", status);
    return status;
}
