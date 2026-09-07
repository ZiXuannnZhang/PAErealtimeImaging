#ifndef CE3A5F30_2F7C_4FFE_989C_2AD5A098E5A1
#define CE3A5F30_2F7C_4FFE_989C_2AD5A098E5A1

#include "pa_recon.h"

#include <QVector>
#include <QString>

#include <limits>
#include <stdexcept>
#include <utility>

namespace pa_recon_qt
{
    // Qt 封装配置。
    // 约定：
    // 1) 坐标单位为 m，采样率单位为 Hz。
    // 2) move_aline 为单帧输入脉冲数，channel_aline 为每通道参与重建的脉冲数。
    // 3) QVector 字段留空表示不启用对应参数。
    struct Config
    {
        enum class TimeWeightMode
        {
            None = 0,        // 关闭
            PMinusTdp = 1,   // p - t*dp
            TwoPMinusTdp = 2 // 2p - t*dp
        };

        enum class LookDirection
        {
            Up = 0,
            Down = 1
        };

        enum class InterpMode
        {
            Linear = 0, // 线性插值
            Nearest = 1 // 最近邻插值
        };

        enum class OutputType
        {
            RF = 0,  // 原始叠加结果
            ENV = 1, // 包络幅值
            DB = 2   // 对数压缩显示
        };

        enum class FilterType
        {
            None = 0,     // 关闭
            Lowpass = 1,  // 低通滤波
            Highpass = 2, // 高通滤波
            Bandpass = 3  // 带通滤波
        };

        enum class FiberType
        {
            F8 = 0, // 单脉冲内只有一条光纤
            F64 = 1 // 单脉冲内有8条光纤
        };

        // ---- 输入数据与采集参数 ----
        int depth = 25000;                     // 单采集通道采样点数（通常 floor(sampletime_s * daq_hz)）
        int channel_num = 64;                  // 总光纤通道数
        int card_num = 8;                      // 总采集卡通道数
        int move_aline = 100;                  // 单帧电机移动脉冲数（输入帧包含的总脉冲数，对应 MATLAB move_Aline）
        int channel_aline = 50;                // 每通道参与重建的脉冲数（对应 MATLAB splice 列数）
        FiberType fiber_type = FiberType::F64; // 光纤复用模式：F64=卡内多光纤；F8=单脉冲单光纤
        int delay = 141;                       // 光纤自激发信号起始位置（采样点）
        float threhold = 1000.0f;              // 预处理门限，单位采样值（对应 MATLAB 新版 threhold=1000）

        // ---- 成像坐标与采样参数 ----
        float fs = 125e6f; // 采样率
        float x0 = -6e-3f; // 成像网格原点 x 坐标（米）
        float y0 = 4e-3f;  // 成像网格原点 y 坐标（米）
        float dx = 10e-6f; // 成像网格 x 方向像素间距（米）
        float dy = 10e-6f; // 成像网格 y 方向像素间距（米）
        int nx = 1200;     // 成像网格 x 方向像素数
        int ny = 800;      // 成像网格 y 方向像素数
        // 阵元参数（detx0 在运行时按通道索引计算，dety 固定为 0）
        float detx_step = 10e-6f; // 阵元 x 方向步长（米）

        // ---- 声速模型参数 ----
        bool use_dual = false;       // 是否启用双声速
        float boundary_y = -1e-6f;   // 双声速边界位置（米），通常取小于0的值以确保包含所有阵元
        float lower_sos = 1500.0f;   // 较低声速值（米/秒），通常对应水或软组织
        float upper_sos = 1560.0f;   // 较高声速值（米/秒），通常对应血管或较硬组织
        bool is_center_align = true; // 是否启用中心对齐后的边界清零逻辑

        // ---- 拼接与通道校准参数 ----
        QVector<int> x_peizhun{14, 9, -2, 12, 9, 0, -2, 0, -4, -4, -4, -5, 19, 15, 24, 2,
                               5, 0, 7, 7, 7, 5, 6, 0, 6, 3, 1, -1, -2, -10, 8, 0,
                               -5, 9, -7, -5, -3, -8, -13, -2, -12, -5, -10, -6, -9, -5, -5, -11,
                               0, -13, -14, 45, 50, -4, 6, 11, 9, 1, 5, 9, 4, 2, 7, 1}; // x 方向通道位置校准，单位采样点（对应 MATLAB x_peizhun）
        QVector<int> y_peizhun{-10, -16, -5, -15, -2, 0, -2, -10, -3, -12, -8, -8, -4, -8, -16, -9,
                               -19, -14, -18, -13, -14, 0, -12, -16, -17, -20, -4, -18, -5, -16, 4, 0,
                               -10, -14, -14, -5, -15, -4, -15, -15, -18, 0, -9, -11, -13, -13, -10, -13,
                               -5, -4, -15, -13, -3, -15, -10, -19, -4, -12, -5, -5, -15, -15, -17, -17}; // y 方向通道位置校准，单位采样点（对应 MATLAB y_peizhun）
        QVector<int> CaliFiberDelay{534, 484, 428, 372, 322, 264, 282, 202};                              // 光纤标定时延，单位采样点（对应 MATLAB CaliFiberDelay）
        QVector<int> CaliCardDelay{0, 2, 2, 2, 4, 6, 6, 8};                                               // 采集卡标定时延，单位采样点（对应 MATLAB CaliCardDelay）
        int delay_time_point = 1601;                                                                      // 时延补偿点，单位采样点（通常为光纤自激发信号位置）会加到表里
        int s1period = 2500;                                                                              // S1 周期，单位采样点（通常为单脉冲采样点数）

        // ---- 预处理参数 ----
        QVector<float> apod{};                     // 阵元加权窗，长度通常为 nd；为空表示全 1
        QVector<float> bandpass_hz{};              // 带通滤波截止频率，长度为 2；为空表示不启用带通滤波
        FilterType filter_type = FilterType::None; // IIR 设计类型
        QVector<double> filter_cut{};              // IIR 截止频率，单位 Hz：low/high 为 1 项，band 为 2 项；为空表示不启用 IIR 滤波
        int filter_order = 0;                      // IIR 滤波器阶数，通常为 2 或 4

        // ---- 算法模式参数 ----
        TimeWeightMode time_weight = TimeWeightMode::None; // 时间加权模式
        LookDirection look_dir = LookDirection::Up;        // 阵元法向方向
        InterpMode interp_mode = InterpMode::Linear;       // 插值模式

        bool remove_dc = false;                                       // 是否启用 DC 分量移除（对应 MATLAB remove_dc）
        float ang_sigma_deg = std::numeric_limits<float>::infinity(); // 角度加权高斯函数标准差，单位度；默认为无穷大表示不启用角度加权（对应 MATLAB ang_sigma_deg）
        float ang_mu_deg = 0.0f;                                      // 角度加权高斯函数均值，单位度；通常为 0，表示以 look_dir 定义的方向为中心（对应 MATLAB ang_mu_deg）
        float dyn_range_db = 50.0f;                                   // 动态范围，单位 dB；通常为 50（对应 MATLAB dyn_range_db）

        // ---- 输出参数 ----
        OutputType output_type = OutputType::RF; // 输出类型：RF/ENV/DB（对应 MATLAB output_type）
        bool norm_by_w = false;                  // 是否启用 w 权重归一化（对应 MATLAB norm_by_w）
        bool is_read_fiber_position = true;      // 是否启用运行时读取光纤位置的逻辑（对应 MATLAB is_read_fiber_position）
        bool has_ang_sigma_deg = true;           // 是否启用 ang_sigma_deg 参数（对应 MATLAB ang_sigma_deg），如果为 false 则忽略 ang_sigma_deg 的值并在底层关闭角度加权
        bool is_cutoff_loc = false;              // 是否对末尾干扰段进行衰减处理（对应 C 接口 is_cutoff_loc）
        int cutoff_point = 2501;                 // 衰减起始采样点（对应 C 接口 cutoff_point）
    };

    class PAReconstructor
    {
    public:
        explicit PAReconstructor(const Config &config)
            : config_(config)
        {
            pa_recon_default_config(&c_config_);
            sync_c_config();
            handle_ = pa_recon_create(&c_config_);
            if (handle_ == nullptr)
                throw std::runtime_error(pa_recon_last_error());
        }

        ~PAReconstructor()
        {
            if (handle_ != nullptr)
                pa_recon_destroy(handle_);
        }

        PAReconstructor(const PAReconstructor &) = delete;
        PAReconstructor &operator=(const PAReconstructor &) = delete;

        PAReconstructor(PAReconstructor &&other) noexcept
            : config_(std::move(other.config_)), c_config_(other.c_config_), handle_(other.handle_)
        {
            other.handle_ = nullptr;
            sync_c_config();
        }

        PAReconstructor &operator=(PAReconstructor &&other) noexcept
        {
            if (this != &other)
            {
                if (handle_ != nullptr)
                    pa_recon_destroy(handle_);
                config_ = std::move(other.config_);
                c_config_ = other.c_config_;
                handle_ = other.handle_;
                other.handle_ = nullptr;
                sync_c_config();
            }
            return *this;
        }

        // 送入单脉冲数据（float）。
        // data.size() 应等于 single_pulse_size()。
        int single_pulse_data(QVector<float> &data)
        {
            return single_pulse_data(static_cast<const QVector<float> &>(data));
        }

        // const 重载，语义与上面一致。
        int single_pulse_data(const QVector<float> &data)
        {
            if (handle_ == nullptr)
                return -1;
            return pa_recon_single_pulse(handle_, data.isEmpty() ? nullptr : data.constData(), static_cast<size_t>(data.size()));
        }

        // 获取一帧输出；无输出时返回 0 且 output 可能为空。
        int get_output(QVector<float> &output)
        {
            if (handle_ == nullptr)
            {
                output.clear();
                return -1;
            }

            output.resize(static_cast<int>(output_size()));
            size_t written_len = 0;
            int status = pa_recon_get_output(handle_, output.isEmpty() ? nullptr : output.data(), static_cast<size_t>(output.size()), &written_len);
            if (status != 0)
            {
                output.clear();
                return status;
            }

            output.resize(static_cast<int>(written_len));
            return 0;
        }

        // 查询单脉冲输入长度（float 元素数）。
        size_t single_pulse_size() const
        {
            return (handle_ != nullptr) ? pa_recon_get_single_pulse_size(handle_) : 0;
        }

        // 查询单帧输出长度（float 元素数，通常为 nx*ny）。
        size_t output_size() const
        {
            return (handle_ != nullptr) ? pa_recon_get_output_size(handle_) : static_cast<size_t>(config_.nx) * static_cast<size_t>(config_.ny);
        }

        // 返回底层 C 接口最近一次错误信息。
        QString last_error() const
        {
            return QString::fromLocal8Bit(pa_recon_last_error());
        }

        const Config &config() const
        {
            return config_;
        }

    private:
        // 将 Qt Config 映射到 C 接口配置结构。
        void sync_c_config()
        {
            c_config_.nx = config_.nx;
            c_config_.ny = config_.ny;
            c_config_.depth = config_.depth;
            c_config_.channel_num = config_.channel_num;
            c_config_.card_num = config_.card_num;
            c_config_.move_aline = config_.move_aline;
            c_config_.channel_aline = config_.channel_aline;
            c_config_.fiber_type = static_cast<PAReconFiberType>(config_.fiber_type);
            c_config_.delay = config_.delay;
            c_config_.threhold = config_.threhold;
            c_config_.fs = config_.fs;
            c_config_.x0 = config_.x0;
            c_config_.y0 = config_.y0;
            c_config_.dx = config_.dx;
            c_config_.dy = config_.dy;
            c_config_.detx_step = config_.detx_step;
            c_config_.use_dual = config_.use_dual ? 1 : 0;
            c_config_.boundary_y = config_.boundary_y;
            // C 库无条件要求 lower_sos > 0，upper_sos > 0（双声速时）
            c_config_.lower_sos = (config_.lower_sos > 0) ? config_.lower_sos : 1500.0f;
            c_config_.upper_sos = (config_.upper_sos > 0) ? config_.upper_sos : c_config_.lower_sos;

            c_config_.is_center_align = config_.is_center_align ? 1 : 0;
            c_config_.x_peizhun = config_.x_peizhun.isEmpty() ? nullptr : config_.x_peizhun.constData();
            c_config_.x_peizhun_len = static_cast<size_t>(config_.x_peizhun.size());
            c_config_.y_peizhun = config_.y_peizhun.isEmpty() ? nullptr : config_.y_peizhun.constData();
            c_config_.y_peizhun_len = static_cast<size_t>(config_.y_peizhun.size());
            c_config_.CaliFiberDelay = config_.CaliFiberDelay.isEmpty() ? nullptr : config_.CaliFiberDelay.constData();
            c_config_.CaliFiberDelay_len = static_cast<size_t>(config_.CaliFiberDelay.size());
            c_config_.CaliCardDelay = config_.CaliCardDelay.isEmpty() ? nullptr : config_.CaliCardDelay.constData();
            c_config_.CaliCardDelay_len = static_cast<size_t>(config_.CaliCardDelay.size());
            c_config_.delay_time_point = config_.delay_time_point;
            c_config_.s1period = config_.s1period;
            c_config_.apod = config_.apod.isEmpty() ? nullptr : config_.apod.constData();
            c_config_.apod_len = static_cast<size_t>(config_.apod.size());
            c_config_.bandpass_hz = config_.bandpass_hz.isEmpty() ? nullptr : config_.bandpass_hz.constData();
            c_config_.bandpass_hz_len = static_cast<size_t>(config_.bandpass_hz.size());
            c_config_.filter_type = static_cast<PAReconFilterType>(config_.filter_type);
            c_config_.filter_cut = config_.filter_cut.isEmpty() ? nullptr : config_.filter_cut.constData();
            c_config_.filter_cut_len = static_cast<size_t>(config_.filter_cut.size());
            c_config_.filter_order = config_.filter_order;
            c_config_.time_weight = static_cast<PAReconTimeWeightMode>(config_.time_weight);
            c_config_.look_dir = static_cast<PAReconLookDirection>(config_.look_dir);
            c_config_.interp_mode = static_cast<PAReconInterpMode>(config_.interp_mode);
            c_config_.remove_dc = config_.remove_dc ? 1 : 0;
            c_config_.ang_sigma_deg = config_.has_ang_sigma_deg ? config_.ang_sigma_deg : std::numeric_limits<float>::infinity();
            c_config_.ang_mu_deg = config_.ang_mu_deg;
            c_config_.dyn_range_db = config_.dyn_range_db;
            c_config_.output_type = static_cast<PAReconOutputType>(config_.output_type);
            c_config_.norm_by_w = config_.norm_by_w ? 1 : 0;
            c_config_.is_read_fiber_position = config_.is_read_fiber_position ? 1 : 0;
            c_config_.is_cutoff_loc = config_.is_cutoff_loc ? 1 : 0;
            c_config_.cutoff_point = config_.cutoff_point;
        }

        Config config_{};
        PAReconConfig c_config_{};
        PAReconHandle *handle_ = nullptr;
    };
} // namespace pa_recon_qt

#endif /* CE3A5F30_2F7C_4FFE_989C_2AD5A098E5A1 */
