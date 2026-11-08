// AgentRunBridge/main.swift —— 蓝牙桥接程序(无线方案 §5.1)。
//
// - 绑定 $AGENT_RUN_HOME/run/ble.sock(Unix 数据报),接收守护进程的状态行并经蓝牙写给设备;
// - 连接附近的 AgentRun-* 设备(记住第一次连上的那台,之后优先重连它);
// - 设备回 HELLO(表示已确认这台电脑)后才算就绪;每 2 秒把状态写入 run/ble-status.json。
// 以后台应用(LSUIElement)运行,Info.plist 声明蓝牙用途,首次运行时由 macOS 请求蓝牙权限。
import BridgeCore
import CoreBluetooth
import Darwin
import Foundation

let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let rxUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
let txUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

let home = ProcessInfo.processInfo.environment["AGENT_RUN_HOME"] ?? (NSHomeDirectory() + "/.agent-run")
let runDir = home + "/run"
let socketPath = runDir + "/ble.sock"
let statusPath = runDir + "/ble-status.json"
let knownPeripheralKey = "knownPeripheral"

/// 日志写到 stderr,由 launchd 重定向到 log/bridge.log
func log(_ message: String) {
    let line = "\(ISO8601DateFormatter().string(from: Date())) \(message)\n"
    FileHandle.standardError.write(Data(line.utf8))
}

final class Bridge: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var assembler = LineAssembler()
    private var ready = false              // 设备已回 HELLO
    private var firmware = ""
    private var socketFD: Int32 = -1
    private var readSource: DispatchSourceRead?
    private var statusTimer: Timer?
    private var pingTimer: Timer?
    private var lastConnectAt = Date.distantPast
    private var quickDisconnects = 0       // 连上不久就断开的次数
    private var hint: String?              // 需要用户处理的问题,写进状态文件

    func start() {
        try? FileManager.default.createDirectory(atPath: runDir, withIntermediateDirectories: true)
        openSocket()
        central = CBCentralManager(delegate: self, queue: .main)
        statusTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.writeStatus()
        }
        writeStatus()
    }

    // MARK: - 本机 socket

    /// 绑定数据报 socket;上次异常退出留下的旧文件先删除
    private func openSocket() {
        unlink(socketPath)
        socketFD = socket(AF_UNIX, SOCK_DGRAM, 0)
        guard socketFD >= 0 else {
            log("创建 socket 失败 errno=\(errno)")
            return
        }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(socketPath.utf8CString)
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in
            for i in 0..<min(raw.count - 1, pathBytes.count) {
                raw[i] = UInt8(bitPattern: pathBytes[i])
            }
        }
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(socketFD, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard rc == 0 else {
            log("绑定 \(socketPath) 失败 errno=\(errno)")
            return
        }
        _ = fcntl(socketFD, F_SETFL, O_NONBLOCK)
        let source = DispatchSource.makeReadSource(fileDescriptor: socketFD, queue: .main)
        source.setEventHandler { [weak self] in self?.drainSocket() }
        source.resume()
        readSource = source
    }

    /// 读出所有排队的数据报;设备就绪时只转发合法的协议行
    private func drainSocket() {
        var buffer = [UInt8](repeating: 0, count: 512)
        while true {
            let n = recv(socketFD, &buffer, buffer.count, 0)
            if n <= 0 { break }
            let data = Data(buffer[0..<n])
            if ready && isForwardable(data) { write(data) }
        }
    }

    /// 带响应写入:保证顺序,失败时有回调可记录
    private func write(_ data: Data) {
        guard let p = peripheral, let rx = rx else { return }
        p.writeValue(data, for: rx, type: .withResponse)
    }

    // MARK: - 状态文件

    private func writeStatus() {
        let status = BridgeStatus(connected: ready, name: peripheral?.name ?? "", fw: firmware,
                                  updated: Date().timeIntervalSince1970, hint: hint)
        guard let data = try? status.jsonData() else { return }
        let tmp = statusPath + ".tmp"
        if FileManager.default.createFile(atPath: tmp, contents: data) {
            rename(tmp, statusPath)   // 原子替换,守护进程不会读到半截内容
        }
    }

    private func markNotReady() {
        pingTimer?.invalidate()
        if ready {
            ready = false
            writeStatus()
        }
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        log("蓝牙状态 \(c.state.rawValue),授权 \(CBManager.authorization.rawValue)")
        guard c.state == .poweredOn else {
            markNotReady()
            return
        }
        connectKnownOrScan()
    }

    /// 有记住的设备就直接连接(不在范围内时请求会挂起,设备出现后自动连上),否则扫描
    private func connectKnownOrScan() {
        if let idString = UserDefaults.standard.string(forKey: knownPeripheralKey),
           let id = UUID(uuidString: idString),
           let known = central.retrievePeripherals(withIdentifiers: [id]).first {
            adopt(known)
            central.connect(known, options: nil)
            log("连接已记住的设备 \(known.name ?? idString)")
        } else {
            central.scanForPeripherals(withServices: [serviceUUID], options: nil)
            log("扫描 AgentRun 设备")
        }
    }

    private func adopt(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData ad: [String: Any], rssi: NSNumber) {
        let name = p.name ?? (ad[CBAdvertisementDataLocalNameKey] as? String) ?? ""
        guard name.hasPrefix("AgentRun-") else { return }
        c.stopScan()
        adopt(p)
        UserDefaults.standard.set(p.identifier.uuidString, forKey: knownPeripheralKey)
        c.connect(p, options: nil)
        log("发现 \(name),信号 \(rssi),开始连接")
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        lastConnectAt = Date()
        assembler = LineAssembler()
        log("已连接 \(p.name ?? "")")
        p.discoverServices([serviceUUID])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log("连接失败:\(error?.localizedDescription ?? "未知")")
        if let code = (error as NSError?)?.code, let text = hintForConnectError(code: code, deviceName: p.name ?? "") {
            if hint != text { log(text) }
            hint = text
            writeStatus()
        }
        scheduleReconnect(after: 5)
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        markNotReady()
        rx = nil
        // 连上不到 10 秒就断开,多半是设备拒绝了配对或确认超时:连续 3 次后退避 60 秒,避免反复打扰
        quickDisconnects = Date().timeIntervalSince(lastConnectAt) < 10 ? quickDisconnects + 1 : 0
        let delay = quickDisconnects >= 3 ? 60.0 : 2.0
        log("已断开(\(error?.localizedDescription ?? "正常断开")),\(Int(delay)) 秒后重连")
        scheduleReconnect(after: delay)
    }

    private func scheduleReconnect(after seconds: Double) {
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { [weak self] in
            guard let self = self, self.central.state == .poweredOn else { return }
            if let p = self.peripheral {
                self.central.connect(p, options: nil)
            } else {
                self.connectKnownOrScan()
            }
        }
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = p.services?.first(where: { $0.uuid == serviceUUID }) else {
            log("设备上没有 Agent Run 服务,断开")
            central.cancelPeripheralConnection(p)
            return
        }
        p.discoverCharacteristics([rxUUID, txUUID], for: service)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for ch in service.characteristics ?? [] {
            if ch.uuid == txUUID { p.setNotifyValue(true, for: ch) }
            if ch.uuid == rxUUID { rx = ch }
        }
        // 设备确认这台电脑之前会丢弃数据、不回 HELLO:每 3 秒发一次 PING,直到收到 HELLO
        pingTimer?.invalidate()
        pingTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            guard let self = self, !self.ready else { return }
            self.write(Data("AR1 PING\n".utf8))
        }
        write(Data("AR1 PING\n".utf8))
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        guard let value = ch.value else { return }
        for line in assembler.push(value) {
            guard let fw = parseHelloFirmware(line) else { continue }
            firmware = fw
            if !ready {
                ready = true
                quickDisconnects = 0
                hint = nil
                pingTimer?.invalidate()
                log("设备已就绪,固件 \(fw)")
                writeStatus()
            }
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let e = error { log("写入失败:\(e.localizedDescription)") }
    }
}

let bridge = Bridge()
bridge.start()
RunLoop.main.run()
