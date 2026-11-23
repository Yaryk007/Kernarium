// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Foundation/Foundation.h>

// Calls a handler with the new value of an object's key path each time it
// changes (key-value observing), and stops cleanly.
//
// Key-value observing runs the handler on whichever thread changed the value,
// with no order against `invalidate`, and `removeObserver:` does not wait for a
// notification that is already running. So the observer owns its handler under
// a lock and a notification takes a strong copy of it under that lock and calls
// the copy outside it: an `invalidate` (and the owner's release of the observer)
// that comes while the handler runs cannot free the block the handler is
// running from, and the handler may call `invalidate` itself. `invalidate` does
// not wait for a notification that is running: once it returns no notification
// takes the handler again, but one that took it before (and is still on its way
// to calling it) calls it after `invalidate` has returned, so a handler must be
// harmless once its owner has stopped.
@interface UMLKeyPathObserver : NSObject
- (instancetype)initWithObject:(NSObject*)object keyPath:(NSString*)keyPath handler:(void (^)(id value))handler;
- (void)invalidate;
@end
