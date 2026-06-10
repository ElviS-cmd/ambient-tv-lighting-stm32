/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "dcmi_capture.h"
#include "ws2812.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Temporary bench tool: turn this Discovery board into a programmer for the
 * TFP401 breakout's EDID EEPROM (the square SDA/SCL/GND/+5V pads next to its
 * HDMI port). Set to 1, flash, follow the LED/Live-Expressions flow in
 * edid_writer_run(), then set back to 0 and reflash the ambilight firmware.
 *
 * Wiring (TFP401 disconnected from HDMI and USB while programming):
 *   breakout +5V pad -> Discovery 3V   (EEPROM runs fine at 3.3 V and this
 *                                       keeps the I2C levels at 3.3 V)
 *   breakout GND pad -> Discovery GND
 *   breakout SCL pad -> PB8
 *   breakout SDA pad -> PB9
 */
#define EDID_WRITER_MODE 1U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
DCMI_HandleTypeDef hdcmi;
DMA_HandleTypeDef hdma_dcmi;

TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_tim2_ch2_ch4;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_DCMI_Init(void);
static void MX_TIM2_Init(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#if EDID_WRITER_MODE
/* Bit-banged I2C master (~25 kHz, open-drain with internal pull-ups) for the
 * 24C02-class EDID EEPROM at address 0x50 on the TFP401 breakout's pads.
 *
 * Flow, watched through Live Expressions:
 *   boot  -> reads all 256 EEPROM bytes into g_edid_dump
 *            g_edid_state = 2 (armed), blue LED blinking
 *            (stock content starts 00 FF .. FF 00 04 81 04 00)
 *   press the blue USER button -> writes the AMBILIGHT EDID (orange LED),
 *            pads bytes 128..255 with 0xFF, then verifies byte-for-byte
 *   green LED blinking = written + verified (g_edid_state = 5)
 *   red LED blinking   = failure; g_edid_state/g_edid_fail_offset say where
 */
#define EDID_EE_ADDR 0x50U

static const uint8_t edid_image[128] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x04, 0x81, 0x20, 0x07, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x24, 0x01, 0x03, 0x80, 0x0F, 0x0A, 0x78, 0x02, 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26,
    0x0F, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xB4, 0x14, 0x00, 0xA0, 0x50, 0xD0, 0x11, 0x20, 0x30, 0x20,
    0x35, 0x00, 0x6C, 0x44, 0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x41, 0x4D, 0x42,
    0x49, 0x4C, 0x49, 0x47, 0x48, 0x54, 0x0A, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x17,
    0x4C, 0x1E, 0x2E, 0x06, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E
};

/* 0=init 1=eeprom-no-ack 2=armed 3=writing 4=verifying 5=OK
 * 6=write-nack 7=verify-mismatch */
volatile uint32_t g_edid_state;
volatile uint32_t g_edid_fail_offset = 0xFFFFFFFFU;
volatile uint8_t g_edid_dump[256];

static void ew_delay(void)
{
    for (volatile uint32_t i = 0U; i < 400U; i++) {
        __NOP();
    }
}

static void ew_scl(uint32_t level)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8,
                      level != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ew_sda(uint32_t level)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9,
                      level != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint32_t ew_sda_read(void)
{
    return HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET ? 1U : 0U;
}

static void ew_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    ew_scl(1U);
    ew_sda(1U);
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);
}

static void ew_start(void)
{
    ew_sda(1U);
    ew_scl(1U);
    ew_delay();
    ew_sda(0U);
    ew_delay();
    ew_scl(0U);
    ew_delay();
}

static void ew_stop(void)
{
    ew_sda(0U);
    ew_delay();
    ew_scl(1U);
    ew_delay();
    ew_sda(1U);
    ew_delay();
}

static uint32_t ew_write_byte(uint8_t value)
{
    uint32_t acked;

    for (int8_t bit = 7; bit >= 0; bit--) {
        ew_sda((value >> bit) & 1U);
        ew_delay();
        ew_scl(1U);
        ew_delay();
        ew_scl(0U);
    }
    ew_sda(1U); /* release for ACK */
    ew_delay();
    ew_scl(1U);
    ew_delay();
    acked = ew_sda_read() == 0U ? 1U : 0U;
    ew_scl(0U);
    ew_delay();
    return acked;
}

static uint8_t ew_read_byte(uint32_t send_ack)
{
    uint8_t value = 0U;

    ew_sda(1U); /* release the line */
    for (int8_t bit = 7; bit >= 0; bit--) {
        ew_delay();
        ew_scl(1U);
        ew_delay();
        value = (uint8_t)((value << 1) | (uint8_t)ew_sda_read());
        ew_scl(0U);
    }
    ew_sda(send_ack != 0U ? 0U : 1U);
    ew_delay();
    ew_scl(1U);
    ew_delay();
    ew_scl(0U);
    ew_sda(1U);
    ew_delay();
    return value;
}

static uint32_t ew_read_all(void)
{
    ew_start();
    if (ew_write_byte((uint8_t)(EDID_EE_ADDR << 1)) == 0U) {
        ew_stop();
        return 0U;
    }
    if (ew_write_byte(0x00U) == 0U) {
        ew_stop();
        return 0U;
    }
    ew_start();
    if (ew_write_byte((uint8_t)((EDID_EE_ADDR << 1) | 1U)) == 0U) {
        ew_stop();
        return 0U;
    }
    for (uint32_t i = 0U; i < 256U; i++) {
        g_edid_dump[i] = ew_read_byte(i < 255U ? 1U : 0U);
    }
    ew_stop();
    return 1U;
}

static uint32_t ew_write_page(uint32_t offset, const uint8_t *data)
{
    ew_start();
    if (ew_write_byte((uint8_t)(EDID_EE_ADDR << 1)) == 0U) {
        ew_stop();
        return 0U;
    }
    if (ew_write_byte((uint8_t)offset) == 0U) {
        ew_stop();
        return 0U;
    }
    for (uint32_t i = 0U; i < 8U; i++) {
        if (ew_write_byte(data[i]) == 0U) {
            ew_stop();
            return 0U;
        }
    }
    ew_stop();
    HAL_Delay(10); /* EEPROM internal write cycle */
    return 1U;
}

static void ew_halt_blink(uint16_t led_pin)
{
    for (;;) {
        HAL_GPIO_TogglePin(GPIOD, led_pin);
        HAL_Delay(200);
    }
}

static void edid_writer_run(void)
{
    static const uint8_t ff_page[8] =
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    ew_gpio_init();
    HAL_Delay(100);

    if (ew_read_all() == 0U) {
        g_edid_state = 1U;
        ew_halt_blink(LD5_Pin); /* red: no ACK - check wiring/power */
    }
    g_edid_state = 2U; /* armed: inspect g_edid_dump, press the USER button */

    while (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_RESET) {
        HAL_GPIO_TogglePin(GPIOD, LD6_Pin); /* blue: waiting */
        HAL_Delay(150);
    }
    HAL_GPIO_WritePin(GPIOD, LD6_Pin, GPIO_PIN_RESET);

    g_edid_state = 3U;
    HAL_GPIO_WritePin(GPIOD, LD3_Pin, GPIO_PIN_SET); /* orange: writing */
    for (uint32_t offset = 0U; offset < 256U; offset += 8U) {
        const uint8_t *src = offset < 128U ? &edid_image[offset] : ff_page;

        if (ew_write_page(offset, src) == 0U) {
            g_edid_state = 6U;
            g_edid_fail_offset = offset;
            ew_halt_blink(LD5_Pin);
        }
    }
    HAL_GPIO_WritePin(GPIOD, LD3_Pin, GPIO_PIN_RESET);

    g_edid_state = 4U;
    if (ew_read_all() == 0U) {
        g_edid_state = 1U;
        ew_halt_blink(LD5_Pin);
    }
    for (uint32_t i = 0U; i < 256U; i++) {
        uint8_t expected = i < 128U ? edid_image[i] : 0xFFU;

        if (g_edid_dump[i] != expected) {
            g_edid_state = 7U;
            g_edid_fail_offset = i;
            ew_halt_blink(LD5_Pin);
        }
    }

    g_edid_state = 5U;
    ew_halt_blink(LD4_Pin); /* green: written and verified */
}
#endif /* EDID_WRITER_MODE */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_DCMI_Init();
  MX_TIM2_Init();

  DCMI_Capture_Init();
  WS2812_Init();
  /* USER CODE BEGIN 2 */
#if EDID_WRITER_MODE
  edid_writer_run(); /* never returns; see flow notes above */
#endif
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* No HAL_Delay here: capture completion is detected by polling the DMA
     * counter, so a 1 ms sleep per pass adds milliseconds of latency to
     * every capture. The loop is naturally paced by the video frame rate.
     */
    DCMI_Capture_Task();
    if (DCMI_Capture_ConsumeLedUpdate() != 0U) {
      WS2812_TaskEdgeZonesRgb(g_dcmi_led_zone_r,
                              g_dcmi_led_zone_g,
                              g_dcmi_led_zone_b,
                              DCMI_LED_ZONE_COUNT);
    }
    WS2812_Flush();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief DCMI Initialization Function
  * @param None
  * @retval None
  */
static void MX_DCMI_Init(void)
{

  /* USER CODE BEGIN DCMI_Init 0 */

  /* USER CODE END DCMI_Init 0 */

  /* USER CODE BEGIN DCMI_Init 1 */

  /* USER CODE END DCMI_Init 1 */
  hdcmi.Instance = DCMI;
  hdcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
  /* Runtime tests show this TFP401 path produces data with active-high
   * HSYNC and none with active-low HSYNC.
   */
  hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
  hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_HIGH;
  hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_HIGH;
  hdcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
  hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
  hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
  if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DCMI_Init 2 */
  __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_LINE | DCMI_IT_VSYNC |
                               DCMI_IT_ERR | DCMI_IT_OVR | DCMI_IT_FRAME);

  /* Sync polarity per source mode (see DCMI_SOURCE_MODE in dcmi_capture.h).
   * The HIGH/HIGH init above matches mode 0 (CEA 720p, positive pulses).
   * Overridden here in user code so CubeMX regeneration keeps it.
   */
#if DCMI_SOURCE_MODE == 1U
  /* Stock TFP401 EDID 800x480: both sync pulses negative -> blanking LOW. */
  DCMI->CR &= ~(DCMI_CR_VSPOL | DCMI_CR_HSPOL);
#elif DCMI_SOURCE_MODE == 2U
  /* AMBILIGHT 720p50-RB EDID: HSync positive, VSync negative. */
  DCMI->CR &= ~DCMI_CR_VSPOL;
#endif

  /* USER CODE END DCMI_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 104;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
  /* DMA2_Stream1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
  /* DCMI peripheral IRQ reports sync/overrun/error conditions separately
   * from the DMA stream IRQ.
   */
  HAL_NVIC_SetPriority(DCMI_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DCMI_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(CS_I2C_SPI_GPIO_Port, CS_I2C_SPI_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, LD4_Pin|LD3_Pin|LD5_Pin|LD6_Pin
                          |Audio_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : CS_I2C_SPI_Pin */
  GPIO_InitStruct.Pin = CS_I2C_SPI_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(CS_I2C_SPI_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PDM_OUT_Pin */
  GPIO_InitStruct.Pin = PDM_OUT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(PDM_OUT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_EVT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : SPI1_SCK_Pin SPI1_MOSI_Pin */
  GPIO_InitStruct.Pin = SPI1_SCK_Pin|SPI1_MOSI_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : BOOT1_Pin */
  GPIO_InitStruct.Pin = BOOT1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(BOOT1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : CLK_IN_Pin */
  GPIO_InitStruct.Pin = CLK_IN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(CLK_IN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PB12 PB13 PB14 PB15 */
  GPIO_InitStruct.Pin = GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : LD4_Pin LD3_Pin LD5_Pin LD6_Pin
                           Audio_RST_Pin */
  GPIO_InitStruct.Pin = LD4_Pin|LD3_Pin|LD5_Pin|LD6_Pin
                          |Audio_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pins : I2S3_SCK_Pin I2S3_SD_Pin */
  GPIO_InitStruct.Pin = I2S3_SCK_Pin|I2S3_SD_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF6_SPI3;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : Audio_SDA_Pin */
  GPIO_InitStruct.Pin = Audio_SDA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(Audio_SDA_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : MEMS_INT2_Pin */
  GPIO_InitStruct.Pin = MEMS_INT2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_EVT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(MEMS_INT2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
