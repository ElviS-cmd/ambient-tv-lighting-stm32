PROJECT := ambient-tv-lighting
BUILD_DIR ?= build
TOOLCHAIN_PREFIX ?= arm-none-eabi-

CC := $(TOOLCHAIN_PREFIX)gcc
SIZE := $(TOOLCHAIN_PREFIX)size
OBJDUMP := $(TOOLCHAIN_PREFIX)objdump
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy

CPU_FLAGS := -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb
DEFINES := \
	-DDEBUG \
	-DSTM32 \
	-DSTM32F407G_DISC1 \
	-DSTM32F4 \
	-DSTM32F407VGTx \
	-DUSE_HAL_DRIVER \
	-DSTM32F407xx
INCLUDES := \
	-IInc \
	-IDrivers/STM32F4xx_HAL_Driver/Inc \
	-IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy \
	-IDrivers/CMSIS/Device/ST/STM32F4xx/Include \
	-IDrivers/CMSIS/Include

CPPFLAGS := $(DEFINES) $(INCLUDES) $(CPPFLAGS_EXTRA)
CFLAGS := $(CPU_FLAGS) -std=gnu11 -O2 -g3 -Wall -Wextra \
	-ffunction-sections -fdata-sections -fstack-usage
ASFLAGS := $(CPU_FLAGS) -g3 -x assembler-with-cpp
LDFLAGS := $(CPU_FLAGS) -TSTM32F407VGTX_FLASH.ld --specs=nosys.specs \
	--specs=nano.specs -Wl,--gc-sections -Wl,-Map,$(BUILD_DIR)/$(PROJECT).map \
	-static -Wl,--start-group -lc -lm -Wl,--end-group

APP_SOURCES := \
	Src/dcmi_capture.c \
	Src/main.c \
	Src/stm32f4xx_hal_msp.c \
	Src/stm32f4xx_it.c \
	Src/syscalls.c \
	Src/sysmem.c \
	Src/system_stm32f4xx.c \
	Src/ws2812.c

HAL_SOURCES := \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dcmi.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dcmi_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_hcd.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c \
	Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_usb.c

# Match STM32CubeIDE's object order so section placement and memory accounting
# remain directly comparable between the generated and portable builds.
SOURCES := $(HAL_SOURCES) $(APP_SOURCES)
ASM_SOURCES := Startup/startup_stm32f407vgtx.s
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o) $(ASM_SOURCES:.s=.o))
DEPS := $(OBJECTS:.o=.d)
ELF := $(BUILD_DIR)/$(PROJECT).elf

.DEFAULT_GOAL := all

.PHONY: all check clean size

all: $(ELF) $(BUILD_DIR)/$(PROJECT).hex $(BUILD_DIR)/$(PROJECT).bin size

$(ELF): $(OBJECTS) STM32F407VGTX_FLASH.ld
	@mkdir -p $(dir $@)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(OBJDUMP) -h -S $@ > $(BUILD_DIR)/$(PROJECT).list

$(BUILD_DIR)/$(PROJECT).hex: $(ELF)
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/$(PROJECT).bin: $(ELF)
	$(OBJCOPY) -O binary $< $@

size: $(ELF)
	$(SIZE) $(ELF)

check:
	./tools/check_build_matrix.sh

$(BUILD_DIR)/Drivers/%.o: Drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-parameter -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)
