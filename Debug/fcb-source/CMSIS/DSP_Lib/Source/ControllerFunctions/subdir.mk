################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_pid_init_f32.c \
../fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_pid_reset_f32.c \
../fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_sin_cos_f32.c 

OBJS += \
./fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_pid_init_f32.o \
./fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_pid_reset_f32.o \
./fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_sin_cos_f32.o 

C_DEPS += \
./fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_pid_init_f32.d \
./fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_pid_reset_f32.d \
./fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/arm_sin_cos_f32.d 


# Each subdirectory must supply rules for building sources it contributes
fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/%.o: ../fcb-source/CMSIS/DSP_Lib/Source/ControllerFunctions/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: Cross ARM C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O0 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -ffreestanding -fno-move-loop-invariants -Wall -Wextra  -g3 -DSTM32F303VC -DARM_MATH_CM4 -DSTM32F303xC -DUSE_HAL_DRIVER -DUSE_USB_INTERRUPT_REMAPPED -D__FPU_USED -D__FPU_PRESENT -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\fcb\inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\cmsis-boot" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\communication" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\communication\usb-cdc-com\inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\fcb-drivers\BSP\STM32F3-Discovery" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\fcb-drivers\BSP\Components\Common" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\fcb-drivers\BSP\Components\l3gd20" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\fcb-drivers\BSP\Components\lsm303dlhc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\FreeRTOS-Plus\Source\FreeRTOS-Plus-CLI" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\nanopb-0.3.3-windows-x86" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\sensors\inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\utilities\inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\STM32Cube_FW_F3_V1.1.0\Drivers\BSP\Components\Common" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\STM32Cube_FW_F3_V1.1.0\Drivers\BSP\STM32F3-Discovery" -I"C:\Users\A501632\workspace\dragonfly-fcb\Tools\4.9 2015q1\arm-none-eabi\include" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\communication\protobuf" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\CMSIS\Include" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\CMSIS\Device\ST\STM32F3xx\Include" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\CMSIS\DSP_Lib\Examples\Common\Include" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\STM32F3xx_HAL_Driver\Inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\STM32_USB_Device_Library\Core\Inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\STM32_USB_Device_Library\Class\CDC\Inc" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\FreeRTOS\Source\include" -I"C:\Users\A501632\workspace\dragonfly-fcb\fcb-source\FreeRTOS\Source\portable\GCC\ARM_CM4F" -std=gnu11 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

