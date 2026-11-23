// SPDX-License-Identifier: GPL-2.0-or-later

#import "key_path_observer.h"

@implementation UMLKeyPathObserver {
    NSObject* _object;
    NSString* _keyPath;
    void (^_handler)(id);
    BOOL _observing;
}

- (instancetype)initWithObject:(NSObject*)object keyPath:(NSString*)keyPath handler:(void (^)(id))handler
{
    if ((self = [super init])) {
        _object = object;
        _keyPath = [keyPath copy];
        _handler = [handler copy];
        [object addObserver:self forKeyPath:keyPath options:NSKeyValueObservingOptionNew context:nil];
        _observing = YES;
    }
    return self;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context
{
    (void)change;
    (void)context;
    void (^handler)(id);
    @synchronized(self) {
        if (![keyPath isEqualToString:_keyPath]) return;
        handler = _handler;
    }
    // The copy keeps the handler alive for as long as it runs, whatever
    // `invalidate` does to the observer's own reference.
    if (handler != nil) handler([object valueForKeyPath:keyPath]);
}

- (void)invalidate
{
    NSObject* object;
    NSString* keyPath;
    @synchronized(self) {
        if (!_observing) return;
        _observing = NO;
        object = _object;
        keyPath = _keyPath;
        _object = nil;
        _handler = nil;
    }
    // Outside the lock: the observed object's own lock is held while it notifies.
    [object removeObserver:self forKeyPath:keyPath];
}

- (void)dealloc
{
    [self invalidate];
}

@end
