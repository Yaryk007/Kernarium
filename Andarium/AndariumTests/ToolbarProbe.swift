// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit

/// Reads the navigation bar's items the way the system holds them, so a test
/// sees what a person would be offered: SwiftUI's toolbar items become
/// `UIBarButtonItem`s of the navigation items in the controller tree.
@MainActor
enum ToolbarProbe {
    /// Every bar button item of every navigation item under `root`, once each.
    static func barItems(under root: UIViewController) -> [UIBarButtonItem] {
        var items: [UIBarButtonItem] = []
        var seenItems = Set<ObjectIdentifier>()
        var seenControllers = Set<ObjectIdentifier>()
        func add(_ item: UIBarButtonItem) {
            if seenItems.insert(ObjectIdentifier(item)).inserted { items.append(item) }
        }
        func visit(_ controller: UIViewController) {
            guard seenControllers.insert(ObjectIdentifier(controller)).inserted else { return }
            let navigation = controller.navigationItem
            let groups = navigation.leadingItemGroups + navigation.centerItemGroups + navigation.trailingItemGroups
            for item in (navigation.leftBarButtonItems ?? []) + (navigation.rightBarButtonItems ?? [])
                + groups.flatMap(\.barButtonItems) {
                add(item)
            }
            controller.children.forEach(visit)
            (controller as? UINavigationController)?.viewControllers.forEach(visit)
            if let presented = controller.presentedViewController { visit(presented) }
        }
        visit(root)
        return items
    }

    /// A line for a failure message: what the bar holds.
    static func describe(under root: UIViewController) -> String {
        barItems(under: root).map { item in
            "\(item.title ?? item.image?.description ?? "item") menu=\(item.menu.map { "\($0.children.map { type(of: $0) })" } ?? "none")"
        }.joined(separator: "; ")
    }

    /// What the accessibility tree holds in the top of the window: the elements the bar shows.
    struct Element {
        let object: NSObject
        let label: String
        let traits: UIAccessibilityTraits
        let top: CGFloat
        var description: String { "\(type(of: object)) label=\(label) top=\(Int(top))" }
    }

    /// Every accessibility element of the window's views and their containers that has a label
    /// (or, when asked, is an element without one) and lies above `limit` on the screen.
    static func elements(in window: UIWindow, above limit: CGFloat = 220, includeUnlabelled: Bool = false) -> [Element] {
        var found: [Element] = []
        var seen = Set<ObjectIdentifier>()
        func visit(_ object: NSObject, depth: Int) {
            guard depth < 60, seen.insert(ObjectIdentifier(object)).inserted else { return }
            let label = object.accessibilityLabel ?? ""
            if !label.isEmpty || (includeUnlabelled && object.isAccessibilityElement) {
                let frame = object.accessibilityFrame
                if frame.minY < limit {
                    found.append(Element(object: object, label: label, traits: object.accessibilityTraits, top: frame.minY))
                }
            }
            if let elements = object.accessibilityElements {
                for case let child as NSObject in elements { visit(child, depth: depth + 1) }
            } else {
                let count = object.accessibilityElementCount()
                if count != NSNotFound, count > 0 {
                    for index in 0..<count {
                        if let child = object.accessibilityElement(at: index) as? NSObject { visit(child, depth: depth + 1) }
                    }
                }
            }
            if let view = object as? UIView {
                for subview in view.subviews { visit(subview, depth: depth + 1) }
            }
        }
        visit(window, depth: 0)
        return found
    }
}
