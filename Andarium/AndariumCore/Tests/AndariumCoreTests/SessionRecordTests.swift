// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The runtime's record of how the previous session ended, read as the
/// application reads it at launch, the notice Home shows for it, and the
/// redaction its text goes through before it leaves the device.
struct SessionRecordTests {
    /// The record as the runtime writes it: a memory slot padded to 256
    /// bytes, then the session's state.
    private func record(_ state: String, memory: String = "memory at 1794114290 footprint 2147483648 peak 2415919104")
        -> String
    {
        var slot = "uml-session 2\n\(memory)\n"
        slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
        return slot + state
    }

    @Test func readsAPanicWithItsKernelOutput() throws {
        let text = record(
            """
            state ended
            end guest-panic
            status 6
            pid 46851
            started 1794114290
            ended 1794114295
            message sysrq triggered crash
            kernel-output
            [    0.566837] Kernel panic - not syncing: sysrq triggered crash
            Darwin UML panic: sysrq triggered crash

            """)
        let record = try #require(SessionRecord(text: text))
        #expect(!record.running && !record.orderly)
        #expect(record.end == "guest-panic" && record.status == 6 && record.processID == 46851)
        #expect(record.started == Date(timeIntervalSince1970: 1_794_114_290))
        #expect(record.ended == Date(timeIntervalSince1970: 1_794_114_295))
        #expect(record.message == "sysrq triggered crash")
        #expect(record.kernelOutput?.hasPrefix("[    0.566837] Kernel panic") == true)
        #expect(record.memory?.footprintBytes == 2 << 30 && record.memory?.availableBytes == nil)
        #expect(record.summary == "The previous session ended in a guest kernel panic: sysrq triggered crash.")
        #expect(record.notice(endedOutOfForeground: false) == SessionEndNotice(
            title: "Android’s kernel panicked", text: "Android stopped with a kernel panic: sysrq triggered crash."))
    }

    @Test func readsWhereAnUnfinishedBootHadGot() throws {
        let running = try #require(SessionRecord(text: record(
            """
            state running
            end running
            status 0
            pid 7
            started 1794114339
            boot unfinished
            restarts 2
            kernel-output
            [   31.2] init: starting service 'netd'...

            """)))
        #expect(running.running && running.bootFinished == false && running.restarts == 2)
        #expect(running.kernelOutput?.hasPrefix("[   31.2] init: starting service 'netd'") == true)
        #expect(running.summary.hasSuffix(
            " Android had not finished booting and the guest had restarted 2 times in this process."))
        let stopped = try #require(SessionRecord(text: record(
            "state ended\nend stopped\nstatus 0\npid 7\nstarted 1794114339\nended 1794114540\nboot unfinished\nrestarts 0\n"
        )))
        #expect(stopped.orderly && stopped.bootFinished == false && stopped.restarts == 0)
        #expect(stopped.summary == "The previous session ended: stopped. Android had not finished booting.")
    }

    @Test func aFinishedBootAddsNothingToTheSummary() throws {
        let done = try #require(SessionRecord(text: record(
            "state ended\nend powered-off\nstatus 0\npid 7\nstarted 1\nended 2\nboot completed\nrestarts 3\n")))
        #expect(done.bootFinished == true && done.restarts == 3)
        #expect(!done.summary.contains("finished booting"))
    }

    @Test func aBootTheRuntimeCouldNotObserveClaimsNothing() throws {
        // The console showed none of init's output: the boot may have been
        // fine, so the record is not read as an unfinished one.
        let unseen = try #require(SessionRecord(text: record(
            "state ended\nend stopped\nstatus 0\npid 7\nstarted 1\nended 2\nboot unobserved\nrestarts 1\n")))
        #expect(unseen.bootFinished == nil && unseen.restarts == 1)
        #expect(!unseen.summary.contains("finished booting"))
    }

    @Test func readsWhichRamdiskTheBootUsedAndHowFarItGot() throws {
        for (word, expected) in [
            ("stock", SessionRecord.BootRamdisk.stock), ("root", .root), ("recovery", .recovery),
        ] {
            let read = try #require(SessionRecord(text: record(
                "state ended\nend guest-panic\nstatus 0\npid 7\nstarted 1\nended 2\nboot unobserved\nrestarts 0\nramdisk \(word)\n")))
            #expect(read.ramdisk == expected && read.bootProgress == .unobserved, "\(word)")
        }
        let progress = try #require(SessionRecord(text: record(
            "state ended\nend stopped\nstatus 0\npid 7\nstarted 1\nended 2\nboot unfinished\nrestarts 0\n")))
        #expect(progress.bootProgress == .unfinished && progress.ramdisk == nil)
        let done = try #require(SessionRecord(text: record(
            "state ended\nend powered-off\nstatus 0\npid 7\nstarted 1\nended 2\nboot completed\nrestarts 0\nramdisk root\n")))
        #expect(done.bootProgress == .completed && done.ramdisk == .root)
        // A word this reader does not know, or no word, names no ramdisk.
        let odd = try #require(SessionRecord(text: record(
            "state ended\nend powered-off\nstatus 0\npid 7\nboot sideways\nramdisk floppy\n")))
        #expect(odd.bootProgress == nil && odd.ramdisk == nil)
    }

    @Test func theRecordStaysWhereItWasWrittenUntilTheLaunchKeepsIt() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("previous-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let current = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        let kept = directory.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        try Data(record("state ended\nend guest-panic\nstatus 0\npid 7\nstarted 1\nended 2\nboot unobserved\nrestarts 0\nramdisk root\n").utf8)
            .write(to: current)
        // Reading it again and again, as a launch that ends in between does, finds it.
        for _ in 0..<2 {
            let read = try #require(PreviousSession.read(in: directory, endedOutOfForeground: false))
            #expect(read.record.ramdisk == .root && read.record.end == "guest-panic")
            #expect(FileManager.default.fileExists(atPath: current.path) && !FileManager.default.fileExists(atPath: kept.path))
        }
        // Once what was decided from it is saved, it is kept for the bug report and a second launch finds none.
        PreviousSession.keep(in: directory)
        #expect(!FileManager.default.fileExists(atPath: current.path) && FileManager.default.fileExists(atPath: kept.path))
        #expect(PreviousSession.read(in: directory, endedOutOfForeground: false) == nil)
        // A launch with no current record leaves the kept one alone.
        PreviousSession.keep(in: directory)
        #expect(FileManager.default.fileExists(atPath: kept.path))
    }

    @Test func aRecordThatSaysNothingOfTheBootLeavesItUnknown() throws {
        let plain = try #require(SessionRecord(text: record(
            "state ended\nend powered-off\nstatus 0\npid 7\nstarted 1\nended 2\n")))
        #expect(plain.bootFinished == nil && plain.restarts == nil)
        #expect(!plain.summary.contains("booting"))
        let odd = try #require(SessionRecord(text: record(
            "state running\nend running\nboot maybe\nrestarts many\n")))
        #expect(odd.bootFinished == nil && odd.restarts == nil)
    }

    /// The runtime writes every state over the whole of its area and pads it
    /// with newlines, so a shorter state never leaves the end of a longer one.
    @Test func newlinesThatPadTheStateAreNotPartOfIt() throws {
        let padding = String(repeating: "\n", count: 3000)
        let unfinished = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\nboot unfinished\nrestarts 2\n"
                + "kernel-output\n[   31.2] init: starting service 'netd'\n[   31.3] init: next\n" + padding)))
        #expect(unfinished.running && unfinished.bootFinished == false && unfinished.restarts == 2)
        #expect(unfinished.kernelOutput == "[   31.2] init: starting service 'netd'\n[   31.3] init: next\n")
        let completed = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\nboot completed\nrestarts 2\n"
                + padding)))
        #expect(completed.bootFinished == true && completed.kernelOutput == nil && completed.processID == 7)
        // A line the guest left unfinished stays as it is.
        let cut = try #require(SessionRecord(text: record(
            "state ended\nend failed\nstatus 1\npid 7\nstarted 1794114339\nended 1794114340\nkernel-output\n[ 1.0] ab")))
        #expect(cut.kernelOutput == "[ 1.0] ab")
    }

    @Test func aRecordStillRunningNearTheLimitWasEndedByIOS() throws {
        let near = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 5905580032 peak 5905580032 available 104857600")))
        #expect(near.running && !near.orderly && near.ended == nil && near.kernelOutput == nil)
        #expect(near.nearMemoryLimit)
        #expect(near.memory?.availableBytes == 100 << 20)
        #expect(near.summary.hasSuffix("it used 5632 MiB, 100 MiB left."))
        #expect(near.notice(endedOutOfForeground: false)?.title == "iOS ended Andarium")
        // Out of the foreground it is the pause marker's to report.
        #expect(near.notice(endedOutOfForeground: true) == nil)

        let roomy = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 2147483648 peak 2147483648 available 2147483648")))
        #expect(!roomy.nearMemoryLimit)
        #expect(roomy.notice(endedOutOfForeground: false)?.title == "Andarium stopped unexpectedly")
    }

    @Test func eachBadEndHasItsNotice() throws {
        let titles = [
            "kernel-abort": "Android’s kernel crashed",
            "kernel-exit": "Android stopped",
            "hibernation-restore-failed": "Android could not resume",
            "host-fault": "Andarium crashed",
            "process-exit": "Andarium crashed",
        ]
        for (end, title) in titles {
            let ended = try #require(SessionRecord(text: record("state ended\nend \(end)\nstatus 11\npid 9\n")))
            #expect(ended.notice(endedOutOfForeground: false)?.title == title, "\(end)")
        }
    }

    @Test func anOrderlyEndHasNoNotice() throws {
        for end in ["powered-off", "restarted", "stopped"] {
            let ended = try #require(SessionRecord(text: record("state ended\nend \(end)\nstatus 0\npid 9\n")))
            #expect(ended.orderly && ended.message == nil)
            #expect(ended.notice(endedOutOfForeground: false) == nil)
        }
    }

    @Test func readsTheFirstFormatToo() throws {
        let first = try #require(SessionRecord(
            text: "uml-session 1\nstate ended\nend powered-off\nstatus 0\npid 9\nstarted 1\nended 2\n"))
        #expect(first.orderly && first.memory == nil)
    }

    @Test func readsTheTermsAndTheirHistory() throws {
        var slot = """
            uml-session 3
            memory at 1794210239 footprint 6441524416 peak 6442458280 available 926528
            terms guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
            peak-terms at 1794210239 footprint 6143 guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
            history seconds footprint available guest gpu graphics cache heaps malloc other
            h 484 6100 43 4090 1250 900 238 258 790 614
            h 486 6143 0 4090 1262 905 238 260 812 650

            """
        slot += String(repeating: " ", count: 4095 - slot.utf8.count) + "\n"
        let near = try #require(SessionRecord(
            text: slot + "state running\nend running\nstatus 0\npid 20373\nstarted 1794209753\n"))
        #expect(near.running && near.nearMemoryLimit)
        #expect(near.memory?.footprintBytes == 6_441_524_416 && near.memory?.availableBytes == 926_528)
        #expect(near.terms?.hasPrefix("guest 4090 gpu 1262/1251") == true)
        #expect(near.peakTerms?.hasPrefix("at 1794210239 footprint 6143 guest 4090") == true)
        #expect(near.notice(endedOutOfForeground: false)?.title == "iOS ended Andarium")
    }

    /// A layout 5 record, whose history rows have ten columns in a 4 KiB
    /// memory slot, and a layout 6 record, whose rows add the figures of the
    /// run in an 8 KiB one, carry the same slots, and the reader takes the
    /// same fields from both. A layout 6 record of the largest size the
    /// runtime writes is read whole.
    @Test func aLayout5ALayout6AndALayout7RecordReadAlike() throws {
        let columns = "h 484 6100 43 4090 1250 900 238 258 790 614"
        let figures = " 2000 120 45 2 1 12 5000 1 2 3 158 5900 5000 74 3000 11800"
        let names = " span frames gap thermal pressure compressed internal reliefs shed renewals largest"
            + " cpu vcpu perf mhz energy"
        for layout in [5, 6, 7, 8] {
            var memory = """
                uml-session \(layout)
                memory at 1794210239 footprint 6441524416 peak 6442458280 available 926528
                terms guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
                peak-terms at 1794210239 footprint 6143 guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
                history seconds footprint available guest gpu graphics cache heaps malloc other\(layout >= 6 ? names : "")
                \(columns)\(layout >= 6 ? figures : "")

                """
            memory = NetworkRecordTests.padded(String(memory.dropLast()), to: layout >= 6 ? 8192 : 4096)
            // Layout 7 has the storage slot, blank until the first settle,
            // between the network slot and the state, and layout 8 the camera
            // slot, blank until the first failed still, after it.
            let text = memory + NetworkRecordTests.padded(try StallRecordTests.golden(), to: 4096)
                + NetworkRecordTests.padded(try NetworkRecordTests.golden(), to: 512)
                + (layout >= 7 ? NetworkRecordTests.padded("", to: 256) : "")
                + (layout == 8 ? NetworkRecordTests.padded("", to: 1024) : "")
                + "state running\nend running\nstatus 0\npid 20373\nstarted 1794209753\n"
            let record = try #require(SessionRecord(text: text), "layout \(layout)")
            #expect(record.running && record.nearMemoryLimit && record.processID == 20373, "layout \(layout)")
            #expect(record.memory?.footprintBytes == 6_441_524_416 && record.memory?.availableBytes == 926_528)
            #expect(record.terms?.hasPrefix("guest 4090 gpu 1262/1251") == true)
            #expect(record.peakTerms?.hasPrefix("at 1794210239 footprint 6143 guest 4090") == true)
            #expect(record.stalls?.total == 37 && record.network?.tcp?.attempts == 12, "layout \(layout)")
            // An earlier layout has no storage slot, nor a camera slot; a blank
            // one makes no claim.
            #expect(record.storage == nil && record.camera == nil, "layout \(layout)")
        }
        #expect(SessionRecord.maximumBytes >= 19200)
        var largest = "uml-session 8\nmemory at 1 footprint 2 peak 3\n"
        largest = NetworkRecordTests.padded(String(largest.dropLast()), to: 8192)
        largest += NetworkRecordTests.padded("stalls 1 base 1 at 1 total 0 shown 0", to: 4096)
        largest += NetworkRecordTests.padded("network 1\nnet-notice denials 0 now none", to: 512)
        largest += NetworkRecordTests.padded("", to: 256)
        largest += NetworkRecordTests.padded("", to: 1024)
        largest += "state running\nend running\nstatus 0\npid 3\n"
        largest += String(repeating: "k", count: 19200 - largest.utf8.count)
        #expect(largest.utf8.count == 19200 && SessionRecord(text: largest)?.running == true)
    }

    /// The runtime's storage slot as Andarium reads it: the golden fixture the
    /// runtime's own test checks its writer against
    /// (`tests/unit/andarium/fixtures/storage-slot-golden.txt`).
    static func storageGolden() throws -> String {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/storage-slot-golden.txt")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// A whole layout 7 record with `storage` in the storage slot.
    static func layout7(storage: String) throws -> String {
        var memory = "uml-session 7\nmemory at 1794114290 footprint 2147483648 peak 2415919104 available 900000000\n"
        memory = NetworkRecordTests.padded(String(memory.dropLast()), to: 8192)
        return memory + NetworkRecordTests.padded(try StallRecordTests.golden(), to: 4096)
            + NetworkRecordTests.padded(try NetworkRecordTests.golden(), to: 512)
            + NetworkRecordTests.padded(storage, to: 256)
            + "state running\nend running\nstatus 0\npid 77\n"
    }

    /// The runtime's camera slot as Andarium reads it: the golden fixture the
    /// runtime's own test checks its writer against
    /// (`tests/unit/andarium/fixtures/camera-slot-golden.txt`).
    static func cameraGolden() throws -> String {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/camera-slot-golden.txt")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// A whole layout 8 record with `camera` in the camera slot.
    static func layout8(camera: String) throws -> String {
        var memory = "uml-session 8\nmemory at 1794114290 footprint 2147483648 peak 2415919104 available 900000000\n"
        memory = NetworkRecordTests.padded(String(memory.dropLast()), to: 8192)
        return memory + NetworkRecordTests.padded(try StallRecordTests.golden(), to: 4096)
            + NetworkRecordTests.padded(try NetworkRecordTests.golden(), to: 512)
            + NetworkRecordTests.padded(try Self.storageGolden().trimmingCharacters(in: .newlines), to: 256)
            + NetworkRecordTests.padded(camera, to: 1024)
            + "state running\nend running\nstatus 0\npid 78\n"
    }

    @Test func theGoldenCameraSlotParses() throws {
        let slot = try #require(CameraRecord(text: try Self.cameraGolden()))
        // Three stills failed; the slot has room for the newest two beside the cameras' lines.
        #expect(slot.failed == 3 && slot.recent.count == 2)
        let error = slot.recent[0]
        #expect(error.stage == "framework-error" && error.camera == 1 && error.position == "front")
        #expect(error.errorDomain == "avfoundation" && error.errorCode == -11800)
        #expect(error.compensationSupported == "yes" && error.compensationEnabled == "no")
        #expect(error.compensationReading == "after-commit")
        #expect(error.at == Date(timeIntervalSince1970: 1_794_114_290))
        #expect(error.asked == .init(width: 2016, height: 1512) && error.resolved == .init(width: 2016, height: 1512))
        #expect(error.encoded == nil && error.exifOrientation == nil)
        let encoded = slot.recent[1]
        #expect(encoded.stage == "encoded-size" && encoded.encoded == .init(width: 1512, height: 2016))
        #expect(encoded.asked == .init(width: 2016, height: 1512) && encoded.resolved == .init(width: 2016, height: 1512))
        #expect(encoded.exifOrientation == 6 && encoded.errorDomain == "none" && encoded.errorCode == nil)
    }

    /// The golden file's cameras, as the runtime wrote them: a back camera with a mount line (the slot had no room for the
    /// video line its note also holds), and a front camera that imitates an earlier one, with a line of every kind, and
    /// the front devices' line of the slot.
    @Test func theGoldenCameraSlotsCamerasParse() throws {
        let slot = try #require(CameraRecord(text: try Self.cameraGolden()))
        #expect(slot.cameras.map(\.index) == [0, 1])
        let back = slot.cameras[0]
        let backMount = try #require(back.mount)
        #expect(backMount.position == "back" && backMount.type == 1 && backMount.angles == [90, 270, 0, 180])
        #expect(backMount.stated == 90 && backMount.platformStated == 90 && backMount.turn == 0 && !backMount.earlier)
        #expect(backMount.anchor == .init(width: 4032, height: 3024) && backMount.ratios == 0 && backMount.fieldOfView == 63)
        #expect(back.video == nil && back.ratio == nil && back.photo == nil && back.refusals == nil)
        #expect(slot.fronts == .init(devices: 2, square: true, dynamicRatios: true, formatsDropped: 3))
        let front = slot.cameras[1]
        let mount = try #require(front.mount)
        #expect(mount.position == "front" && mount.angles == [0, 180, 90, 270] && mount.stated == 90 && mount.turn == 270 && mount.earlier)
        #expect(mount.anchor == .init(width: 1920, height: 1440) && mount.ratios == 24 && mount.fieldOfView == 85)
        let ratio = try #require(front.ratio)
        #expect(ratio.pinned == "3x4" && ratio.now == "3x4" && ratio.dimensions == .init(width: 1512, height: 2016))
        #expect(ratio.error == 0 && !ratio.pending)
        let video = try #require(front.video)
        #expect(video.defaultAngle == 0 && video.angle == 270 && video.angleSupported == "yes")
        #expect(video.defaultMirrored == 0 && video.mirrored == 0)
        #expect(video.asked == .init(width: 1920, height: 1440) && video.first == .init(width: 1920, height: 1440))
        let photo = try #require(front.photo)
        #expect(photo.compensationSupported == "yes" && photo.compensationEnabled == "yes")
        #expect(photo.compensationReading == "after-commit")
        #expect(photo.defaultAngle == 90 && photo.angle == 90 && photo.defaultMirrored == 0 && photo.mirrored == 0)
        #expect(photo.maximum == .init(width: 2016, height: 1512) && photo.exifOrientation == 6)
        #expect(photo.centerStage == "no" && photo.distortionCorrected == "yes")
        #expect(front.refusals?.count == 1 && front.refusals?.last == 4)
    }

    @Test func aLayout8RecordCarriesTheCameraSlot() throws {
        let session = try #require(
            SessionRecord(text: try Self.layout8(camera: try Self.cameraGolden().trimmingCharacters(in: .newlines))))
        #expect(session.processID == 78 && session.running)
        #expect(session.camera?.failed == 3 && session.camera?.recent.last?.stage == "encoded-size")
        #expect(session.camera?.cameras.count == 2)
        // The other slots are read as they are in layout 7.
        #expect(session.storage?.settles == 4 && session.stalls?.total == 37 && session.network?.tcp?.attempts == 12)
    }

    @Test func aBlankCameraSlotMakesNoClaim() throws {
        // A session in which no still failed says nothing of its stills.
        let session = try #require(SessionRecord(text: try Self.layout8(camera: "")))
        #expect(session.camera == nil && session.running && session.processID == 78)
    }

    @Test func aLayout7RecordHasNoCameraSlotEvenWithACameraLookingLine() throws {
        let seven = "uml-session 7\nmemory at 1 footprint 2 peak 3\nstate ended\nend powered-off\n"
            + (try Self.cameraGolden())
        let session = try #require(SessionRecord(text: seven))
        #expect(session.camera == nil && session.end == "powered-off")
    }

    @Test func aCameraSlotOfAnotherVersionOrWithoutACountIsNotRead() throws {
        #expect(CameraRecord(text: "") == nil)
        #expect(CameraRecord(text: "camera 2\nstills failed 1\n") == nil)
        #expect(CameraRecord(text: "stills failed 1\n") == nil)
        #expect(CameraRecord(text: "camera 1\n") == nil)
        // A line this reader cannot read is skipped, and the count stands.
        let slot = CameraRecord(text: "camera 1\nstills failed 2\nstill at x\nstill\n")
        #expect(slot?.failed == 2 && slot?.recent.isEmpty == true)
        // A later runtime may add lines and fields; a field is read by its name.
        let later = CameraRecord(
            text: "camera 1\nstills failed 1\nstill at 7 camera 1 front stage timeout asked 2016x1512 resolved - "
                + "encoded - compensation supported no enabled no exif 8 error none extra 1\nnew line\n"
                + "mount camera 2 external type 7 angles - - - - stated 90 turn 0 earlier no anchor - ratios 0 fov - extra 3\n")
        #expect(later?.recent.first?.stage == "timeout" && later?.recent.first?.compensationSupported == "no")
        #expect(later?.recent.first?.exifOrientation == 8)
        #expect(later?.cameras.first?.mount?.angles == [nil, nil, nil, nil] && later?.cameras.first?.mount?.anchor == nil)
        #expect(later?.cameras.first?.mount?.fieldOfView == nil && later?.fronts == nil)
        // A failure without the reading's field (an earlier runtime's) reads as unknown, and so does a hyphen.
        #expect(later?.recent.first?.compensationReading == "unknown")
        let hyphen = CameraRecord(
            text: "camera 1\nstills failed 1\nstill at 7 camera 1 front stage timeout asked - resolved - encoded - "
                + "compensation supported no enabled no read - exif - error none\n")
        #expect(hyphen?.recent.first?.compensationReading == "unknown")
        // A camera line without its camera, or with a mount it cannot read, is skipped.
        let broken = CameraRecord(text: "camera 1\nstills failed 0\nvideo default 0\nmount camera x back\nrefused camera 1 last 3\n")
        #expect(broken?.cameras.isEmpty == true && broken?.failed == 0)
    }

    /// A slot with notes and no failure is a record: the cameras are in it, and the count is nil of stills.
    @Test func aCameraSlotOfNotesAloneIsARecordOfNoFailure() throws {
        let slot = try #require(
            CameraRecord(
                text: "camera 1\nstills failed 0\nfronts 1 square no dynamic no dropped 4\n"
                    + "mount camera 0 front type 1 angles 0 180 90 270 stated 90 turn 270 earlier yes "
                    + "anchor 1920x1440 ratios 0 fov 85\n"
                    + "ratio camera 0 pinned 3x4 now 4x3 dims 1920x1440 error pending\n"
                    + "video camera 0 default 0 set - supported no mirror - - asked 1920x1440 first -\n"
                    + "refused camera 0 count 2 last 10\n"))
        #expect(slot.failed == 0 && slot.recent.isEmpty && slot.cameras.count == 1)
        let camera = slot.cameras[0]
        #expect(camera.ratio?.pending == true && camera.ratio?.error == nil && camera.ratio?.now == "4x3")
        #expect(camera.video?.angle == nil && camera.video?.angleSupported == "no" && camera.video?.defaultMirrored == nil)
        #expect(camera.video?.first == nil && camera.photo == nil && camera.refusals?.last == 10)
        let report = slot.report()
        #expect(report.contains("Camera 0 (front):"))
        #expect(report.contains("the guest is told 90; the video connection is turned 270; imitates an earlier front camera: yes."))
        #expect(report.contains("aspect ratios listed: none, field of view 85 degrees."))
        #expect(report.contains("Front devices listed: 1; any with a square format: no; any that lists aspect ratios: no; formats not offered to the guest: 4."))
        #expect(report.contains("the change's completion error pending."))
        #expect(report.contains("angle 0 by default and unknown now (the angle asked for is supported: no)"))
        #expect(report.contains("the first frame was not seen."))
        #expect(report.contains("refused admissions: 2, the last for the reason: the device's aspect ratio was not the one pinned."))
        #expect(report.contains("No still of the guest's cameras failed to end in a picture."))
    }

    /// A front device whose formats were all dropped is no camera: the slot's line says what the platform found of it.
    @Test func aFrontDeviceWithAllItsFormatsDroppedLeavesItsFacts() throws {
        let slot = try #require(
            CameraRecord(
                text: "camera 1\nstills failed 0\nfronts 1 square no dynamic no dropped 4\n"
                    + "mount camera 0 back type 1 angles 90 270 0 180 stated 90 turn 0 earlier no anchor 4032x3024 ratios 0 fov 63\n"))
        #expect(slot.fronts == .init(devices: 1, square: false, dynamicRatios: false, formatsDropped: 4))
        #expect(slot.cameras.map(\.index) == [0] && slot.cameras[0].mount?.position == "back")
        let report = slot.report()
        #expect(report.contains("Front devices listed: 1; any with a square format: no; any that lists aspect ratios: no; formats not offered to the guest: 4."))
        // A line it cannot read is no claim.
        #expect(CameraRecord(text: "camera 1\nstills failed 0\nfronts x\n")?.fronts == nil)
        #expect(CameraRecord(text: "camera 1\nstills failed 0\nfronts 1 square no\n")?.fronts == nil)
    }

    /// A tablet boots turned: the guest is told another orientation than the platform states, and the report says which is
    /// which. The measured values of a tablet: 180 for the front camera and 0 for the back, the platform's own 90 for both.
    @Test func theTwoOrientationsOfATabletReadApart() throws {
        let slot = try #require(
            CameraRecord(
                text: "camera 1\nstills failed 0\n"
                    + "mount camera 0 back type 1 angles 90 270 0 180 stated 0 platform 90 turn 0 earlier no anchor 4032x3024 ratios 0 fov 63\n"
                    + "mount camera 1 front type 1 angles 90 270 180 0 stated 180 platform 90 turn 0 earlier no anchor 1920x1440 ratios 0 fov 85\n"))
        let back = try #require(slot.cameras[0].mount)
        #expect(back.stated == 0 && back.platformStated == 90)
        let front = try #require(slot.cameras[1].mount)
        #expect(front.stated == 180 && front.platformStated == 90)
        let report = slot.report()
        #expect(report.contains("the guest is told 0 (the platform states 90 for it); the video connection is turned 0"))
        #expect(report.contains("the guest is told 180 (the platform states 90 for it); the video connection is turned 0"))
        // A phone, whose line has no second number, states one: the guest is told what the platform states.
        let phone = try #require(CameraRecord(text: try Self.cameraGolden()))
        #expect(phone.cameras.allSatisfy { $0.mount?.platformStated == $0.mount?.stated })
        #expect(!phone.report().contains("the platform states"))
    }

    /// The reasons the platform refuses an admission, by the runtime's code.
    @Test func theRefusalReasonsOfTheStillChecksReadInWords() throws {
        for (code, reason) in [
            (11, "the video connection does not hold the turn"), (12, "the photo output does not turn its stills"),
            (13, "the device's aspect ratio change failed"),
            (14, "the device's aspect ratio change did not complete in time"),
        ] {
            let slot = try #require(CameraRecord(text: "camera 1\nstills failed 0\nrefused camera 0 count 1 last \(code)\n"
                + "mount camera 0 front type 1 angles 0 180 90 270 stated 90 turn 270 earlier yes anchor - ratios 0 fov -\n"))
            #expect(slot.report().contains("the last for the reason: \(reason)."))
        }
    }

    @Test func theCameraFailuresReadInWords() throws {
        let slot = try #require(CameraRecord(text: try Self.cameraGolden()))
        let report = slot.report()
        #expect(report.contains("3 stills of the guest's cameras did not end in a picture."))
        #expect(report.contains("Camera 1 (front):"))
        #expect(report.contains("mounting: device type wide-angle; the platform's fixed angles for portrait, portrait upside down, landscape right and landscape left are 0, 180, 90, 270; the guest is told 90; the video connection is turned 270; imitates an earlier front camera: yes."))
        #expect(report.contains("anchor: 1920x1440, aspect ratios listed: 4x3 3x4, field of view 85 degrees."))
        #expect(report.contains("aspect ratio: pinned 3x4, the device holds 3x4, output dimensions 1512x2016, the change's completion error 0."))
        #expect(report.contains("video connection: angle 0 by default and 270 now (the angle asked for is supported: yes)"))
        #expect(report.contains("photo output: orientation compensation supported yes and enabled yes (read after the session's configuration was committed); photo connection angle 90 by default and 90 now"))
        #expect(report.contains("Front devices listed: 2; any with a square format: yes; any that lists aspect ratios: yes; formats not offered to the guest: 3."))
        #expect(report.contains("largest photo 2016x1512; Exif orientation of the last photo 6; Center Stage active no; distortion correction yes."))
        #expect(report.contains("camera 1 (front), stopped at framework-error, asked 2016x1512, resolved 2016x1512"))
        #expect(report.contains("the framework's error avfoundation -11800"))
        #expect(report.contains("file states 1512x2016, orientation compensation supported yes and enabled no (read after the session's configuration was committed), the host photo's Exif orientation 6"))
        // Only the failures the slot holds are listed, and the report says so.
        let more = try #require(CameraRecord(text: "camera 1\nstills failed 50\n" + (try Self.cameraGolden()).split(separator: "\n").dropFirst(2).joined(separator: "\n")))
        #expect(more.report().contains("The last 2 that the record holds, oldest first:"))
    }

    @Test func theCameraSlotIsAnEntryOfTheBugReportOnlyWhenARecordHasOne() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-camera-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let none = BugReportAttachments.entries(from: directory, containers: [])
        #expect(!none.contains { $0.name == BugReportAttachments.cameraReport })
        try Self.layout8(camera: "").write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        let blank = BugReportAttachments.entries(from: directory, containers: [])
        #expect(!blank.contains { $0.name == BugReportAttachments.cameraReport })
        try Self.layout8(camera: try Self.cameraGolden().trimmingCharacters(in: .newlines)).write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        let report = try #require(entries.first { $0.name == BugReportAttachments.cameraReport })
        let text = String(decoding: report.data, as: UTF8.self)
        #expect(text.contains("== The current session (process 78) =="))
        #expect(text.contains("stopped at encoded-size"))
        #expect(text.contains("Camera 1 (front):") && text.contains("imitates an earlier front camera: yes."))
        #expect(text.contains("== The previous session ==") && text.contains("There is no record."))
        // A record whose slot holds notes and no failure is an entry too, and says so.
        try Self.layout8(camera: "camera 1\nstills failed 0\nmount camera 0 back type 1 angles 90 270 0 180 stated 90 turn 0 "
            + "earlier no anchor 4032x3024 ratios 0 fov 63\n").write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        let notes = BugReportAttachments.entries(from: directory, containers: [])
        let noted = String(decoding: try #require(notes.first { $0.name == BugReportAttachments.cameraReport }).data, as: UTF8.self)
        #expect(noted.contains("Camera 0 (back):") && noted.contains("No still of the guest's cameras failed to end in a picture."))
    }

    @Test func theGoldenStorageSlotParses() throws {
        let slot = try #require(StorageRecord(text: try Self.storageGolden()))
        #expect(slot.settles == 4 && slot.unsettled == 2)
        let last = try #require(slot.last)
        #expect(last.call == "quiesce" && last.storage == "confirmed" && last.suspension == "suspended")
        #expect(last.at == Date(timeIntervalSince1970: 1_794_114_290) && last.settled)
    }

    @Test func aLayout7RecordCarriesTheStorageSlot() throws {
        let session = try #require(
            SessionRecord(text: try Self.layout7(storage: try Self.storageGolden().trimmingCharacters(in: .newlines))))
        #expect(session.processID == 77 && session.running)
        #expect(session.storage?.settles == 4 && session.storage?.unsettled == 2)
        // The other slots are read as they are in layout 6.
        #expect(session.stalls?.total == 37 && session.network?.tcp?.attempts == 12)
        #expect(session.memory?.footprintBytes == 2 << 30)
    }

    @Test func aBlankStorageSlotMakesNoClaim() throws {
        // A session that never settled the guest says nothing of it: neither
        // that its storage was settled nor that it was not.
        let session = try #require(SessionRecord(text: try Self.layout7(storage: "")))
        #expect(session.storage == nil && session.running && session.processID == 77)
    }

    @Test func aLayout6RecordHasNoStorageSlotEvenWithAStorageLookingLine() throws {
        let six = "uml-session 6\nmemory at 1 footprint 2 peak 3\nstate ended\nend powered-off\n"
            + "storage 1\nsettles 3 unsettled 3\nlast sync unreachable not-asked at 5\n"
        let session = try #require(SessionRecord(text: six))
        #expect(session.storage == nil && session.end == "powered-off")
    }

    @Test func anUnsettledLastOutcomeIsNotSettled() throws {
        let text = "storage 1\nsettles 1 unsettled 1\nlast sync unreachable not-asked at 1794114290\n"
        let slot = try #require(StorageRecord(text: text))
        #expect(slot.settles == 1 && slot.unsettled == 1 && slot.last?.settled == false)
        // A guest that was not running has nothing to settle.
        let none = try #require(
            StorageRecord(text: "storage 1\nsettles 1 unsettled 0\nlast sync no-guest not-asked at 1794114290\n"))
        #expect(none.last?.settled == true)
    }

    @Test func aStorageSlotOfAnotherVersionOrWithoutCountsIsNotRead() {
        #expect(StorageRecord(text: "") == nil)
        #expect(StorageRecord(text: "storage 2\nsettles 1 unsettled 0\n") == nil)
        #expect(StorageRecord(text: "settles 1 unsettled 0\n") == nil)
        #expect(StorageRecord(text: "storage 1\nsettles 1\n") == nil)
        // A last line this reader cannot read leaves the counts.
        let slot = StorageRecord(text: "storage 1\nsettles 2 unsettled 1\nlast sync\n")
        #expect(slot?.settles == 2 && slot?.last == nil)
        // A later runtime may add lines and fields.
        let later = StorageRecord(
            text: "storage 1\nsettles 2 unsettled 1\nlast quiesce refused not-suspended at 7 extra 1\nnew line\n")
        #expect(later?.last?.storage == "refused")
    }

    @Test func otherTextIsNoRecord() {
        #expect(SessionRecord(text: "") == nil)
        #expect(SessionRecord(text: "uml-session 9\nstate ended\nend powered-off\n") == nil)
        #expect(SessionRecord(text: "uml-session 2\nstatus 0\n") == nil)
        #expect(SessionRecord.read(at: URL(fileURLWithPath: "/nonexistent/session.txt")) == nil)
    }

    @Test func redactionTakesTheContainerPathsOut() {
        let container = "/private/var/mobile/Containers/Data/Application/0A1B2C3D-0000-4000-8000-0123456789AB"
        let text = """
            [    0.36] Not locking "\(container)/Library/Application Support/android/data.img" on the host
            opened /var/mobile/Containers/Data/Application/0A1B2C3D-0000-4000-8000-0123456789AB/tmp/x
            bundle /private/var/containers/Bundle/Application/11111111-2222-4333-8444-555555555555/Andarium.app
            mac /Users/NAME/Library/Containers/app/Data/x
            """
        let redacted = DiagnosticRedaction.redact(text, containers: [container])
        #expect(!redacted.contains("0A1B2C3D") && !redacted.contains("11111111") && !redacted.contains("/Users/NAME"))
        #expect(redacted.contains("Not locking \"<container>/Library/Application Support/android/data.img\""))
        #expect(redacted.contains("opened <container>/tmp/x"))
        #expect(redacted.contains("bundle <container>/Andarium.app"))
        #expect(redacted.contains("mac /Users/<user>/Library/Containers/app/Data/x"))
    }
}

struct RunRecordTests {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("run-record-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private func state(_ ramdisk: String, ended: Bool, started: Int) -> Data {
        var slot = "uml-session 8\nmemory at 1794114290 footprint 2147483648 peak 2415919104\n"
        slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
        let text = ended
            ? "state ended\nend guest-panic\nstatus 0\npid 7\nstarted \(started)\nended \(started + 3)\nboot unobserved\nrestarts 0\nramdisk \(ramdisk)\n"
            : "state running\nend running\nstatus 0\npid 7\nstarted \(started)\nboot unobserved\nrestarts 0\nramdisk \(ramdisk)\n"
        return Data((slot + text).utf8)
    }

    @Test func aNewRunsRecordIsTheOneAtItsPlaceWhateverTheClockSaysOfTheOneBefore() throws {
        let diagnostics = try directory()
        defer { try? FileManager.default.removeItem(at: diagnostics) }
        let file = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        let kept = diagnostics.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        let run = RunRecord(in: diagnostics)
        // None yet.
        #expect(run.read() == nil && !run.retiredAnEarlierRun)
        // A run of this process ended a moment ago: its record says it was the Magisk ramdisk's, and it
        // began in the same whole second as the next run's begin, or after the clock stepped back.
        let second = Int(Date().timeIntervalSince1970)
        try state("root", ended: true, started: second + 5).write(to: file)
        #expect(run.read()?.ramdisk == .root)
        // The next run is asked for: nothing of the earlier one stays at the place.
        run.retire()
        #expect(run.retiredAnEarlierRun, "a record this process moved aside is its own")
        #expect(run.read() == nil, "the record of the run before was taken for the next run's")
        #expect(SessionRecord.read(at: kept)?.ramdisk == .root)
        // The runtime begins its record; that one is the run's.
        try state("stock", ended: false, started: second).write(to: file)
        let current = try #require(run.read())
        #expect(current.running && current.ramdisk == .stock)
        // And at the next run it goes the same way.
        run.retire()
        #expect(run.read() == nil)
        #expect(SessionRecord.read(at: kept)?.ramdisk == .stock)
    }

    @Test func thePreviousProcessesRecordIsNotTouchedByAnyRetirement() throws {
        let diagnostics = try directory()
        defer { try? FileManager.default.removeItem(at: diagnostics) }
        let file = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        let previous = diagnostics.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        let earlier = diagnostics.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        let run = RunRecord(in: diagnostics)
        // The launch kept the record of the process that ended badly.
        let ended = state("recovery", ended: true, started: 50)
        try ended.write(to: previous)
        // Three runs of this process, each asked for after the one before ended.
        for (ramdisk, started) in [("stock", 100), ("root", 200)] {
            try state(ramdisk, ended: true, started: started).write(to: file)
            #expect(run.retire() == .moved)
            #expect(try Data(contentsOf: previous) == ended, "the previous process's record was replaced by a run's")
        }
        // The last earlier run is the one kept beside it.
        #expect(SessionRecord.read(at: earlier)?.ramdisk == .root)
        #expect(run.retire() == .nothing)
        #expect(try Data(contentsOf: previous) == ended)
    }

    /// Takes the write access to what is in `directory` away, as a file that
    /// cannot be replaced does: `body` runs with the directory unwritable
    /// (nothing in it can be moved or removed) and, when `file` is given,
    /// that file unwritable too.
    private func withoutWriteAccess(to directory: URL, file: URL? = nil, _ body: () throws -> Void) throws {
        let manager = FileManager.default
        if let file { try manager.setAttributes([.posixPermissions: 0o400], ofItemAtPath: file.path) }
        try manager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: directory.path)
        defer {
            try? manager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory.path)
            if let file { try? manager.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path) }
        }
        try body()
    }

    @Test(.enabled(if: geteuid() != 0))
    func aRecordThatCannotBeMovedAsideIsEmptiedAndNeverTakenForTheNextRuns() throws {
        let diagnostics = try directory()
        defer { try? FileManager.default.removeItem(at: diagnostics) }
        let file = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        let run = RunRecord(in: diagnostics)
        try state("root", ended: true, started: 100).write(to: file)
        try withoutWriteAccess(to: diagnostics) {
            // Nothing in the directory can be moved: the record can only be emptied where it is.
            #expect(run.retire() == .emptied)
        }
        #expect(!run.retiredAnEarlierRun)
        #expect(run.read() == nil, "the record of the run before was taken for the next run's")
        // The runtime opens the same file and writes the run's own.
        let handle = try FileHandle(forWritingTo: file)
        try handle.write(contentsOf: state("stock", ended: false, started: 200))
        try handle.close()
        let current = try #require(run.read())
        #expect(current.running && current.ramdisk == .stock)
    }

    @Test(.enabled(if: geteuid() != 0))
    func aRecordThatCanBeNeitherMovedNorEmptiedIsNotReadUntilItIsGotOutOfTheWay() throws {
        let diagnostics = try directory()
        defer { try? FileManager.default.removeItem(at: diagnostics) }
        let file = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        let kept = diagnostics.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        let run = RunRecord(in: diagnostics)
        try state("root", ended: true, started: 100).write(to: file)
        try withoutWriteAccess(to: diagnostics, file: file) {
            #expect(run.retire() == .stays)
            #expect(!run.retiredAnEarlierRun)
            // The file is still a record, and it is not the run's.
            #expect(SessionRecord.read(at: file)?.ramdisk == .root)
            #expect(run.read() == nil, "a record that stays in place was taken for the next run's")
            // And it stays unread at every look, whatever else changes.
            #expect(run.read() == nil)
        }
        // Once it can be moved, the next run is asked for and its record is its own.
        #expect(run.retire() == .moved)
        #expect(run.retiredAnEarlierRun)
        #expect(run.read() == nil)
        #expect(SessionRecord.read(at: kept)?.ramdisk == .root)
        try state("stock", ended: false, started: 200).write(to: file)
        #expect(run.read()?.ramdisk == .stock)
    }
}

/// An `earlier-run.txt` is a process's own: a launch files or drops the one
/// the process before left (`PreviousSession.settleEarlierRun`).
struct EarlierRunAtLaunchTests {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("earlier-run-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private func text(pid: Int, end: String, ramdisk: String = "stock") -> Data {
        var slot = "uml-session 8\nmemory at 1794114290 footprint 2147483648 peak 2415919104\n"
        slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
        let state = "state ended\nend \(end)\nstatus 0\npid \(pid)\nstarted 1000\nended 1003\n"
            + "boot unobserved\nrestarts 0\nramdisk \(ramdisk)\n"
        return Data((slot + state).utf8)
    }

    private func url(_ name: String, in directory: URL) -> URL { directory.appendingPathComponent(name) }

    private func exists(_ name: String, in directory: URL) -> Bool {
        FileManager.default.fileExists(atPath: url(name, in: directory).path)
    }

    private func pid(of name: String, in directory: URL) -> Int? {
        SessionRecord.read(at: url(name, in: directory))?.processID
    }

    @Test func aProcessThatRanTwiceLeavesItsLastRunAsThePreviousSessionAndNoEarlierRun() throws {
        let diagnostics = try directory()
        defer { try? FileManager.default.removeItem(at: diagnostics) }
        // The process before that ended in a crash, kept at the launch of this one (pid 50).
        try text(pid: 40, end: "guest-panic").write(to: url(BugReportAttachments.previousSessionRecord, in: diagnostics))
        // Two runs; the second ends in a panic.
        let run = RunRecord(in: diagnostics)
        try text(pid: 50, end: "powered-off").write(to: url(BugReportAttachments.sessionRecord, in: diagnostics))
        #expect(run.retire() == .moved)
        try text(pid: 50, end: "guest-panic").write(to: url(BugReportAttachments.sessionRecord, in: diagnostics))
        #expect(pid(of: BugReportAttachments.earlierRunRecord, in: diagnostics) == 50)
        // The next launch.
        PreviousSession.settleEarlierRun(in: diagnostics)
        let previous = try #require(PreviousSession.consume(in: diagnostics, endedOutOfForeground: false))
        #expect(previous.record.end == "guest-panic" && previous.record.processID == 50)
        #expect(pid(of: BugReportAttachments.previousSessionRecord, in: diagnostics) == 50)
        #expect(!exists(BugReportAttachments.earlierRunRecord, in: diagnostics), "the first run outlived its process")
        // Whatever a caller says, no run of this process is reported before it has one.
        let entries = BugReportAttachments.entries(from: diagnostics, containers: [], includingEarlierRun: true)
        #expect(entries.map(\.name) == ["andarium/session-previous.txt"])
    }

    @Test func aPausedFailedStartThatWasLeftIsThePreviousSessionAtTheNextLaunchAndDecidesNothing() throws {
        let diagnostics = try directory()
        defer { try? FileManager.default.removeItem(at: diagnostics) }
        try text(pid: 40, end: "guest-panic").write(to: url(BugReportAttachments.previousSessionRecord, in: diagnostics))
        // The failed start's pause was saved and its record moved aside; the person left.
        let run = RunRecord(in: diagnostics)
        try text(pid: 50, end: "guest-panic", ramdisk: "root").write(
            to: url(BugReportAttachments.sessionRecord, in: diagnostics))
        run.retire()
        PreviousSession.settleEarlierRun(in: diagnostics)
        #expect(pid(of: BugReportAttachments.previousSessionRecord, in: diagnostics) == 50)
        #expect(!exists(BugReportAttachments.earlierRunRecord, in: diagnostics))
        // Nothing is at the place the launch decides from, so its pause is not decided again.
        #expect(PreviousSession.read(in: diagnostics, endedOutOfForeground: false) == nil)
        let entries = BugReportAttachments.entries(from: diagnostics, containers: [], includingEarlierRun: true)
        #expect(entries.map(\.name) == ["andarium/session-previous.txt"])
    }

    @Test(.enabled(if: geteuid() != 0))
    func anEarlierRunThatCannotBeFiledOrDroppedIsNotReportedAsThisProcesss() throws {
        let diagnostics = try directory()
        defer {
            try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: diagnostics.path)
            try? FileManager.default.removeItem(at: diagnostics)
        }
        try text(pid: 50, end: "powered-off").write(to: url(BugReportAttachments.earlierRunRecord, in: diagnostics))
        try text(pid: 51, end: "guest-panic").write(to: url(BugReportAttachments.sessionRecord, in: diagnostics))
        try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: diagnostics.path)
        PreviousSession.settleEarlierRun(in: diagnostics)
        // It stays; this process retired nothing, so it is not carried.
        #expect(exists(BugReportAttachments.earlierRunRecord, in: diagnostics))
        #expect(!RunRecord(in: diagnostics).retiredAnEarlierRun)
        #expect(BugReportAttachments.entries(from: diagnostics, containers: []).map(\.name)
            == ["andarium/session-current.txt"])
    }
}
