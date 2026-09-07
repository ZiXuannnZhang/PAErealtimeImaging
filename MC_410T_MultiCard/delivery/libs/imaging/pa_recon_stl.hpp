#ifndef PA_RECON_STL_HPP
#define PA_RECON_STL_HPP

#include "pa_recon.h"

#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <algorithm>

namespace pa_recon_stl
{
    struct Config
    {
        enum class TimeWeightMode
        {
            None = 0,
            PMinusTdp = 1,
            TwoPMinusTdp = 2
        };

        enum class LookDirection
        {
            Up = 0,
            Down = 1
        };

        enum class InterpMode
        {
            Linear = 0,
            Nearest = 1
        };

        enum class OutputType
        {
            RF = 0,
            ENV = 1,
            DB = 2
        };

        enum class FilterType
        {
            None = 0,
            Lowpass = 1,
            Highpass = 2,
            Bandpass = 3
        };

        enum class FiberType
        {
            F8 = 0,
            F64 = 1
        };

        // ---- 输入数据与采集参数 ----
        int depth = 50000;
        int channel_num = 64;
        int card_num = 4;
        int move_aline = 100;
        int channel_aline = 50;
        FiberType fiber_type = FiberType::F64;
        int delay = 141;
        float threhold = 1000.0f;

        // ---- 成像坐标与采样参数 ----
        float fs = 250e6f;
        float x0 = -6e-3f;
        float y0 = 4e-3f;
        float dx = 10e-6f;
        float dy = 10e-6f;
        int nx = 1200;
        int ny = 800;
        float detx_step = 10e-6f;

        // ---- 声速模型参数 ----
        bool use_dual = false;
        float boundary_y = -1e-6f;
        float lower_sos = 1500.0f;
        float upper_sos = 1560.0f;
        bool is_center_align = true;

        // ---- 拼接与通道校准参数 ----
        std::vector<int> x_peizhun{14, 9, -2, 12, 9, 0, -2, 0, -4, -4, -4, -5, 19, 15, 24, 2,
                                   5, 0, 7, 7, 7, 5, 6, 0, 6, 3, 1, -1, -2, -10, 8, 0,
                                   -5, 9, -7, -5, -3, -8, -13, -2, -12, -5, -10, -6, -9, -5, -5, -11,
                                   0, -13, -14, 45, 50, -4, 6, 11, 9, 1, 5, 9, 4, 2, 7, 1};
        std::vector<int> y_peizhun{-10, -16, -5, -15, -2, 0, -2, -10, -3, -12, -8, -8, -4, -8, -16, -9,
                                   -19, -14, -18, -13, -14, 0, -12, -16, -17, -20, -4, -18, -5, -16, 4, 0,
                                   -10, -14, -14, -5, -15, -4, -15, -15, -18, 0, -9, -11, -13, -13, -10, -13,
                                   -5, -4, -15, -13, -3, -15, -10, -19, -4, -12, -5, -5, -15, -15, -17, -17};
        std::vector<int> CaliFiberDelay{534, 484, 428, 372, 322, 264, 282, 202};
        std::vector<int> CaliCardDelay{0, 2, 2, 2, 4, 6, 6, 8};
        int delay_time_point = 1601;
        int s1period = 50000;

        // ---- 预处理参数 ----
        std::vector<float> apod{};
        std::vector<float> bandpass_hz{};
        FilterType filter_type = FilterType::None;
        std::vector<double> filter_cut{};
        int filter_order = 0;

        // ---- 算法模式参数 ----
        TimeWeightMode time_weight = TimeWeightMode::None;
        LookDirection look_dir = LookDirection::Up;
        InterpMode interp_mode = InterpMode::Linear;

        bool remove_dc = false;
        float ang_sigma_deg = std::numeric_limits<float>::infinity();
        float ang_mu_deg = 0.0f;
        float dyn_range_db = 50.0f;

        // ---- 输出参数 ----
        OutputType output_type = OutputType::RF;
        bool norm_by_w = false;
        bool is_read_fiber_position = true;
        bool has_ang_sigma_deg = true;
    };

    class PAReconstructor
    {
    public:
        explicit PAReconstructor(const Config &config)
            : config_(config)
        {
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

        int single_pulse_data(const std::vector<float> &data)
        {
            if (handle_ == nullptr)
                return -1;
            return pa_recon_single_pulse(handle_, data.empty() ? nullptr : data.data(), data.size());
        }

        int get_output(std::vector<float> &output)
        {
            if (handle_ == nullptr)
            {
                output.clear();
                return -1;
            }

            output.resize(output_size());
            size_t written_len = 0;
            int status = pa_recon_get_output(handle_, output.empty() ? nullptr : output.data(), output.size(), &written_len);
            if (status != 0)
            {
                output.clear();
                return status;
            }
            output.resize(written_len);
            return 0;
        }

        size_t single_pulse_size() const
        {
            return (handle_ != nullptr) ? pa_recon_get_single_pulse_size(handle_) : 0;
        }

        size_t output_size() const
        {
            return (handle_ != nullptr) ? pa_recon_get_output_size(handle_)
                                        : static_cast<size_t>(config_.nx) * static_cast<size_t>(config_.ny);
        }

        std::string last_error() const
        {
            const char *err = pa_recon_last_error();
            return err ? std::string(err) : std::string();
        }

        const Config &config() const { return config_; }

    private:
        void sync_c_config()
        {
            std::memset(&c_config_, 0, sizeof(c_config_));
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
            c_config_.lower_sos = config_.lower_sos;
            c_config_.upper_sos = config_.upper_sos;
            c_config_.is_center_align = config_.is_center_align ? 1 : 0;
            c_config_.x_peizhun = config_.x_peizhun.empty() ? nullptr : config_.x_peizhun.data();
            c_config_.x_peizhun_len = config_.x_peizhun.size();
            c_config_.y_peizhun = config_.y_peizhun.empty() ? nullptr : config_.y_peizhun.data();
            c_config_.y_peizhun_len = config_.y_peizhun.size();
            c_config_.CaliFiberDelay = config_.CaliFiberDelay.empty() ? nullptr : config_.CaliFiberDelay.data();
            c_config_.CaliFiberDelay_len = config_.CaliFiberDelay.size();
            c_config_.CaliCardDelay = config_.CaliCardDelay.empty() ? nullptr : config_.CaliCardDelay.data();
            c_config_.CaliCardDelay_len = config_.CaliCardDelay.size();
            c_config_.delay_time_point = config_.delay_time_point;
            c_config_.s1period = config_.s1period;
            c_config_.apod = config_.apod.empty() ? nullptr : config_.apod.data();
            c_config_.apod_len = config_.apod.size();
            c_config_.bandpass_hz = config_.bandpass_hz.empty() ? nullptr : config_.bandpass_hz.data();
            c_config_.bandpass_hz_len = config_.bandpass_hz.size();
            c_config_.filter_type = static_cast<PAReconFilterType>(config_.filter_type);
            c_config_.filter_cut = config_.filter_cut.empty() ? nullptr : config_.filter_cut.data();
            c_config_.filter_cut_len = config_.filter_cut.size();
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
        }

        Config config_{};
        PAReconConfig c_config_{};
        PAReconHandle *handle_ = nullptr;
    };
} // namespace pa_recon_stl

#endif // PA_RECON_STL_HPP
