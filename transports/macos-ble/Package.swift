// swift-tools-version:5.9
// AgentRunBridge:把 agent-run 守护进程的状态行经蓝牙转发给 AI Passport(无线方案 §5.1)。
import PackageDescription

let package = Package(
    name: "AgentRunBridge",
    platforms: [.macOS(.v13)],
    targets: [
        // 纯逻辑(拼行、识别 HELLO、转发过滤、状态文件),可单元测试
        .target(name: "BridgeCore"),
        // 应用入口:CoreBluetooth 与本机 socket
        .executableTarget(
            name: "AgentRunBridge",
            dependencies: ["BridgeCore"],
            linkerSettings: [.linkedFramework("CoreBluetooth")]
        ),
        .testTarget(name: "BridgeCoreTests", dependencies: ["BridgeCore"]),
    ]
)
