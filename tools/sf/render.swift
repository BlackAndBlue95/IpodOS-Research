// render.swift LIST OUTDIR — renders Apple SF Symbols (or system-font text) as 8-bit alpha masks.
// LIST lines: name W H bx by bw bh weight rot what   (what = SF symbol name, or "text:STRING"; rot degrees clockwise)
// The glyph is fitted into the box (bx,by,bw,bh) keeping its aspect, centred. Output OUTDIR/name.a (W*H bytes).
import AppKit
let args = CommandLine.arguments
let lines = try! String(contentsOfFile: args[1], encoding: .utf8).split(separator: "\n")
let weights: [String: NSFont.Weight] = ["ultralight": .ultraLight, "thin": .thin, "light": .light, "regular": .regular,
    "medium": .medium, "semibold": .semibold, "bold": .bold, "heavy": .heavy, "black": .black]
for l in lines {
    let f = l.split(separator: " ", maxSplits: 9).map(String.init)
    if f.count < 10 || f[0].hasPrefix("#") { continue }
    let (W, H) = (Int(f[1])!, Int(f[2])!)
    let box = NSRect(x: Double(f[3])!, y: Double(f[4])!, width: Double(f[5])!, height: Double(f[6])!)
    let wt = weights[f[7]] ?? .regular
    var img: NSImage? = nil
    if f[9].hasPrefix("text:") {
        let s = String(f[9].dropFirst(5)) as NSString
        let font = NSFont.monospacedDigitSystemFont(ofSize: 400, weight: wt)
        let attr: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.black]
        // tight glyph bounds
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s as String, attributes: attr))
        let gb = CTLineGetImageBounds(line, nil)
        img = NSImage(size: NSSize(width: gb.width, height: gb.height), flipped: false) { _ in
            let ctx = NSGraphicsContext.current!.cgContext
            ctx.textPosition = CGPoint(x: -gb.minX, y: -gb.minY)
            CTLineDraw(line, ctx); return true }
    } else {
        let cfg = NSImage.SymbolConfiguration(pointSize: 400, weight: wt)
        img = NSImage(systemSymbolName: f[9], accessibilityDescription: nil)?.withSymbolConfiguration(cfg)
    }
    guard let im = img else { FileHandle.standardError.write("missing symbol \(f[9])\n".data(using: .utf8)!); continue }
    let sc = min(box.width / im.size.width, box.height / im.size.height)
    let dw = im.size.width * sc, dh = im.size.height * sc
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: W, pixelsHigh: H, bitsPerSample: 8, samplesPerPixel: 4,
        hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: W * 4, bitsPerPixel: 32)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    // box is given top-down (y from the top); AppKit is bottom-up
    let x = box.minX + (box.width - dw) / 2, ytop = box.minY + (box.height - dh) / 2
    let rot = Double(f[8])!
    if rot != 0 {
        let ctx = NSGraphicsContext.current!.cgContext
        let cx = box.midX, cy = Double(H) - box.midY
        ctx.translateBy(x: cx, y: cy); ctx.rotate(by: -rot * .pi / 180); ctx.translateBy(x: -cx, y: -cy)
    }
    im.draw(in: NSRect(x: x, y: Double(H) - ytop - dh, width: dw, height: dh))
    NSGraphicsContext.restoreGraphicsState()
    var out = [UInt8](repeating: 0, count: W * H)
    let p = rep.bitmapData!
    for i in 0..<(W * H) { out[i] = p[i * 4 + 3] }
    FileManager.default.createFile(atPath: args[2] + "/" + f[0] + ".a", contents: Data(out))
}
