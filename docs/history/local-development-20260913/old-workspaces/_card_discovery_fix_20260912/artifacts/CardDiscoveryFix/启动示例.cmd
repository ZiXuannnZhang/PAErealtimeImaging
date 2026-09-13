@echo off
rem CONFIG-ACK 动态发现修复版接收程序启动示例
rem 不使用 --target-ips 时走动态发现；需要显式目标时去掉下一行注释并按需修改 IP。
rem PAimageReceiverDiagnostics.exe --target-ips=192.168.0.2,192.168.0.3,192.168.0.4,192.168.0.5
PAimageReceiverDiagnostics.exe
