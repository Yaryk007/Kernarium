// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// The device's storage as the application reads it.
public enum HostStorage {
    /// The space a write can use on the volume of `directory` right now, the
    /// one figure every guard and every screen of both applications holds to:
    /// the volume's available blocks times its block size, as `statfs(2)`
    /// states them. The system's "available for important usage" figure (what
    /// Settings shows) adds purgeable space, which a write that finds the
    /// volume full cannot use and fails on, so it can be larger. Zero when the
    /// volume cannot be read, which reads as full.
    ///
    /// `directory` is the application's container unless a caller names
    /// another place. The read is one system call, so any thread may make it
    /// whenever it needs the figure.
    public static func freeBytes(on directory: URL? = nil) -> UInt64 {
        var volume = statfs()
        let status = (directory ?? container).withUnsafeFileSystemRepresentation { path -> Int32 in
            guard let path else { return -1 }
            return statfs(path, &volume)
        }
        return status == 0 ? writableBytes(of: volume) : 0
    }

    private static let container = URL(fileURLWithPath: NSHomeDirectory())

    /// What a `statfs(2)` answer says a write can use: `f_bavail` counts the
    /// blocks left for a process without special privilege, in `f_bsize`
    /// units.
    static func writableBytes(of volume: statfs) -> UInt64 {
        let (bytes, overflow) = UInt64(volume.f_bavail).multipliedReportingOverflow(by: UInt64(volume.f_bsize))
        return overflow ? .max : bytes
    }
}
