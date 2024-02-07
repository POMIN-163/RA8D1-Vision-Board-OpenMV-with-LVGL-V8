MCU=STM32F427xx
CPU=cortex-m4
FPU=fpv4-sp-d16
PORT=stm32
HAL_DIR=hal/stm32/f4
ARM_MATH=ARM_MATH_CM4
HAL_INC='<stm32f4xx_hal.h>'
CFLAGS_MCU=MCU_SERIES_F4
VECT_TAB_OFFSET=0x10000
MAIN_APP_ADDR=0x08010000
OMV_HSE_VALUE=12000000
DFU_DEVICE=0x0483:0xdf11
OMV_ENABLE_BL=1
OMV_ENABLE_UVC=1
MICROPY_PY_SENSOR = 1
MICROPY_PY_WINC1500 = 1
MICROPY_PY_LCD = 1
MICROPY_PY_TV = 1
MICROPY_PY_BUZZER = 0
