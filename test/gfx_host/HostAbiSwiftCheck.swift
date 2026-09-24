import Foundation
import PocketUIHost

// Local interop check only. The app bridge and its pinned artifact are separate.
guard CommandLine.arguments.count == 2 else { fatalError("Expected a local cpfont path") }
let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: CommandLine.arguments[1]))
defer { try? handle.close() }
let font = try handle.read(upToCount: 64 * 1024 * 1024 + 1) ?? Data()
precondition(!font.isEmpty && font.count <= 64 * 1024 * 1024)
var context: OpaquePointer?
let created = font.withUnsafeBytes { bytes in
    pdui_create(792, 528, 0, bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count, &context)
}
precondition(created == PDUI_OK && context != nil)
defer { pdui_destroy(context) }
precondition(pdui_abi_version() == 1)

func setText<T>(_ text: String, _ field: inout T) {
    let bytes = Array(text.utf8) + [0]
    withUnsafeMutableBytes(of: &field) { output in
        precondition(bytes.count <= output.count)
        output.initializeMemory(as: UInt8.self, repeating: 0)
        output.copyBytes(from: bytes)
    }
}
var options = pdui_content_options()
options.side_padding = 12
options.top_padding = 8
options.spacing = 4
setText("Pocket", &options.empty_title)
setText("아직 카드가 없습니다", &options.empty_message)
setText("Back", &options.labels.0)
precondition(pdui_render_content(context, nil, 0, nil, 0, &options) == PDUI_OK)
var info = pdui_frame_info()
precondition(pdui_get_frame_info(context, &info) == PDUI_OK)
precondition(info.byte_count == 52_272 && info.logical_width == 528 && info.logical_height == 792)
var pixels = [UInt8](repeating: 0xA5, count: Int(info.byte_count))
let copied = pixels.withUnsafeMutableBufferPointer { output in
    pdui_copy_frame(context, output.baseAddress, output.count)
}
precondition(copied == PDUI_OK && pixels.contains(where: { $0 != 0xFF }))
precondition(pdui_render_content(context, nil, 0, nil, 0, nil) == PDUI_INVALID_ARGUMENT)
precondition(pdui_get_frame_info(context, &info) == PDUI_NO_FRAME && info.byte_count == 0)
print("Swift imported C ABI, rendered Korean empty state, copied 52272 bytes and rejected a stale frame.")
