// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Home's "Translated system libraries": whether Android's own libraries (and
/// the compiled Java of its boot image) run from the machine code translated
/// from them ahead of time, which a build that carries the translated-code
/// framework has. On is the default, and the person turns it off to
/// trouble-shoot. It applies when Android next starts: the choice is read at
/// each start and a running Android is not changed.
public struct TranslatedLibrariesPreference {
    /// The stored value's key in the defaults database; a missing value is the
    /// default, on, for a fresh install and for an update alike.
    public static let key = "andarium.translatedLibraries"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; on until one is.
    public var isOn: Bool { defaults.object(forKey: Self.key) as? Bool ?? true }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    public struct Launch: Equatable, Sendable {
        /// Whether the option is on from the start.
        public var isOn: Bool
        /// Whether what the person sets from here on is kept. A harness's
        /// launch decides for its run alone, so what the person chose stays.
        public var keepsChanges: Bool
    }

    /// The start of a launch. A harness launch (`automated`) starts with the
    /// default, so that a measured run never inherits what the person chose;
    /// only a launch of the person's own restores the kept value.
    public func launch(automated: Bool) -> Launch {
        automated ? Launch(isOn: true, keepsChanges: false) : Launch(isOn: isOn, keepsChanges: true)
    }
}

/// What one start of Android does about the translated libraries: the facts
/// that decide it, and what the launch states and the session record says.
public struct TranslatedLibrariesStart: Equatable, Sendable {
    /// The build carries the translated-code framework (`Acknowledgements`
    /// reads the same fact), so the option exists in it.
    public var buildCarries: Bool
    /// The option, as it stands at this start.
    public var optionOn: Bool
    /// The start runs on the JIT engine, which does not use them.
    public var jitInUse: Bool

    public init(buildCarries: Bool, optionOn: Bool, jitInUse: Bool = false) {
        self.buildCarries = buildCarries
        self.optionOn = optionOn
        self.jitInUse = jitInUse
    }

    /// Whether the launch states the release it boots to the emulator, which
    /// is what lets the component run (the boot request's
    /// `android_aot_disabled` is the opposite). Only a build that has the
    /// option and a person who turned it off leave the release out: a build
    /// without the framework launches as it always did.
    public var statesRelease: Bool { !buildCarries || optionOn }

    /// What the session record says of the start.
    public enum Word: String, Equatable, Sendable, CaseIterable {
        /// The option was on and the launch stated the release.
        case on
        /// The option was off.
        case off
        /// The start ran on the JIT engine, which does not use them.
        case notUsedWithJit = "not-used-with-jit"
        /// The build has no translated libraries.
        case unavailable
    }

    public var word: Word {
        if !buildCarries { return .unavailable }
        if jitInUse { return .notUsedWithJit }
        return optionOn ? .on : .off
    }
}
