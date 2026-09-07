#pragma once

// ============================================================
// Constants.h  全局常量定义
// MC_410T 多卡上位机软件
// ============================================================

//  硬件规格 
constexpr int    MAX_CARDS            = 64;
constexpr int    CARDS_PER_BOX        = 4;

//  UDP 数据包格式 
constexpr int    UDP_HEADER_BYTES     = 4;
constexpr int    UDP_PAYLOAD_BYTES    = 1440;
constexpr int    UDP_TOTAL_BYTES      = UDP_HEADER_BYTES + UDP_PAYLOAD_BYTES;

//  包重组缓冲区 
constexpr int    MAX_PKTS_PER_TRIG    = 300;   // 最大包数/触发（16bit@125M=70包，32bit@250M≈278包，留余量）

//  端口范围 
constexpr int    BASE_PORT            = 8001;
constexpr int    MAX_PORT             = BASE_PORT + MAX_CARDS - 1;

//  对象池大小
constexpr int    PACKET_POOL_SIZE     = 65536;

//  采样与时钟 
constexpr double FPGA_ADC_FREQ_HZ     = 250e6;
constexpr double SAMPLE_FREQ_HZ       = 125e6;
constexpr double FPGA_ADC_INTERVAL_NS = 4.0;
constexpr double SAMPLE_INTERVAL_NS   = 8.0;

//  差分相位/频率转换系数
// freq_kHz = delta_phi_int16 / 32768 * (250e6 / 2pi) / 1000 = delta_phi * 1.21468
constexpr double FREQ_SCALE_KHZ       = 1.21468;

//  线程模型参数 
constexpr int    PROC_BATCH_SIZE      = 512;
constexpr int    PROC_WAKE_TIMEOUT_MS = 1;
constexpr int    STATS_UPDATE_MS      = 1000;
constexpr int    DISPLAY_REFRESH_MS   = 33;

//  存储参数 
constexpr int    DEFAULT_TRIGGERS_PER_FILE = 1000;
constexpr int    FILESAVER_QUEUE_SIZE      = 5000;

//  与旧项目 Constants.h 兼容的别名 / 补充常量
constexpr double DIFF_SAMPLE_RATE_HZ  = FPGA_ADC_FREQ_HZ;   // 差分在250MHz时钟域完成
constexpr int    BYTES_PER_SAMPLE     = 2;
constexpr int    BYTES_PER_SAMPLE_PAIR = BYTES_PER_SAMPLE * 2;
constexpr int    CARDS_PER_GROUP      = CARDS_PER_BOX;       // 每组4张卡
constexpr int    NUM_GROUPS           = MAX_CARDS / CARDS_PER_GROUP;
constexpr int    ACTIVE_CARDS         = 4;
constexpr int    CHANNELS_PER_CARD    = 2;
constexpr int    CONTROL_PORT         = 8080;
constexpr int    FEEDBACK_PORT        = 8000;
constexpr int    DATA_PORT_BASE       = BASE_PORT;
constexpr int    MAX_UDP_PAYLOAD      = UDP_PAYLOAD_BYTES;

//  配置确认（60 字节反馈）状态机参数
constexpr int    CONFIG_ACK_TICK_MS    = 200;    // 配置确认轮询周期（ms）
constexpr int    CONFIG_ACK_TIMEOUT_MS = 1000;   // 单卡等待 60 字节反馈超时（ms）
constexpr int    CONFIG_ACK_MAX_RETRY  = 3;      // 每卡最大重发次数（超时未确认时）

//  网段扫描自动识别（采集卡 IP 固定从 base 起递增，最多 32 张）
constexpr char   DEFAULT_SCAN_BASE_IP[] = "192.168.0.2";  // 默认扫描起始 IP
constexpr int    DEFAULT_SCAN_IP_COUNT  = 32;               // 默认扫描 IP 数量
constexpr int    SCAN_ICMP_TIMEOUT_MS   = 150;              // 扫描 ICMP 探测超时（ms）