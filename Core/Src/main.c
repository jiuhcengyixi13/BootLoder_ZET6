

/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Bootloader主程序入口
 * @details        : 实现Bootloader的核心流程：
 *                   1. 初始化HAL库、系统时钟、GPIO、UART
 *                   2. 启动串口空闲中断接收
 *                   3. 主循环处理：分包接收→Flash写入→超时跳转
 *                   支持通过串口接收应用程序并自动跳转到应用
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
#include "main.h"           // 主程序头文件（HAL库、外设声明等）
#include "usart.h"          // UART驱动头文件（串口初始化和操作）
#include "gpio.h"           // GPIO驱动头文件（LED等外设控制）
#include "int_bootloader.h" // Bootloader接口头文件（Flash操作、跳转函数）
#include "string.h"         // 字符串操作函数（memset等）
#include "stdio.h"          // 标准输入输出（printf重定向到串口）

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// 外部变量声明（定义在 int_bootloader.c 中）
extern uint8_t g_uart_rec_buff[];    // 串口接收缓冲区（最大512字节）
extern uint16_t g_uart_rec_len;      // 当前接收帧的数据长度
extern uint16_t g_uart_rec_full_len; // 累计接收的总数据长度
extern uint32_t g_uart_rec_offset;   // Flash写入偏移量（相对于APP起始地址）
extern uint8_t g_last_byte_flag;     // 遗留单字节标记（1=有遗留，0=无遗留）
extern uint8_t g_last_byte;          // 遗留的单字节（用于奇偶长度拼接）
extern uint8_t uart_rx_finish;       // 接收完成标志位（1=有新数据待处理）
extern uint32_t last_receive_time;   // 最后接收时间戳（用于超时检测）
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void); // 系统时钟配置函数（72MHz）
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @details 程序入口函数，执行初始化并进入主循环
 * @retval int
 */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init(); // 初始化HAL库（包括Flash接口、SysTick定时器、NVIC等）

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config(); // 配置系统时钟为72MHz（使用HSE外部晶振）

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();        // 初始化GPIO（包括LED引脚配置）
  MX_USART1_UART_Init(); // 初始化USART1（115200波特率，8N1）

  /* USER CODE BEGIN 2 */

  // 启动Bootloader串口空闲中断接收
  // 调用后开始监听串口数据，当串口空闲或缓冲区满时触发中断
  Int_bootloader_init();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) // 主循环
  {
    /* USER CODE END WHILE */

    // ========== 串口数据处理 ==========
    if (uart_rx_finish == 1) // 检测是否有新数据待处理
    {
      // Flash操作流程：解锁→擦除→写入→锁定
      HAL_FLASH_Unlock();         // 解锁Flash（允许写入操作）
      Int_flash_erase();          // 判断并擦除目标Flash页（按需擦除）
      Int_flash_write_halfword(); // 将数据写入Flash（处理奇偶长度拼接）
      HAL_FLASH_Lock();           // 锁定Flash（保护数据不被意外修改）

      // 清空接收缓冲区，准备接收下一包数据
      memset(g_uart_rec_buff, 0, BOOTLOADER_UART_REC_BUFF_LEN);

      // 清除接收完成标志，等待下一次中断
      uart_rx_finish = 0;
    }

    // ========== 接收进度显示 ==========
    static uint16_t last_len = 0;        // 静态变量，记录上次打印的长度
    if (g_uart_rec_full_len != last_len) // 只有当数据长度变化时才打印
    {
      printf("Received: %d bytes\n", g_uart_rec_full_len); // 打印当前接收进度
      last_len = g_uart_rec_full_len;                      // 更新上次打印的长度
    }

// ========== 超时检测与跳转 ==========
#define RECEIVE_TIMEOUT_MS 2000                                   // 超时时间：2秒（无新数据即认为接收完成）
    if (g_uart_rec_full_len > 0 &&                                // 条件1：已接收数据
        (HAL_GetTick() - last_receive_time) > RECEIVE_TIMEOUT_MS) // 条件2：超过超时时间
    {
      printf("Receive complete! Total: %d bytes\n", g_uart_rec_full_len);
      printf("Jumping to application...\n");
      HAL_Delay(500);               // 等待串口发送完成（避免数据丢失）
      Int_bootloader_jump_to_app(); // 跳转到应用程序（只执行一次）
    }

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @details 配置系统时钟为72MHz，使用HSE外部晶振作为时钟源
 * @retval None
 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
 * @brief  This function is executed in case of error occurrence.
 * @details 错误处理函数，发生致命错误时进入死循环
 * @retval None
 */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq(); // 关闭所有中断
  while (1)        // 死循环，防止程序继续执行
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
