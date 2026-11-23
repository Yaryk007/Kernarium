// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the scene root's update reads, which is all that can change what the
/// update does. The root runs it after a layout, a geometry change or a phase
/// change, and it only has something to do when one of these differs from
/// where the last run found it: the deferred edges follow the phase, the scene
/// and its screen; the guest follows the policy (which follows the phase) and
/// the interface orientation (the sensors' mount). Everything else the guest
/// follows reaches it by its own trigger, not through the root.
public struct SceneUpdateInputs: Equatable, Sendable {
    /// The scene the root is in, and the driver the update tells.
    public var scene: ObjectIdentifier
    public var driver: ObjectIdentifier?
    public var sceneSize: CGSize
    public var screenSize: CGSize
    /// `UIInterfaceOrientation`'s raw value.
    public var interfaceOrientation: Int
    public var phase: Phase
    /// Whether the interface of the display the scene is on turns with the
    /// device. It changes on a folding iPhone when the scene moves between the
    /// cover and the inner display.
    public var interfaceRotates: Bool

    public init(
        scene: ObjectIdentifier, driver: ObjectIdentifier?, sceneSize: CGSize, screenSize: CGSize,
        interfaceOrientation: Int, phase: Phase, interfaceRotates: Bool = false
    ) {
        self.scene = scene
        self.driver = driver
        self.sceneSize = sceneSize
        self.screenSize = screenSize
        self.interfaceOrientation = interfaceOrientation
        self.phase = phase
        self.interfaceRotates = interfaceRotates
    }
}
