// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What guest memory the host can honour, at each vCPU count. Every figure is
/// the runtime's own (`runtime/guest_memory_budget.hpp`), which the application
/// reads through the loader the way Kernarium does, and none is computed here:
///
/// - the bound is the most a person may choose: the largest multiple of 64 MiB
///   from 512 MiB up to the host's ceiling (the window the kernel gets and the
///   host's RAM) that leaves the host GPU its floor under the memory limit the
///   system sets this process;
/// - the safe size is the line above which iOS is likely to end the
///   application: the same measure with the launch's conservative allowances
///   for the application, the rest of the engine and headroom, in 0.5 GiB
///   steps and at least 1.5 GiB, and never above the bound;
/// - the default is what a new guest gets: the safe size held to 4 GiB, and on
///   a device whose bound reaches 4 GiB also the most that leaves the host
///   GPU a gibibyte, so it is never above the safe size;
/// - the graphics memory is the host GPU's budget, B, a guest of a size and
///   vCPU count would launch with;
/// - the graphics line is the size above which games may run out of graphics
///   memory: the default itself where the runtime lowered the default to keep
///   B at `Limits.minimumDefaultGraphicsMemoryMiB` (and where the host gives
///   no estimate), and elsewhere the largest size that keeps that much B,
///   never under the default or over the safe size.
///
/// Each added vCPU holds more of the process's memory, so neither bound,
/// safe size nor default rises with the count. The limit is fixed for the
/// process, so they are read once, for every count the host has.
///
/// The least a person may choose is `Limits.minimumMemoryMiB`, or the bound
/// where a device's limit leaves less than that: the bound always wins, so the
/// picker is never empty and never offers what the host cannot honour.
public struct MemoryPolicy: Equatable, Sendable {
    /// The most guest RAM at 1, 2, ... vCPUs, each on the runtime's steps and
    /// inside its range.
    private let maximums: [Int]
    /// What a new guest gets at the same counts, on the same steps.
    private let defaults: [Int]
    /// The line above which iOS is likely to end the application, at the same
    /// counts: no lower than the default, no higher than the bound.
    private let safes: [Int]
    /// The graphics memory at each count (one row each) and each of the
    /// runtime's memory steps from the bottom of its range; empty where the
    /// host does not say.
    private let graphics: [[Int]]
    /// The size above which games may run out of graphics memory, at the same
    /// counts.
    private let graphicsLines: [Int]

    /// The bounds, the defaults and the safe sizes as they are: each is taken
    /// down to the runtime's steps and into its range. A count without a
    /// default takes its bound, and one without a safe size its default.
    /// `defaultHoldsGraphics` says per count whether the runtime lowered the
    /// default to keep the host GPU its budget (a count without an entry says
    /// no). `graphicsMiB` answers the graphics memory of a size at a count,
    /// which is asked once for every size of every count.
    public init(
        maximumsMiB: [Int], defaultsMiB: [Int], safeMiB: [Int] = [], defaultHoldsGraphics: [Bool] = [],
        graphicsMiB: ((_ memoryMiB: Int, _ vCPUs: Int) -> Int)? = nil
    ) {
        let values = maximumsMiB.isEmpty ? [Limits.defaultMemoryRangeMiB.upperBound] : maximumsMiB
        let bounds = values.map { Self.steppedDown($0) }
        maximums = bounds
        let steppedDefaults = bounds.indices.map { index in
            index < defaultsMiB.count ? Self.steppedDown(defaultsMiB[index]) : bounds[index]
        }
        defaults = steppedDefaults
        let steppedSafes = bounds.indices.map { index in
            guard index < safeMiB.count else { return steppedDefaults[index] }
            return min(max(Self.steppedDown(safeMiB[index]), steppedDefaults[index]), bounds[index])
        }
        safes = steppedSafes
        var table: [[Int]] = []
        if let graphicsMiB {
            // Every size a person may choose at any count, from the bottom of
            // the runtime's range.
            let steps = Self.runtimeSteps(through: bounds.max() ?? Limits.runtimeMemoryRangeMiB.lowerBound)
            table = bounds.indices.map { index in
                steps.map { graphicsMiB($0, index + 1) }
            }
        }
        graphics = table
        // Where the runtime lowered the default to keep B at a gibibyte, the
        // default itself is the line, as it was before the range passed
        // 4 GiB. Elsewhere the largest size a person may choose that still
        // keeps that much: no lower than the default (a new guest is never
        // warned about) and no higher than the safe size (above it the
        // warning is the worse one). The default itself where B falls below
        // that figure just above it, and where the host gives no estimate.
        graphicsLines = bounds.indices.map { index in
            let atLeast = steppedDefaults[index]
            guard !table.isEmpty, !(index < defaultHoldsGraphics.count && defaultHoldsGraphics[index]) else {
                return atLeast
            }
            let row = table[index]
            let first = Limits.runtimeMemoryRangeMiB.lowerBound
            let top = (bounds[index] - first) / Limits.runtimeMemoryStepMiB
            guard let kept = (0...top).last(where: { row[$0] >= Limits.minimumDefaultGraphicsMemoryMiB }) else {
                return atLeast
            }
            return min(max(atLeast, first + kept * Limits.runtimeMemoryStepMiB), steppedSafes[index])
        }
    }

    /// The bounds alone: every default and safe size is its bound, so nothing
    /// is above them. A host that sets no limit, and the fixtures that do not
    /// exercise the default.
    public init(maximumsMiB: [Int]) {
        self.init(maximumsMiB: maximumsMiB, defaultsMiB: [])
    }

    /// The bounds of 1...`processorCount` vCPUs from `bound`, the loader's
    /// answer for a count; every default is its bound.
    public init(processorCount: Int, bound: (Int) -> Int) {
        self.init(maximumsMiB: (1...max(processorCount, 1)).map(bound))
    }

    /// The bounds, defaults, safe sizes and graphics memory of
    /// 1...`processorCount` vCPUs from the loader's answers for a count: a
    /// safe size defaults to the default, a default is not one the runtime
    /// lowered for the GPU without `defaultHoldsGraphics`, and without
    /// `graphicsMemory` the host gives no estimate.
    public init(
        processorCount: Int, bound: (Int) -> Int, defaultMemory: (Int) -> Int,
        safeMemory: ((Int) -> Int)? = nil,
        defaultHoldsGraphics: ((Int) -> Bool)? = nil,
        graphicsMemory: ((_ memoryMiB: Int, _ vCPUs: Int) -> Int)? = nil
    ) {
        let counts = 1...max(processorCount, 1)
        self.init(
            maximumsMiB: counts.map(bound), defaultsMiB: counts.map(defaultMemory),
            safeMiB: safeMemory.map { rule in counts.map(rule) } ?? [],
            defaultHoldsGraphics: defaultHoldsGraphics.map { rule in counts.map(rule) } ?? [],
            graphicsMiB: graphicsMemory
        )
    }

    /// The most guest RAM the picker offers at `vCPUs` vCPUs; a count above the
    /// host's takes the last bound.
    public func maximumMiB(vCPUs: Int) -> Int {
        maximums[min(max(vCPUs, 1), maximums.count) - 1]
    }

    /// The least guest RAM the picker offers at `vCPUs` vCPUs: the minimum
    /// for Android, or the bound where that is lower.
    public func minimumMiB(vCPUs: Int) -> Int {
        min(Limits.minimumMemoryMiB, maximumMiB(vCPUs: vCPUs))
    }

    /// Every position of the picker at `vCPUs` vCPUs: the minimum up to the
    /// bound, in the runtime's 64 MiB steps.
    public func stops(vCPUs: Int) -> [Int] {
        Array(stride(
            from: minimumMiB(vCPUs: vCPUs), through: maximumMiB(vCPUs: vCPUs), by: Limits.memoryStepMiB
        ))
    }

    /// What a new guest gets at `vCPUs` vCPUs; a count above the host's takes
    /// the last one.
    public func defaultMemoryMiB(vCPUs: Int) -> Int {
        defaults[min(max(vCPUs, 1), defaults.count) - 1]
    }

    /// The size above which iOS is likely to end the application at `vCPUs`
    /// vCPUs; a count above the host's takes the last one. It is the default
    /// unless the default is held lower for the graphics memory.
    public func safeMemoryMiB(vCPUs: Int) -> Int {
        safes[min(max(vCPUs, 1), safes.count) - 1]
    }

    /// Whether a guest with `mib` of memory and `vCPUs` vCPUs has more than a
    /// new guest gets. It need not be short of graphics memory: that is
    /// `isShortOfGraphicsMemory`.
    public func isAboveDefault(_ mib: Int, vCPUs: Int) -> Bool {
        mib > defaultMemoryMiB(vCPUs: vCPUs)
    }

    /// Whether a guest with `mib` of memory and `vCPUs` vCPUs has more than
    /// `safeMemoryMiB`: iOS is then likely to end the application while the
    /// guest runs. The same figure on Home and on the memory picker, so they
    /// agree at every size and count.
    public func isAboveSafe(_ mib: Int, vCPUs: Int) -> Bool {
        mib > safeMemoryMiB(vCPUs: vCPUs)
    }

    /// The size above which a guest with `vCPUs` vCPUs is short of graphics
    /// memory. Where the runtime lowered the default to keep B at
    /// `Limits.minimumDefaultGraphicsMemoryMiB` it is the default itself, so
    /// the warning may show while B still has that much. Elsewhere it is the
    /// largest size that leaves B at that figure or more, at least the
    /// default, since a new guest is never warned about, and at most the safe
    /// size, above which the closing warning is the one shown: the default
    /// itself where B is already under it above the default, as on a device
    /// with little room, and where the host gives no estimate. A count above
    /// the host's takes the last one.
    public func graphicsLineMiB(vCPUs: Int) -> Int {
        graphicsLines[min(max(vCPUs, 1), graphicsLines.count) - 1]
    }

    /// Whether a guest with `mib` of memory and `vCPUs` vCPUs has more than
    /// `graphicsLineMiB`: graphics-hungry apps are then likely to run out of
    /// graphics memory, though iOS need not end the application. Where the
    /// default was not lowered for the GPU and more memory leaves B at a
    /// gibibyte or more, the sheet does not warn.
    public func isShortOfGraphicsMemory(_ mib: Int, vCPUs: Int) -> Bool {
        mib > graphicsLineMiB(vCPUs: vCPUs)
    }

    /// What a guest of `mib` of memory and `vCPUs` vCPUs risks, the worse of
    /// the two when it risks both: iOS ending the application, then the guest
    /// running out of graphics memory.
    public func risk(_ mib: Int, vCPUs: Int) -> MemoryRisk {
        if isAboveSafe(mib, vCPUs: vCPUs) { return .closing }
        return isShortOfGraphicsMemory(mib, vCPUs: vCPUs) ? .graphics : .none
    }

    /// The graphics memory, in MiB, a guest of `mib` of memory and `vCPUs`
    /// vCPUs would launch with: the host GPU's budget, from the runtime's own
    /// formula. A size is taken to the runtime's step below it, and a count
    /// above the host's takes the last one. Nil where the host does not say.
    public func graphicsMemoryMiB(forMemory mib: Int, vCPUs: Int) -> Int? {
        guard !graphics.isEmpty else { return nil }
        let row = graphics[min(max(vCPUs, 1), graphics.count) - 1]
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        return row[min(offset / Limits.runtimeMemoryStepMiB, row.count - 1)]
    }

    /// A stored or requested size at `vCPUs` vCPUs: on the nearest step, at
    /// least the minimum and no more than the bound.
    public func clamped(_ mib: Int, vCPUs: Int) -> Int {
        min(max(Self.nearestStep(mib), Limits.minimumMemoryMiB), maximumMiB(vCPUs: vCPUs))
    }

    /// Whether the runtime accepts `mib` (a value of its own step in range).
    public static func runtimeAccepts(_ mib: Int) -> Bool {
        Limits.runtimeMemoryRangeMiB.contains(mib)
            && (mib - Limits.runtimeMemoryRangeMiB.lowerBound) % Limits.runtimeMemoryStepMiB == 0
    }

    /// The sizes the runtime accepts, from the bottom of its range through
    /// `top`.
    private static func runtimeSteps(through top: Int) -> [Int] {
        Array(stride(
            from: Limits.runtimeMemoryRangeMiB.lowerBound, through: top,
            by: Limits.runtimeMemoryStepMiB
        ))
    }

    private static func nearestStep(_ mib: Int) -> Int {
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        let step = Limits.runtimeMemoryStepMiB
        return range.lowerBound + (offset + step / 2) / step * step
    }

    private static func steppedDown(_ mib: Int) -> Int {
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        return range.lowerBound + offset / Limits.runtimeMemoryStepMiB * Limits.runtimeMemoryStepMiB
    }
}

/// What a guest of some size risks: the application being ended by iOS
/// (`MemoryWarning`), or the guest running out of graphics memory
/// (`GraphicsMemoryWarning`).
public enum MemoryRisk: Equatable, Sendable {
    case none
    case graphics
    case closing
}

/// What the application says when a guest has more memory than `safeMemoryMiB`
/// (`MemoryPolicy.isAboveSafe`): iOS is then likely to close Andarium while
/// the guest runs. Home and the memory picker use these words, so they say the
/// same thing.
public enum MemoryWarning {
    public static let title = "Guest memory is high"

    /// Home's notice: the guest's size, the size that is safer, and where to
    /// lower it.
    public static func explanation(memoryMiB: Int, safeMiB: Int, deviceName: String) -> String {
        "iOS is likely to close Andarium with \(Formats.size(mib: memoryMiB)) of guest memory on this "
            + "\(deviceName). \(Formats.size(mib: safeMiB)) or less is safer. "
            + "Lower it with Edit under Guest system."
    }

    /// The line beneath the memory picker.
    public static func note(safeMiB: Int) -> String {
        "iOS is likely to close Andarium above \(Formats.size(mib: safeMiB))."
    }
}

/// What the System sheet says of the graphics memory (VRAM): that it is not
/// part of the guest's RAM, and, when a guest has more RAM than leaves it the
/// graphics memory a new one keeps (`MemoryPolicy.isShortOfGraphicsMemory`),
/// that a game may run short of it.
public enum GraphicsMemoryWarning {
    public static let title = "VRAM"

    /// Beneath the estimate: what VRAM is relative to the memory above it.
    public static let separateNote = "VRAM is graphics memory, separate from the guest\u{2019}s RAM."

    /// The line beneath the memory picker above the graphics line
    /// (`MemoryPolicy.graphicsLineMiB`) and not above the safe size.
    public static func note(aboveMiB: Int) -> String {
        "Above \(Formats.size(mib: aboveMiB)), games may run out of graphics memory."
    }
}
