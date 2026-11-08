// BridgeCore 单元测试:通知分片拼行、HELLO 识别、转发过滤与状态文件内容。
import XCTest
@testable import BridgeCore

final class LineAssemblerTests: XCTestCase {
    func testJoinsChunksAndSplitsOnAnyTerminator() {
        var a = LineAssembler()
        XCTAssertEqual(a.push(Data("AR1 HEL".utf8)), [])
        XCTAssertEqual(a.push(Data("LO fw=0.2.0 proto=1\r\nI (1) x\r".utf8)),
                       ["AR1 HELLO fw=0.2.0 proto=1", "I (1) x"])
        XCTAssertEqual(a.push(Data("\nAR1 PING\n".utf8)), ["AR1 PING"])
    }

    func testOverlongLineIsDropped() {
        var a = LineAssembler()
        XCTAssertEqual(a.push(Data(String(repeating: "x", count: 300).utf8)), [])
        XCTAssertEqual(a.push(Data("\nAR1 PING\n".utf8)), ["AR1 PING"])
    }
}

final class ProtocolTests: XCTestCase {
    func testParseHello() {
        XCTAssertEqual(parseHelloFirmware("AR1 HELLO fw=0.2.0 proto=1"), "0.2.0")
        XCTAssertNil(parseHelloFirmware("AR1 S seq=1"))
        XCTAssertNil(parseHelloFirmware("I (1) AR1 HELLO fw=1"))
    }

    func testOnlyProtocolLinesAreForwarded() {
        XCTAssertTrue(isForwardable(Data("AR1 S seq=1 cc=0 cx=0 ds=0 done=0 wn=0 w=-\n".utf8)))
        XCTAssertTrue(isForwardable(Data("AR1 PING\n".utf8)))
        XCTAssertFalse(isForwardable(Data("AR1 PING".utf8)))             // 缺行尾
        XCTAssertFalse(isForwardable(Data("hello\n".utf8)))               // 不是协议行
        XCTAssertFalse(isForwardable(Data(("AR1 " + String(repeating: "x", count: 200) + "\n").utf8)))
    }
}

final class StatusTests: XCTestCase {
    func testStatusJSONFields() throws {
        let status = BridgeStatus(connected: true, name: "AgentRun-XXXX", fw: "0.2.0", updated: 1000.5)
        let object = try JSONSerialization.jsonObject(with: status.jsonData()) as? [String: Any]
        XCTAssertEqual(object?["connected"] as? Bool, true)
        XCTAssertEqual(object?["name"] as? String, "AgentRun-XXXX")
        XCTAssertEqual(object?["fw"] as? String, "0.2.0")
        XCTAssertEqual(object?["updated"] as? Double, 1000.5)
        XCTAssertNil(object?["hint"])
    }

    func testStatusCarriesHintForRemovedPairing() throws {
        // 设备重新烧录后配对信息被清除,macOS 拒绝连接:需要提示用户在系统设置里移除设备
        let hint = hintForConnectError(code: 14, deviceName: "AgentRun-XXXX")
        XCTAssertNotNil(hint)
        XCTAssertTrue(hint!.contains("AgentRun-XXXX"))
        XCTAssertNil(hintForConnectError(code: 6, deviceName: "AgentRun-XXXX"))
        let status = BridgeStatus(connected: false, name: "AgentRun-XXXX", fw: "", updated: 1, hint: hint)
        let object = try JSONSerialization.jsonObject(with: status.jsonData()) as? [String: Any]
        XCTAssertEqual(object?["hint"] as? String, hint)
    }
}
