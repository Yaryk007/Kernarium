// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

#if !APP_STORE

/// The debugger side of the JIT: whether one is attached to Andarium and
/// serves the runtime's request for executable memory, and the preparation
/// the runtime makes with it. An App Store build compiles this file out and
/// has no JIT.
enum JitDebugger {
    /// Whether an attached debugger is one that serves the request. Release
    /// builds run from StikDebug, whose script serves it. Under Xcode a Debug
    /// build is attached to lldb, which would stop at the request instead of
    /// serving it, so Debug asks the launch to say a servicing debugger is
    /// attached (the test harness's stand-in does).
    static var serves: Bool {
        #if DEBUG
        ProcessInfo.processInfo.environment["ANDARIUM_JIT_DEBUGGER"] == "1"
        #else
        true
        #endif
    }

    /// A servicing debugger is attached now.
    static func attached() -> Bool {
        serves && kernarium_runtime_loader_debugger_attached() != 0
    }

    /// Prepares the executable memory for `shape` when `wanted`, or tells an
    /// attached debugger to leave when not, and says how it came out. The
    /// debugger takes seconds, so this runs off the main thread. Without a
    /// servicing debugger nothing is asked of the runtime: a request would
    /// stop in a debugger that does not serve it.
    static func prepare(wanted: Bool, shape: JitShape, flavor: UInt32) async -> JitAnswer {
        let attached = attached()
        // Not wanted and nobody to tell to leave: nothing to do.
        guard wanted || attached else { return .disabled }
        // Wanted, but a debugger that does not serve the request may be
        // attached (Debug under Xcode): the runtime must not ask it.
        if wanted, !serves { return .noDebugger }
        return await Task.detached(priority: .userInitiated) { () -> JitAnswer in
            guard kernarium_runtime_loader_select(flavor) == 0 else { return .unsupported }
            return JitAnswer(runtimeValue: kernarium_runtime_loader_prepare_jit(
                wanted ? 1 : 0, UInt32(shape.memoryMiB), UInt32(shape.processorCount)))
        }.value
    }
}

#endif
