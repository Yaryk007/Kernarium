// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What a system posts when another app takes the audio session and gives it
// back, as the Simulator tests of the Apple sink and the Apple microphone
// post it. iOS 27 posts the session becoming inactive and the system's
// resumption recommendation; earlier systems post one interruption
// notification. A test built with UML_AUDIO_LEGACY_SESSION posts the earlier
// one, and runs where the system is earlier too.

enum class DeactivationSource { app, system };

// What iOS 27 posts when the session becomes inactive: for the app's own
// deactivation, or for the system's, which carries an interruption context
// when another app caused it. An earlier system posts nothing for a
// deactivation that interrupts nothing.
void postDeactivation(DeactivationSource source, bool interrupted);

// Another app takes the session: the system deactivates it with an
// interruption context.
void postInterruptionBegan();

// What the system posts when the interrupter lets go, with its advice on
// resuming.
void postInterruptionEnded(bool shouldResume);
