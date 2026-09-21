# Realtek USB 10GbE Family Controller 高级设置只读报告

## 查询结论

目标适配器已确认：

- 适配器名称：以太网 3
- 接口描述：Realtek USB 10GbE Family Controller
- 状态：Up；管理状态：Up；媒体连接：Connected
- 当前链路速度：2.5 Gbps
- 高级属性总数：76 项
- 有显示名称的属性：38 项
- 无显示名称的驱动/注册表内部属性：38 项

本报告仅记录读取结果，未修改任何网卡设置。

## 采集范围与方法

- 采集时间：2026-09-14 13:07:14 +08:00
- 查询命令：Get-NetAdapterAdvancedProperty -Name '以太网 3' -IncludeHidden -AllProperties
- “当前显示值”来自驱动返回的 DisplayValue。
- “原始值”来自驱动返回的 RegistryValue。
- “驱动允许值”同时列出驱动返回的显示值和对应原始值；未返回的字段记为“—”。
- 本查询返回的 76 项属性中，DefaultValue 和 Type 均为空；Source 均为 3。

## 适配器身份信息

| 项目 | 读取结果 |
|---|---|
| 适配器名称 | 以太网 3 |
| 接口描述 | Realtek USB 10GbE Family Controller |
| 状态 | Up |
| 管理状态 | Up |
| 媒体连接状态 | Connected |
| Interface Index | 6 |
| MAC 地址 | 1C-86-0B-3C-AE-18 |
| 当前链路速度 | 2.5 Gbps |
| 驱动信息 | Driver Date 2025-04-10；Version 11.19.410.2025；NDIS 6.89 |
| 驱动文件 | rtucx22x64.sys |
| 驱动提供商 | Realtek |
| INF 文件 | oem282.inf |
| INF 节 | RTL815Ax64.ndi.NT |
| MatchingDeviceId | USB\VID_0BDA&PID_815A&REV_3000 |
| DeviceInstanceID | USB\VID_0BDA&PID_815A\00031C860B3CAE18 |
| ComponentId | USB\VID_0BDA&PID_815A&REV_3000 |
| NetCfgInstanceId | {38DD49D3-64FE-41D3-AC4C-4F3C8564221D} |

## 有显示名称的高级设置

| 原始序号 | 显示名称 | 当前显示值 | RegistryKeyword | 原始值 | 驱动允许值（显示值 [原始值]） |
|---:|---|---|---|---:|---|
| 22 | 连接速度和双工模式 | 自动侦测 | <code>*SpeedDuplex</code> | 0 | 自动侦测 [0]；10 Mbps 半双工 [1]；10 Mbps 全双工 [2]；100 Mbps 半双工 [3]；100 Mbps 全双工 [4]；1.0 Gbps 全双工 [6]；2.5 Gbps 全双工 [2500]；5.0 Gbps 全双工 [5000]；10.0 Gbps 全双工 [10000] |
| 23 | 魔术封包唤醒 | 关闭 | <code>*WakeOnMagicPacket</code> | 0 | 关闭 [0]；开启 [1] |
| 24 | 样式比对唤醒 | 开启 | <code>*WakeOnPattern</code> | 1 | 关闭 [0]；开启 [1] |
| 25 | 网络唤醒和关机连接速度 | 不降速 | <code>WolShutdownLinkSpeed</code> | 2 | 10 Mbps 优先 [0]；100 Mbps 优先 [1]；不降速 [2] |
| 26 | IPv4 硬件校验和 | Rx & Tx 开启 | <code>*IPChecksumOffloadIPv4</code> | 3 | 关闭 [0]；Tx 开启 [1]；Rx 开启 [2]；Rx & Tx 开启 [3] |
| 27 | TCP 硬件校验和 (IPv4) | Rx & Tx 开启 | <code>*TCPChecksumOffloadIPv4</code> | 3 | 关闭 [0]；Tx 开启 [1]；Rx 开启 [2]；Rx & Tx 开启 [3] |
| 28 | UDP 硬件校验和 (IPv4) | Rx & Tx 开启 | <code>*UDPChecksumOffloadIPv4</code> | 3 | 关闭 [0]；Tx 开启 [1]；Rx 开启 [2]；Rx & Tx 开启 [3] |
| 29 | TCP 硬件校验和 (IPv6) | Rx & Tx 开启 | <code>*TCPChecksumOffloadIPv6</code> | 3 | 关闭 [0]；Tx 开启 [1]；Rx 开启 [2]；Rx & Tx 开启 [3] |
| 30 | UDP 硬件校验和 (IPv6) | Rx & Tx 开启 | <code>*UDPChecksumOffloadIPv6</code> | 3 | 关闭 [0]；Tx 开启 [1]；Rx 开启 [2]；Rx & Tx 开启 [3] |
| 31 | 优先级和VLAN | 优先级和VLAN 开启 | <code>*PriorityVLANTag</code> | 3 | 优先级和VLAN 关闭 [0]；优先级开启 [1]；VLAN 开启 [2]；优先级和VLAN 开启 [3] |
| 32 | 巨型帧 | 关闭 | <code>*JumboPacket</code> | 1514 | 关闭 [1514]；4088 Bytes [4088]；9014 Bytes [9014]；16128 Bytes [16128] |
| 33 | 大量传送减负 v2 (IPv4) | 关闭 | <code>*LsoV2IPv4</code> | 0 | 关闭 [0]；开启 [1] |
| 34 | 大量传送减负 v2 (IPv6) | 关闭 | <code>*LsoV2IPv6</code> | 0 | 关闭 [0]；开启 [1] |
| 35 | Recv Segment Coalescing (IPv4) | 关闭 | <code>*RscIPv4</code> | 0 | 关闭 [0]；开启 [1] |
| 36 | Recv Segment Coalescing (IPv6) | 关闭 | <code>*RscIPv6</code> | 0 | 关闭 [0]；开启 [1] |
| 37 | ARP 减负 | 关闭 | <code>*PMARPOffload</code> | 0 | 关闭 [0]；开启 [1] |
| 38 | NS 减负 | 关闭 | <code>*PMNSOffload</code> | 0 | 关闭 [0]；开启 [1] |
| 39 | Wake on link change | 关闭 | <code>WakeOnLinkChange</code> | 0 | 关闭 [0]；开启 [1] |
| 40 | Transmit URBs | 64 | <code>PendingTransmits</code> | 64 | — |
| 41 | 传送缓冲区 | 256 | <code>TransmitBufferLen</code> | 256 | — |
| 42 | Receive URBs | 64 | <code>PendingReceives</code> | 64 | — |
| 43 | 接收缓冲区 | 256 | <code>ReceiveBufferLen</code> | 256 | — |
| 56 | 流控制 | 关闭 | <code>*FlowControl</code> | 0 | 关闭 [0]；Rx & Tx 开启 [3] |
| 57 | 节能乙太网路 | 关闭 | <code>*EEE</code> | 0 | 关闭 [0]；开启 [1] |
| 58 | EEE Max Support Speed | 10.0 Gbps 全双工 | <code>EEEMaxSupportSpeed</code> | 10000 | 10 Mbps 全双工 [10]；100 Mbps 全双工 [100]；1.0 Gbps 全双工 [1000]；2.5 Gbps 全双工 [2500]；5.0 Gbps 全双工 [5000]；10.0 Gbps 全双工 [10000] |
| 59 | 环保节能 | 关闭 | <code>EnableGreenEthernet</code> | 0 | 关闭 [0]；开启 [1] |
| 60 | Advanced EEE | 关闭 | <code>AdvancedEEE</code> | 0 | 关闭 [0]；开启 [1] |
| 61 | Idle Power Saving | 关闭 | <code>EnableExtraPowerSaving</code> | 0 | 关闭 [0]；开启 [1] |
| 62 | Miscellaneous Transfer Settings | 开启 | <code>EnableExtraTransmissionParm</code> | 1 | 关闭 [0]；开启 [1] |
| 63 | Gigabit Lite | 关闭 | <code>GigaLite</code> | 0 | 关闭 [0]；开启 [1] |
| 64 | 接收端调整 | 关闭 | <code>*RSS</code> | 0 | 关闭 [0]；开启 [1] |
| 66 | 接收端调整最大伫列 | 4个伫列 | <code>*NumRssQueues</code> | 4 | 1个伫列 [1]；2个伫列 [2]；4个伫列 [4] |
| 67 | Mask WakeUp Event Timer | 0 second | <code>MaskTimer</code> | 0 | 0 second [0]；4 seconds [1]；8 seconds [2]；12 seconds [3]；16 seconds [4]；20 seconds [5]；24 seconds [6]；28 seconds [7]；32 seconds [8] |
| 69 | Battery Mode Link Speed | 不降速 | <code>BatteryModeLinkSpeed</code> | 0 | 不降速 [0]；10 Mbps 优先 [1]；100 Mbps 优先 [2] |
| 70 | Adaptive Link Speed | 关闭 | <code>EnableAdaptiveLinkCap</code> | 0 | 关闭 [0]；开启 [1] |
| 71 | idle power down restriction | No Restriction | <code>*IdleRestriction</code> | 0 | No Restriction [0]；Only idle when user is not present [1] |
| 75 | 网络地址 | — | <code>NetworkAddress</code> | — | — |
| 76 | VLAN ID | — | <code>RegVlanID</code> | — | — |

## 无显示名称的驱动/注册表内部属性

以下属性由 AllProperties 查询返回，但驱动没有提供 DisplayName 或 DisplayValue。它们仍按原始查询结果完整保留。

| 原始序号 | RegistryKeyword | 原始值 | DisplayName / DisplayValue |
|---:|---|---:|---|
| 1 | <code>DriverDesc</code> | Realtek USB 10GbE Family Controller | — |
| 2 | <code>ProviderName</code> | Realtek | — |
| 3 | <code>DriverDate</code> | 4-10-2025 | — |
| 4 | <code>DriverVersion</code> | 11.19.410.2025 | — |
| 5 | <code>InfPath</code> | oem282.inf | — |
| 6 | <code>InfSection</code> | RTL815Ax64.ndi.NT | — |
| 7 | <code>MatchingDeviceId</code> | USB\VID_0BDA&PID_815A&REV_3000 | — |
| 8 | <code>HwOption</code> | 0 | — |
| 9 | <code>HwOptionV2</code> | 0 | — |
| 10 | <code>HwOptionV3</code> | 0 | — |
| 11 | <code>HwOptionV4</code> | 0 | — |
| 12 | <code>SwOption</code> | 0 | — |
| 13 | <code>SwOptionV2</code> | 0 | — |
| 14 | <code>SwOptionV3</code> | 0 | — |
| 15 | <code>SwOptionV4</code> | 0 | — |
| 16 | <code>HwOptimize</code> | 0 | — |
| 17 | <code>HwFlags</code> | 0 | — |
| 18 | <code>HwMode</code> | 0 | — |
| 19 | <code>ICMask</code> | 0 | — |
| 20 | <code>MonitorModeEnabled</code> | 0 | — |
| 21 | <code>EnableTestIO</code> | 0 | — |
| 44 | <code>*IfType</code> | 6 | — |
| 45 | <code>*MediaType</code> | 0 | — |
| 46 | <code>*PhysicalMediaType</code> | 14 | — |
| 47 | <code>BusType</code> | 15 | — |
| 48 | <code>Characteristics</code> | 132 | — |
| 49 | <code>IfTypePreStart</code> | 6 | — |
| 50 | <code>NetworkInterfaceInstallTimestamp</code> | 134302183573169619 | — |
| 51 | <code>DeviceInstanceID</code> | USB\VID_0BDA&PID_815A\00031C860B3CAE18 | — |
| 52 | <code>ComponentId</code> | USB\VID_0BDA&PID_815A&REV_3000 | — |
| 53 | <code>NetCfgInstanceId</code> | {38DD49D3-64FE-41D3-AC4C-4F3C8564221D} | — |
| 54 | <code>NetLuidIndex</code> | 32775 | — |
| 55 | <code>RtHwCapability</code> | 8 | — |
| 65 | <code>*RSSProfile</code> | 4 | — |
| 68 | <code>BulkOutBurstMode</code> | 0 | — |
| 72 | <code>S5WakeOnLan</code> | 0 | — |
| 73 | <code>HwVer</code> | 150994963 | — |
| 74 | <code>StatHotRST</code> | 3 | — |

## 原始字段备注

- 76 项属性的 Source 均为 3。
- 76 项属性的 DefaultValue 均未由驱动返回。
- 76 项属性的 Type 均未由驱动返回。
- NetworkAddress 和 RegVlanID 的当前 DisplayValue 与 RegistryValue 均为空，报告中记为“—”。
- 本报告覆盖网卡属性页 Advanced 相关的驱动高级属性；Windows 单独的 Power Management 页面不属于本次 AdvancedProperty 查询范围。
