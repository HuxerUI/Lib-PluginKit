// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Lib-PluginKit",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "PluginKit",
            targets: ["PluginKit"]
        ),
    ],
    targets: [
        .target(name: "PluginKit"),
    ]
)
