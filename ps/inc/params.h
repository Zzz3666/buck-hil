//=============================================================================
// params.h — Buck HIL 参数管理与 PL 寄存器映射
//
// 所有参数通过 AXI-Lite GP0 写入 PL 的 param_regs 模块。
// dt/L 和 dt/C 在 PS 端预计算为 Q8.24 定点数后写入。
// 参数双缓冲: PL 在 PWM 周期边界统一锁存。
//=============================================================================

#ifndef PARAMS_H
#define PARAMS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// AXI-Lite 寄存器偏移 (相对于 AXI GP0 基址, 由 Vivado Address Editor 分配)
// 注: 最终基址由 bitstream 决定，此处定义为逻辑偏移，用宏重映射
//=============================================================================

// --- Buck 求解器参数 (可读/写) ---
#define PL_REG_VIN          0x000  // Q16.16, input voltage (V)
#define PL_REG_L_VAL        0x004  // Q8.24,  dt/L
#define PL_REG_C_VAL        0x008  // Q8.24,  dt/C
#define PL_REG_R_LOAD        0x00C  // Q16.16, R_load (Ω) — reserved
#define PL_REG_INV_R_LOAD   0x010  // Q16.16, 1/R_load
#define PL_REG_R_L          0x014  // Q8.24,  inductor ESR (Ω)
#define PL_REG_VF           0x018  // Q16.16, diode Vf (V)
#define PL_REG_IL_MAX       0x01C  // Q16.16, current clamp (A)

// --- DAC 接口 (只写数据, R/W 配置) ---
#define PL_REG_DAC_VOUT_DATA   0x020  // W, 16-bit DAC code for Vout
#define PL_REG_DAC_IL_DATA     0x024  // W, 16-bit DAC code for IL
#define PL_REG_DAC_SCALE_VOUT  0x028  // R/W, Q16.16 Vout→DAC scale
#define PL_REG_DAC_SCALE_IL    0x02C  // R/W, Q16.16 IL→DAC scale
#define PL_REG_DAC_UPDATE_DIV  0x030  // R/W, DAC update divider (每N步更新一次)
#define PL_REG_DAC_CTRL        0x034  // R/W, DAC init control (bit0=trigger init seq)

// --- 捕获管理器 ---
#define PL_REG_TRIG_SRC      0x040  // R/W, 触发源: 0=软件 1=Vout↑ 2=Vout↓ 3=IL↑ ...
#define PL_REG_TRIG_LEVEL    0x044  // R/W, Q16.16 触发阈值
#define PL_REG_TRIG_ARM      0x048  // R/W, bit0=使能触发
#define PL_REG_TRIG_STATUS   0x04C  // R,   bit0=触发发生, 写1清除

// --- 仿真控制 ---
#define PL_REG_CTRL          0x100  // R/W: bit0=run, bit1=reset, bit2=param_update
#define PL_REG_STATUS        0x104  // R:   bit0=running, bit1=pwm_ok, bit2=triggered
#define PL_REG_FW_VERSION    0x108  // R:   BCD 版本号
#define PL_REG_DEVICE_ID     0x10C  // R:   设备唯一ID低32位

//=============================================================================
// 参数 ID (与上位机协议一致)
//=============================================================================
typedef enum {
    PARAM_L          = 0x0001,
    PARAM_C          = 0x0002,
    PARAM_R_LOAD     = 0x0003,
    PARAM_VIN        = 0x0004,
    PARAM_R_L        = 0x0005,
    PARAM_VF         = 0x0006,
    PARAM_F_SW       = 0x0007,
    PARAM_IL_MAX     = 0x0008,
    PARAM_VOUT_SCALE = 0x0009,
    PARAM_IL_SCALE   = 0x000A,
    PARAM_DAC_UPDATE_DIV = 0x000B,
    PARAM_FW_VERSION     = 0x0100,
    PARAM_DEVICE_ID      = 0x0101,
} param_id_t;

//=============================================================================
// 参数范围
//=============================================================================
#define PARAM_L_MIN          100u            // nH
#define PARAM_L_MAX          1000000u
#define PARAM_C_MIN          100u            // pF
#define PARAM_C_MAX          100000000u
#define PARAM_R_LOAD_MIN     10u             // mΩ
#define PARAM_R_LOAD_MAX     1000000u
#define PARAM_VIN_MIN        0u              // mV
#define PARAM_VIN_MAX        50000u
#define PARAM_R_L_MIN        0u              // mΩ
#define PARAM_R_L_MAX        100000u
#define PARAM_VF_MIN         0u              // mV
#define PARAM_VF_MAX         2000u
#define PARAM_FSW_MIN        10000u          // Hz
#define PARAM_FSW_MAX        1000000u
#define PARAM_IL_MAX_MIN     0u              // mA
#define PARAM_IL_MAX_MAX     20000u
#define PARAM_DAC_DIV_MIN    1u
#define PARAM_DAC_DIV_MAX    1000u

//=============================================================================
// 参数镜像 (DDR 本地副本)
//=============================================================================
typedef struct {
    uint32_t l_nh;
    uint32_t c_pf;
    uint32_t r_load_mohm;
    uint32_t vin_mv;
    uint32_t r_l_mohm;
    uint32_t vf_mv;
    uint32_t f_sw_hz;
    uint32_t il_max_ma;
    uint32_t dac_update_div;
} param_mirror_t;

//=============================================================================
// 默认参数
//=============================================================================
#define DEFAULT_L_NH          100000u    // 100 μH
#define DEFAULT_C_PF          1000000u   // 100 μF → 1e6 pF
#define DEFAULT_R_LOAD_MOHM   10000u     // 10 Ω
#define DEFAULT_VIN_MV        12000u     // 12 V
#define DEFAULT_R_L_MOHM      100u       // 100 mΩ
#define DEFAULT_VF_MV         700u       // 0.7 V
#define DEFAULT_FSW_HZ        200000u    // 200 kHz
#define DEFAULT_IL_MAX_MA     10000u     // 10 A
#define DEFAULT_DAC_DIV       10u        // 每 10 步 (1μs) 更新一次

//=============================================================================
// API
//=============================================================================

// 初始化参数镜像为默认值, 并写入 PL
void params_init(void);

// 读参数 (返回 Q16.16 定点值)
// id: PARAM_xxx, 返回 32-bit 值, *ok 为 true 表示成功
int32_t params_read(param_id_t id, bool *ok);

// 写参数 (value 为协议中定义的工程单位原始值)
// 返回 0=成功, >0=错误码
int params_write(param_id_t id, uint32_t value);

// 批量写参数 (count 个 {id:2B, value:4B} 项, total bytes = count*6)
int params_write_batch(const uint8_t *data, uint8_t count);

// 预计算 dt/L 和 dt/C (Q8.24)
uint32_t params_calc_dt_over_L(uint32_t l_nh);
uint32_t params_calc_dt_over_C(uint32_t c_pf);

// 获取参数镜像指针 (用于状态响应帧构建)
const param_mirror_t *params_get_mirror(void);

#ifdef __cplusplus
}
#endif

#endif // PARAMS_H
