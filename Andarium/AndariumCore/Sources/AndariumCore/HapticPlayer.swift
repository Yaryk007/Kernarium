// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a player did with a score.
public enum HapticPlayResult: Equatable, Sendable {
    case played
    /// The host's haptic engine could not play it: it would not start, or the
    /// target has no actuator.
    case failed
}

/// The device's haptics as a session drives them. Every call is made on the
/// session's one queue, in order; a player keeps its own state and never
/// blocks that queue for longer than starting an engine takes.
public protocol HapticPlayer: AnyObject, Sendable {
    /// Plays the score on its target, replacing what the target plays. Nothing
    /// plays past the end of the score: its elements are bounded by their
    /// durations.
    func play(_ score: HapticScore) -> HapticPlayResult
    /// Sets the intensity of the tone the target plays now. Answers whether
    /// there was one: a tone ends without the caller's say when something else
    /// stops the actuator (the system stopping the engine, a stop of every
    /// target, the player's own bound on a tone), and a caller that kept a
    /// record of it plays again.
    func setToneIntensity(_ intensity: Float, on target: HapticTarget) -> Bool
    /// Stops what the target plays.
    func stop(_ target: HapticTarget)
    /// Stops every target and lets the engine go: the application left the
    /// front, or the guest's connection ended.
    func stopAll()
    /// Has the engine ready for the first score, as the application returns
    /// to the front with a guest that may ask for one.
    func prepare()
    /// Stops the target and drops what the player keeps for it, the engine of
    /// an actuator that is gone (a game controller that disconnected): the
    /// next score for the target asks the engine source again.
    func forget(_ target: HapticTarget)
}

extension HapticPlayer {
    /// A player that keeps nothing for a target has nothing to forget.
    public func forget(_ target: HapticTarget) { stop(target) }
}
