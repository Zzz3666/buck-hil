#=============================================================================
# Makefile — Buck HIL PS Firmware (ZU3EG Cortex-A53 Baremetal + lwIP)
#
# 用法:
#   make                → 编译 (依赖 Vitis 导出的 BSP)
#   make BSP_DIR=/path  → 指定 BSP 路径
#   make clean          → 清理
#   make standalone     → 无 BSP 的独立编译 (用于代码检查)
#=============================================================================

# ---- 目标 ----
TARGET       := buck_hil_ps
ELF          := $(TARGET).elf
BIN          := $(TARGET).bin

# ---- 工具链 (aarch64 bare-metal) ----
CROSS        ?= aarch64-none-elf-
CC           := $(CROSS)gcc
AS           := $(CROSS)as
LD           := $(CROSS)ld
OBJCOPY      := $(CROSS)objcopy
OBJDUMP      := $(CROSS)objdump
SIZE         := $(CROSS)size

# ---- BSP 目录 (Vitis 导出) ----
# 典型路径: ~/vitis_workspace/buck_hil_ps/export/buck_hil_ps/sw/buck_hil_ps/standalone_domain/bsp/
BSP_DIR      ?= $(VITIS_BSP_DIR)
BSP_INC      := $(BSP_DIR)/psu_cortexa53_0/include
BSP_LIB      := $(BSP_DIR)/psu_cortexa53_0/lib

# ---- lwIP (通常由 BSP 包含, 也可独立编译) ----
LWIP_DIR     ?= $(BSP_DIR)/psu_cortexa53_0/libsrc/lwip211_v1_2/src
LWIP_INC     := $(LWIP_DIR)/include \
                $(LWIP_DIR)/include/lwip \
                $(LWIP_DIR)/include/lwip/priv \
                $(LWIP_DIR)/contrib/ports/xilinx/include

# ---- 项目路径 ----
SRC_DIR      := src
INC_DIR      := inc
BUILD_DIR    := build
LINKER_SCRIPT:= lscript.ld

# ---- 源文件 ----
C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
ASM_SRCS     := $(wildcard $(SRC_DIR)/*.S)
OBJS         := $(patsubst $(SRC_DIR)/%.c,   $(BUILD_DIR)/%.o, $(C_SRCS))
OBJS         += $(patsubst $(SRC_DIR)/%.S,   $(BUILD_DIR)/%.o, $(ASM_SRCS))

# ---- 预处理器定义 ----
DEFINES      := -DUSE_LWIP \
                -DPLATFORM_ZYNQMP \
                -D__aarch64__ \
                -DARM64 \
                -DEL1_NONSECURE

# ---- 编译标志 (Cortex-A53, 64-bit, bare-metal) ----
ARCH_FLAGS   := -march=armv8-a -mtune=cortex-a53
CPU_FLAGS    := -mcpu=cortex-a53 -mstrict-align
CFLAGS       := $(ARCH_FLAGS) $(CPU_FLAGS) \
                -O2 -g -Wall -Wextra \
                -ffunction-sections -fdata-sections \
                -fno-common -fno-builtin \
                -fno-stack-protector \
                -nostdlib -nostartfiles \
                -ffreestanding \
                -mcmodel=small \
                $(DEFINES)

# ---- Include 路径 ----
INCLUDES     := -I$(INC_DIR) \
                -I$(BSP_INC) \
                $(addprefix -I, $(LWIP_INC))

# ---- 链接标志 ----
LDFLAGS      := -Wl,-T,$(LINKER_SCRIPT) \
                -Wl,--gc-sections \
                -Wl,--build-id=none \
                -nostdlib -nostartfiles \
                -Wl,--start-group \
                -lxil -llwip4 -lm \
                -Wl,--end-group \
                -L$(BSP_LIB)

# ---- 颜色输出 ----
COLOR_RESET  := \033[0m
COLOR_GREEN  := \033[32m
COLOR_YELLOW := \033[33m
COLOR_RED    := \033[31m
COLOR_CYAN   := \033[36m

#=============================================================================
# 规则
#=============================================================================

.PHONY: all clean standalone check help

all: $(BUILD_DIR) $(ELF)
	@echo "$(COLOR_GREEN)✓ Build complete: $(ELF)$(COLOR_RESET)"
	@$(SIZE) $(ELF)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ---- 编译 C ----
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "$(COLOR_CYAN)  CC    $<$(COLOR_RESET)"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@ $(INCLUDES) 2>&1 | \
		sed 's/^/  /' || { echo "$(COLOR_RED)  FAILED: $<$(COLOR_RESET)"; exit 1; }

# ---- 编译汇编 ----
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@echo "$(COLOR_CYAN)  AS    $<$(COLOR_RESET)"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---- 链接 ----
$(ELF): $(OBJS)
	@echo "$(COLOR_YELLOW)  LD    $@$(COLOR_RESET)"
	@$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@ 2>&1 | \
		sed 's/^/  /' || { echo "$(COLOR_RED)  LINK FAILED$(COLOR_RESET)"; exit 1; }

# ---- 二进制文件 ----
$(BIN): $(ELF)
	@$(OBJCOPY) -O binary $< $@

#=============================================================================
# 独立模式 (无 BSP — 仅语法检查)
#=============================================================================
standalone: CFLAGS += -DNO_XILINX_BSP
standalone: DEFINES :=
standalone: INCLUDES := -I$(INC_DIR)
standalone: LDFLAGS := -Wl,-T,$(LINKER_SCRIPT) -nostdlib
standalone: $(BUILD_DIR) $(OBJS)
	@echo "$(COLOR_YELLOW)Standalone build (no BSP): objects only$(COLOR_RESET)"

#=============================================================================
# 代码检查
#=============================================================================
check: standalone
	@echo "$(COLOR_GREEN)✓ Syntax check passed$(COLOR_RESET)"

#=============================================================================
# 清理
#=============================================================================
clean:
	@rm -rf $(BUILD_DIR) $(ELF) $(BIN)
	@echo "$(COLOR_GREEN)✓ Clean$(COLOR_RESET)"

#=============================================================================
# 帮助
#=============================================================================
help:
	@echo "Buck HIL PS Firmware Build"
	@echo ""
	@echo "Targets:"
	@echo "  make [BSP_DIR=/path/to/bsp]    — Full build with Xilinx BSP"
	@echo "  make standalone                — Build objects only (no BSP, syntax check)"
	@echo "  make check                     — Alias for standalone"
	@echo "  make clean                     — Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  BSP_DIR    — Path to Vitis exported BSP (default: \$$VITIS_BSP_DIR)"
	@echo "  CROSS      — Cross-compiler prefix (default: aarch64-none-elf-)"
