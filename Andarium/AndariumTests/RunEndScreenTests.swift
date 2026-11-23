// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Testing
@testable import Andarium

/// How a run's end reaches the failure screen: the runtime's result becomes
/// the end the model holds (`LiveRuntime.runEnd`), and the end becomes the
/// words the screen shows (`EndWords`). Neither needs a window.
@MainActor
struct RunEndScreenTests {
    private let refusal = Int32(KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED)

    @Test func onlyTheRuntimesResultForARefusedMemoryNamesTheSize() {
        let refused = LiveRuntime.runEnd(result: refusal, memoryMiB: 2560, stoppedByUser: false)
        #expect(refused.memoryRefusedMiB == 2560)
        #expect(refused.phase == .failed)
        // The library's own out-of-memory status, which a failed allocation of
        // its own returns too, is a failure and names no size.
        let bare = LiveRuntime.runEnd(result: -2, memoryMiB: 2560, stoppedByUser: false)
        #expect(bare.memoryRefusedMiB == nil)
        #expect(bare.phase == .failed)
        // Neither is the loop guard's result, a person's stop or a power-off.
        let loop = LiveRuntime.runEnd(
            result: Int32(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP), memoryMiB: 2560, stoppedByUser: false)
        #expect(loop.memoryRefusedMiB == nil)
        #expect(loop.restartedInALoop)
        let stopped = LiveRuntime.runEnd(result: -4, memoryMiB: 2560, stoppedByUser: true)
        #expect(stopped.memoryRefusedMiB == nil)
        #expect(stopped.stoppedByUser)
        let poweredOff = LiveRuntime.runEnd(result: 0, memoryMiB: 2560, stoppedByUser: false)
        #expect(poweredOff.memoryRefusedMiB == nil)
        #expect(poweredOff.poweredOff)
    }

    @Test func theScreenSaysWhyForARefusalAStopAndAPlainFailure() {
        let refused = EndWords(
            LiveRuntime.runEnd(result: refusal, memoryMiB: 2560, stoppedByUser: false), leastOfferedMiB: 1536)
        #expect(refused.headline == "The virtual machine could not start")
        #expect(
            refused.advice
                == "This device could not give it 2.5 GiB of memory. Close Andarium in the App Switcher and open it again, then lower the memory with Edit under Virtual machine."
        )
        let stopped = EndWords(
            LiveRuntime.runEnd(result: -4, memoryMiB: 2560, stoppedByUser: true), leastOfferedMiB: 1536)
        #expect(stopped.headline == "You stopped the virtual machine")
        #expect(stopped.advice == "To start it again, close Andarium in the App Switcher and open it again.")
        let failed = EndWords(
            LiveRuntime.runEnd(result: -2, memoryMiB: 2560, stoppedByUser: false), leastOfferedMiB: 1536)
        #expect(failed.headline == "The virtual machine stopped unexpectedly")
        #expect(failed.advice == stopped.advice)
    }

    /// The advice to lower the memory is not given where it cannot be done: a
    /// refusal at the least size the ruler offers keeps its headline and takes
    /// the plain advice.
    @Test func theScreenDoesNotAdviseLoweringTheLeastSize() {
        let end = LiveRuntime.runEnd(result: refusal, memoryMiB: 1536, stoppedByUser: false)
        let words = EndWords(end, leastOfferedMiB: 1536)
        #expect(words.headline == "The virtual machine could not start")
        #expect(words.advice == "To start it again, close Andarium in the App Switcher and open it again.")
        #expect(EndWords(end, leastOfferedMiB: 1024).advice.contains("lower the memory"))
    }
}
