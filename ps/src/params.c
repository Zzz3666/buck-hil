//=============================================================================
// params.c — 参数管理: 镜像维护, 范围校验, dt/L dt/C 预计算, PL 寄存器同步
//
// 参数写流程: PC → 协议层 → params_write() → 校验 → 写镜像 → 写 PL
// dt/L 和 dt/C 是 PS 预计算的 Q8.24 定点数, 直接写入 PL 求解器系数寄存器
//=============================================================================

#include "params.h"
#include "platform.h"
#include "protocol.h"   // for ERR_PARAM_RANGE, ERR_INVALID_PARAM_ID
#include "logger.h"
#include <string.h>

//=============================================================================
// 参数镜像 (DDR 副本)
//=============================================================================
static param_mirror_t g_params;

//=============================================================================
// 预计算 dt/L = 100.0 / L_nH
// Δt = 100 ns = 100e-9 s
// L 输入单位: nH = 1e-9 H
// dt/L = 100e-9 / (L × 1e-9) = 100 / L  (无量纲比值)
// Q8.24: (100.0 << 24) / L = 0x64000000 / L
//=============================================================================
uint32_t params_calc_dt_over_L(uint32_t l_nh)
{
    if (l_nh == 0)
        return 0xFFFFFFFFu;
    uint64_t num = (uint64_t)100u << 24;   // 100.0 in Q8.24
    return (uint32_t)(num / l_nh);
}

//=============================================================================
// 预计算 dt/C = 100000.0 / C_pF
// Δt = 100 ns = 100e-9 s
// C 输入单位: pF = 1e-12 F
// dt/C = 100e-9 / (C × 1e-12) = 100000 / C  (无量纲比值 × 1000)
// Q8.24: (100000.0 << 24) / C
//=============================================================================
uint32_t params_calc_dt_over_C(uint32_t c_pf)
{
    if (c_pf == 0)
        return 0xFFFFFFFFu;
    uint64_t num = (uint64_t)100000u << 24;
    return (uint32_t)(num / c_pf);
}

//=============================================================================
// 参数值写 PL 寄存器 (内部函数)
// 注意: 参数更新后需要触发 param_update 脉冲 → PL 在 PWM 边界锁存
//=============================================================================
static void sync_params_to_pl(void)
{
    // 停止仿真时才能安全更新参数, 或使用 PL 双缓冲
    // 此处假定参数更新在仿真停止或 PL 双缓冲安全时调用

    // 写基础参数到 PL
    pl_write32(PL_REG_VIN,        g_params.vin_mv       << 16); // mV → Q16.16
    pl_write32(PL_REG_L_VAL,      params_calc_dt_over_L(g_params.l_nh));
    pl_write32(PL_REG_C_VAL,      params_calc_dt_over_C(g_params.c_pf));
    pl_write32(PL_REG_INV_R_LOAD, (1000000u / g_params.r_load_mohm) << 16); // 1/R
    pl_write32(PL_REG_R_L,        g_params.r_l_mohm     << 16);
    pl_write32(PL_REG_VF,         g_params.vf_mv        << 16);
    pl_write32(PL_REG_IL_MAX,     g_params.il_max_ma    << 16);

    // DAC 缩放因子: DAC80508 0~5V 满量程 → 65535 code
    // Vout: 0~12V → 运放 2.4× → DAC 0~5V → code = V * 65535/5
    // IL: 0~10A → 运放 0.5V/A → DAC 0~5V → code = I * 0.5 * 65535/5
    uint32_t dac_scale_vout = (uint32_t)((65535.0 / 5.0) * 65536.0); // Q16.16
    uint32_t dac_scale_il   = (uint32_t)((0.5 * 65535.0 / 5.0) * 65536.0);

    pl_write32(PL_REG_DAC_SCALE_VOUT, dac_scale_vout);
    pl_write32(PL_REG_DAC_SCALE_IL,   dac_scale_il);
    pl_write32(PL_REG_DAC_UPDATE_DIV, g_params.dac_update_div);

    // 触发 PL 参数更新脉冲
    pl_write32(PL_REG_CTRL, pl_read32(PL_REG_CTRL) | 0x4u);  // set bit2
    for (volatile int i = 0; i < 10; i++) {}                   // brief hold
    pl_write32(PL_REG_CTRL, pl_read32(PL_REG_CTRL) & ~0x4u); // clear bit2
}

//=============================================================================
// 初始化
//=============================================================================
void params_init(void)
{
    g_params.l_nh           = DEFAULT_L_NH;
    g_params.c_pf           = DEFAULT_C_PF;
    g_params.r_load_mohm    = DEFAULT_R_LOAD_MOHM;
    g_params.vin_mv         = DEFAULT_VIN_MV;
    g_params.r_l_mohm       = DEFAULT_R_L_MOHM;
    g_params.vf_mv          = DEFAULT_VF_MV;
    g_params.f_sw_hz        = DEFAULT_FSW_HZ;
    g_params.il_max_ma      = DEFAULT_IL_MAX_MA;
    g_params.dac_update_div = DEFAULT_DAC_DIV;

    sync_params_to_pl();

    // 写版本号和设备ID
    pl_write32(PL_REG_FW_VERSION, 0x00010000u);   // v1.0.0 (BCD)
    pl_write32(PL_REG_DEVICE_ID,  0x00000001u);    // placeholder

    // 触发 DAC 初始化序列 (bit0 = init trigger)
    pl_write32(PL_REG_DAC_CTRL, 0x1u);
    for (volatile int i = 0; i < 1000; i++) {}
    pl_write32(PL_REG_DAC_CTRL, 0x0u);

    LOG_I("Params initialized: L=%lu nH, C=%lu pF, R=%lu mΩ, Vin=%lu mV, Fs=%lu Hz",
          (unsigned long)g_params.l_nh,
          (unsigned long)g_params.c_pf,
          (unsigned long)g_params.r_load_mohm,
          (unsigned long)g_params.vin_mv,
          (unsigned long)g_params.f_sw_hz);
}

//=============================================================================
// 读参数
//=============================================================================
int32_t params_read(param_id_t id, bool *ok)
{
    *ok = true;

    switch (id) {
    case PARAM_L:          return (int32_t)g_params.l_nh;
    case PARAM_C:          return (int32_t)g_params.c_pf;
    case PARAM_R_LOAD:     return (int32_t)g_params.r_load_mohm;
    case PARAM_VIN:        return (int32_t)g_params.vin_mv;
    case PARAM_R_L:        return (int32_t)g_params.r_l_mohm;
    case PARAM_VF:         return (int32_t)g_params.vf_mv;
    case PARAM_F_SW:       return (int32_t)g_params.f_sw_hz;
    case PARAM_IL_MAX:     return (int32_t)g_params.il_max_ma;
    case PARAM_DAC_UPDATE_DIV: return (int32_t)g_params.dac_update_div;
    case PARAM_FW_VERSION: return (int32_t)pl_read32(PL_REG_FW_VERSION);
    case PARAM_DEVICE_ID:  return (int32_t)pl_read32(PL_REG_DEVICE_ID);
    default:
        *ok = false;
        return 0;
    }
}

//=============================================================================
// 范围校验
//=============================================================================
static int validate_range(uint32_t val, uint32_t min_val, uint32_t max_val)
{
    if (val < min_val || val > max_val)
        return ERR_PARAM_RANGE;
    return 0;
}

//=============================================================================
// 写单个参数
//=============================================================================
int params_write(param_id_t id, uint32_t value)
{
    int err = 0;

    switch (id) {
    case PARAM_L:
        err = validate_range(value, PARAM_L_MIN, PARAM_L_MAX);
        if (err) break;
        g_params.l_nh = value;
        pl_write32(PL_REG_L_VAL, params_calc_dt_over_L(value));
        break;

    case PARAM_C:
        err = validate_range(value, PARAM_C_MIN, PARAM_C_MAX);
        if (err) break;
        g_params.c_pf = value;
        pl_write32(PL_REG_C_VAL, params_calc_dt_over_C(value));
        break;

    case PARAM_R_LOAD:
        err = validate_range(value, PARAM_R_LOAD_MIN, PARAM_R_LOAD_MAX);
        if (err) break;
        g_params.r_load_mohm = value;
        if (value > 0)
            pl_write32(PL_REG_INV_R_LOAD,
                       (1000000u / value) << 16);
        break;

    case PARAM_VIN:
        err = validate_range(value, PARAM_VIN_MIN, PARAM_VIN_MAX);
        if (err) break;
        g_params.vin_mv = value;
        pl_write32(PL_REG_VIN, value << 16);
        break;

    case PARAM_R_L:
        err = validate_range(value, PARAM_R_L_MIN, PARAM_R_L_MAX);
        if (err) break;
        g_params.r_l_mohm = value;
        pl_write32(PL_REG_R_L, value << 16);
        break;

    case PARAM_VF:
        err = validate_range(value, PARAM_VF_MIN, PARAM_VF_MAX);
        if (err) break;
        g_params.vf_mv = value;
        pl_write32(PL_REG_VF, value << 16);
        break;

    case PARAM_F_SW:
        err = validate_range(value, PARAM_FSW_MIN, PARAM_FSW_MAX);
        if (err) break;
        g_params.f_sw_hz = value;
        // F_SW 变化需要更新 PWM 周期参数
        // TODO: 写 PL 周期寄存器 (需 PL 支持)
        break;

    case PARAM_IL_MAX:
        err = validate_range(value, PARAM_IL_MAX_MIN, PARAM_IL_MAX_MAX);
        if (err) break;
        g_params.il_max_ma = value;
        pl_write32(PL_REG_IL_MAX, value << 16);
        break;

    case PARAM_DAC_UPDATE_DIV:
        err = validate_range(value, PARAM_DAC_DIV_MIN, PARAM_DAC_DIV_MAX);
        if (err) break;
        g_params.dac_update_div = value;
        pl_write32(PL_REG_DAC_UPDATE_DIV, value);
        break;

    default:
        return ERR_INVALID_PARAM_ID;
    }

    if (err == 0)
        LOG_D("Param write: id=0x%04X val=%lu ok", id, (unsigned long)value);
    else
        LOG_W("Param write: id=0x%04X val=%lu ERR=%d", id, (unsigned long)value, err);

    return err;
}

//=============================================================================
// 批量写参数
// data: count × {id(2B,BE) + value(4B,BE)}
//=============================================================================
int params_write_batch(const uint8_t *data, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        uint16_t id = ((uint16_t)data[i * 6] << 8)
                    |  data[i * 6 + 1];
        uint32_t val = ((uint32_t)data[i * 6 + 2] << 24)
                     | ((uint32_t)data[i * 6 + 3] << 16)
                     | ((uint32_t)data[i * 6 + 4] << 8)
                     |  data[i * 6 + 5];

        int err = params_write((param_id_t)id, val);
        if (err != 0)
            return err;
    }
    return 0;
}

//=============================================================================
// 获取参数镜像
//=============================================================================
const param_mirror_t *params_get_mirror(void)
{
    return &g_params;
}
