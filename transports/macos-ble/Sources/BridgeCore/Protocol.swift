// BridgeCore/Protocol.swift —— 桥接程序的纯逻辑:与 v1 串口协议一致的拼行、HELLO 识别、
// 转发过滤,以及写给守护进程的状态文件。见外层仓库 docs/specs/2026-11-08-agent-run-wireless-design.md §5。
import Foundation

/// 单行最大字节数(不含行尾),与设备端 AR_PROTO_MAX_LINE、主机端 MAX_LINE 一致
public let maxLineBytes = 160

/// 把蓝牙通知的分片拼成整行。\r、\n、\r\n 都算行尾;超长的行整行丢弃,直到下一个行尾。
public struct LineAssembler {
    private var buffer = Data()
    private var overflow = false

    public init() {}

    /// 喂入一段数据,返回其中完整的行(不含行尾,空行不返回)
    public mutating func push(_ chunk: Data) -> [String] {
        var lines: [String] = []
        for byte in chunk {
            if byte == 0x0A || byte == 0x0D {
                if !overflow, !buffer.isEmpty, let line = String(data: buffer, encoding: .utf8) {
                    lines.append(line)
                }
                buffer.removeAll(keepingCapacity: true)
                overflow = false
            } else if overflow {
                continue
            } else if buffer.count >= maxLineBytes {
                overflow = true
                buffer.removeAll(keepingCapacity: true)
            } else {
                buffer.append(byte)
            }
        }
        return lines
    }
}

/// 解析设备的 HELLO 行,返回固件版本;不是 HELLO 时返回 nil
public func parseHelloFirmware(_ line: String) -> String? {
    let parts = line.split(separator: " ")
    guard parts.count >= 2, parts[0] == "AR1", parts[1] == "HELLO" else { return nil }
    for part in parts.dropFirst(2) where part.hasPrefix("fw=") {
        return String(part.dropFirst(3))
    }
    return "?"
}

/// 守护进程发来的一个数据报是否可以转发:必须是以 "AR1 " 开头、以 \n 结尾的单行,且不超长
public func isForwardable(_ data: Data) -> Bool {
    guard data.count >= 5, data.count <= maxLineBytes + 1, data.last == 0x0A else { return false }
    guard data.prefix(4) == Data("AR1 ".utf8) else { return false }
    return !data.dropLast().contains(0x0A) && !data.contains(0x0D)
}

/// CoreBluetooth 连接错误码 14(CBError.peerRemovedPairingInformation):
/// 设备端的配对信息已被清除(例如从 0x0 重新烧录了合并镜像),macOS 会一直拒绝连接,
/// 只有用户在系统设置里移除设备、重新配对才能恢复。返回给用户看的提示,其他错误返回 nil。
public func hintForConnectError(code: Int, deviceName: String) -> String? {
    guard code == 14 else { return nil }
    let name = deviceName.isEmpty ? "AgentRun 设备" : deviceName
    return "\(name) 上的配对信息已清除(例如重新烧录过固件):请在 系统设置 → 蓝牙 中移除 \(name),"
        + "然后在设备上确认重新配对"
}

/// 写给守护进程的状态文件内容(run/ble-status.json)。
/// connected 只有在设备回过 HELLO(即设备已确认这台电脑)后才为 true。
public struct BridgeStatus: Encodable, Equatable {
    public var connected: Bool
    public var name: String
    public var fw: String
    public var updated: Double
    /// 需要用户处理的问题(如配对失效),没有时不写入 JSON
    public var hint: String?

    public init(connected: Bool, name: String, fw: String, updated: Double, hint: String? = nil) {
        self.connected = connected
        self.name = name
        self.fw = fw
        self.updated = updated
        self.hint = hint
    }

    public func jsonData() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(self)
    }
}
