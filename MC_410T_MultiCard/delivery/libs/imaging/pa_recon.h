#ifndef A461E16C_3E6E_4484_AFFC_DC29020AD602
#define A461E16C_3E6E_4484_AFFC_DC29020AD602

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(PA_RECON_EXPORTS)
#define PA_RECON_API __declspec(dllexport)
#else
#define PA_RECON_API __declspec(dllimport)
#endif
#else
#if defined(PA_RECON_EXPORTS)
#define PA_RECON_API __attribute__((visibility("default")))
#else
#define PA_RECON_API
#endif
#endif

// C 接口默认参数（pa_recon_default_config 使用这些值）
#define PA_RECON_DEFAULT_NX 1200
#define PA_RECON_DEFAULT_NY 800
#define PA_RECON_DEFAULT_DEPTH 25000
#define PA_RECON_DEFAULT_CHANNEL_NUM 64
#define PA_RECON_DEFAULT_CARD_NUM 8
#define PA_RECON_DEFAULT_MOVE_ALINE 100
#define PA_RECON_DEFAULT_CHANNEL_ALINE 50
#define PA_RECON_DEFAULT_DELAY 141
#define PA_RECON_DEFAULT_THRESHOLD 1000.0f
#define PA_RECON_DEFAULT_FS 125e6f
#define PA_RECON_DEFAULT_X0 (-6e-3f)
#define PA_RECON_DEFAULT_Y0 (4e-3f)
#define PA_RECON_DEFAULT_DX 10e-6f
#define PA_RECON_DEFAULT_DY 10e-6f
#define PA_RECON_DEFAULT_DETX_STEP 10e-6f
#define PA_RECON_DEFAULT_BOUNDARY_Y (-1e-6f)
#define PA_RECON_DEFAULT_LOWER_SOS 1500.0f
#define PA_RECON_DEFAULT_UPPER_SOS 1560.0f

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct PAReconHandle PAReconHandle;

    typedef enum PAReconTimeWeightMode
    {
        PA_RECON_TIME_WEIGHT_NONE = 0,
        PA_RECON_TIME_WEIGHT_P_MINUS_TDP = 1,
        PA_RECON_TIME_WEIGHT_TWO_P_MINUS_TDP = 2
    } PAReconTimeWeightMode;

    typedef enum PAReconLookDirection
    {
        PA_RECON_LOOK_UP = 0,
        PA_RECON_LOOK_DOWN = 1
    } PAReconLookDirection;

    typedef enum PAReconInterpMode
    {
        PA_RECON_INTERP_LINEAR = 0,
        PA_RECON_INTERP_NEAREST = 1
    } PAReconInterpMode;

    typedef enum PAReconOutputType
    {
        PA_RECON_OUTPUT_RF = 0,
        PA_RECON_OUTPUT_ENV = 1,
        PA_RECON_OUTPUT_DB = 2
    } PAReconOutputType;

    typedef enum PAReconFilterType
    {
        PA_RECON_FILTER_NONE = 0,
        PA_RECON_FILTER_LOWPASS = 1,
        PA_RECON_FILTER_HIGHPASS = 2,
        PA_RECON_FILTER_BANDPASS = 3
    } PAReconFilterType;

    typedef enum PAReconFiberType
    {
        PA_RECON_FIBER_F8 = 0,
        PA_RECON_FIBER_F64 = 1
    } PAReconFiberType;

    // 重建参数配置。
    // 说明：
    // 1) 所有坐标单位均为米（m），采样率单位为 Hz。
    // 2) 指针字段可为 nullptr；当对应 *_len 为 0 时会被视为未提供。
    // 3) move_aline 是单帧输入脉冲数，channel_aline 是每通道参与重建的脉冲数。
    typedef struct PAReconConfig
    {
        // ---- 成像网格参数 ----
        int nx;
        int ny;

        // ---- 输入数据与采集参数 ----
        int depth;
        int channel_num;
        int card_num;
        int move_aline;
        int channel_aline;
        PAReconFiberType fiber_type;
        int delay;
        float threhold;

        // ---- 成像坐标与采样参数 ----
        float fs;
        float x0;
        float y0;
        float dx;
        float dy;
        float detx_step;

        // ---- 声速模型参数 ----
        int8_t use_dual;
        float boundary_y;
        float lower_sos;
        float upper_sos;

        // ---- 通道位置校准参数 ----
        int8_t is_center_align;

        const int *x_peizhun;
        size_t x_peizhun_len;
        const int *y_peizhun;
        size_t y_peizhun_len;
        const int *CaliFiberDelay;
        size_t CaliFiberDelay_len;
        const int *CaliCardDelay;
        size_t CaliCardDelay_len;
        int delay_time_point;
        int s1period;

        // ---- 预处理参数 ----
        const float *apod;
        size_t apod_len;
        const float *bandpass_hz;
        size_t bandpass_hz_len;
        PAReconFilterType filter_type;
        const double *filter_cut;
        size_t filter_cut_len;
        int filter_order;

        // ---- 算法模式参数 ----
        PAReconTimeWeightMode time_weight;
        PAReconLookDirection look_dir;
        PAReconInterpMode interp_mode;

        // ---- 输出控制参数 ----
        int8_t remove_dc;
        float ang_sigma_deg;
        float ang_mu_deg;
        float dyn_range_db;

        PAReconOutputType output_type;
        int8_t norm_by_w;
        int8_t is_read_fiber_position;
        int8_t is_cutoff_loc; // 是否对末尾干扰段进行衰减处理，0/1
        int cutoff_point;     // 衰减起始采样点（原始坐标，C++内部会自动换算）
    } PAReconConfig;

    // 将 config 写为库内默认值（见 PA_RECON_DEFAULT_* 宏）。
    // 传入空指针时行为未定义，调用方需保证非空。
    PA_RECON_API void pa_recon_default_config(PAReconConfig *config);

    // 根据配置创建重建器句柄。
    // 成功返回非空句柄；失败返回 nullptr，可通过 pa_recon_last_error 查询错误。
    PA_RECON_API PAReconHandle *pa_recon_create(const PAReconConfig *config);

    // 销毁句柄并释放资源。允许传入 nullptr（无操作）。
    PA_RECON_API void pa_recon_destroy(PAReconHandle *handle);

    // 送入单脉冲数据（长度通常为 card_num * depth）。
    // 返回 0 表示成功，非 0 表示失败。
    PA_RECON_API int pa_recon_single_pulse(PAReconHandle *handle, const float *data, size_t data_len);

    // 获取一帧输出。
    // 若当前无可用输出，written_len 可能为 0 且返回 0。
    // output_capacity 必须不小于 pa_recon_get_output_size 返回值。
    PA_RECON_API int pa_recon_get_output(PAReconHandle *handle, float *output, size_t output_capacity, size_t *written_len);

    // 查询单脉冲输入长度（float 元素数）。
    PA_RECON_API size_t pa_recon_get_single_pulse_size(const PAReconHandle *handle);

    // 查询单帧输出长度（float 元素数，通常为 nx * ny）。
    PA_RECON_API size_t pa_recon_get_output_size(const PAReconHandle *handle);

    // 返回最近一次错误信息（线程内可读字符串）。
    PA_RECON_API const char *pa_recon_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* A461E16C_3E6E_4484_AFFC_DC29020AD602 */
