// tools/window-size.swift - M116: the size of a process's window, in points.
//
//   swift tools/window-size.swift <pid>
//
// Prints "<width> <height>" for the largest on-screen window the process
// owns, and exits 1 if it owns none. Bounds come from CGWindowList, which
// reports them without the screen-recording or accessibility permission
// a screenshot or System Events would need - so the test that uses this
// runs on a developer's machine as it is. Host tooling, like everything
// under tools/; nothing here reaches the image.
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2, let pid = Int(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: window-size.swift <pid>\n".data(using: .utf8)!)
    exit(2)
}
let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
var best: (Int, Int)? = nil
for w in windows {
    guard (w[kCGWindowOwnerPID as String] as? Int) == pid,
          (w[kCGWindowLayer as String] as? Int) == 0,
          let b = w[kCGWindowBounds as String] as? [String: Any],
          let width = b["Width"] as? Double, let height = b["Height"] as? Double else { continue }
    if best == nil || Int(width * height) > best!.0 * best!.1 {
        best = (Int(width), Int(height))
    }
}
if let (w, h) = best {
    print("\(w) \(h)")
} else {
    exit(1)
}
