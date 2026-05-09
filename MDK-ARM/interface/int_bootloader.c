
/**
 * @file/int_bootloader.c
 * @brief Bootloader串口接收与Flash写入实现
 * @details 实现通过串口接收应用程序并写入Flash的功能
 */

#include "Int_bootloader.h"

// 全局变量定义
uint8_t g_uart_rec_buff[BOOTLOADER_UART_REC_BUFF_LEN] = {0}; // 串口接收缓冲区
uint16_t g_uart_rec_len = 0;                                 // 当前接收数据长度
uint16_t g_uart_rec_full_len = 0;                            // 累计接收数据长度
uint32_t g_uart_rec_offset = 0;                              // Flash写入偏移量
uint8_t g_last_byte_flag = 0;                                // 是否有遗留单字节标记
uint8_t g_last_byte = 0;                                     // 保存遗留的单字节
uint8_t uart_rx_finish = 0;                                  // 接收完成标志位
uint32_t last_receive_time = 0;

void Int_flash_erase(void)
{
    // 判断是否需要擦除Flash页（检测目标地址是否全为0xff）
    uint8_t is_erase = 0;
    uint32_t page_addr = 0;
    for (uint16_t i = 0; i < g_uart_rec_len; i++)
    {
        volatile uint8_t *data = (volatile uint8_t *)(APP_START_ADDRESS + i + g_uart_rec_offset);
        if (*data != 0xff)
        {

            is_erase = 1;
            // 计算页起始地址（页对齐）
            page_addr = (APP_START_ADDRESS + i + g_uart_rec_offset) -
                        ((APP_START_ADDRESS + i + g_uart_rec_offset) % FLASH_PAGE_SIZE);
            break;
        }
    }

    // 执行Flash页擦除
    if (is_erase)
    {
        FLASH_EraseInitTypeDef erase_init;
        erase_init.TypeErase = FLASH_TYPEERASE_PAGES; // 页擦除
        erase_init.Banks = FLASH_BANK_1;              // 选择Bank 1
        erase_init.PageAddress = page_addr;           // 擦除起始地址
        erase_init.NbPages = 1;                       // 擦除1页
        uint32_t page_error = 0;
        HAL_FLASHEx_Erase(&erase_init, &page_error);
    }
}
/**
 * @brief 带遗留字节的Flash写入函数
 * @details 将上次遗留的单字节与本次数据拼接后写入Flash
 */
void Int_flash_write_with_last(void)
{
    for (uint16_t i = 0; i < g_uart_rec_len; i += 2)
    {
        uint16_t data16;
        uint32_t flash_addr = APP_START_ADDRESS + i + g_uart_rec_offset;

        if (i == 0) // 第一个半字：拼接遗留字节与当前第一个字节
        {
            // data16 = g_last_byte | (g_uart_rec_buff[i] << 8);
            data16 = (g_uart_rec_buff[i] << 8) | g_last_byte;
        }
        else // 后续半字：拼接当前字节与前一字节
        {
            data16 = (uint16_t)(g_uart_rec_buff[i - 1] | g_uart_rec_buff[i] << 8);
        }

        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr, data16);
    }
}

/**
 * @brief 无遗留字节的Flash写入函数
 * @details 直接将数据以半字为单位写入Flash
 */
void Int_flash_write_no_last(void)
{
    for (uint16_t i = 0; i < g_uart_rec_len; i += 2)
    {
        uint16_t data16;
        uint32_t flash_addr = APP_START_ADDRESS + i + g_uart_rec_offset;
        if (i + 1 < g_uart_rec_len)
        {
            // data16 = (uint16_t)(g_uart_rec_buff[i] << 8 | g_uart_rec_buff[i + 1]);
            data16 = (uint16_t)(g_uart_rec_buff[i + 1] << 8 | g_uart_rec_buff[i]);
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr, data16);
        }
    }
}

void Int_flash_write_halfword(void)
{
    // 根据数据长度奇偶性选择写入方式
    if ((g_uart_rec_len + g_last_byte_flag) % 2 == 0)
    {
        if (g_last_byte_flag)
        {
            // 有遗留字节，且总长度为偶数
            Int_flash_write_with_last();
            g_uart_rec_offset += g_uart_rec_len + 1;
        }
        else
        {
            // 无遗留字节，且长度为偶数
            Int_flash_write_no_last();
            g_uart_rec_offset += g_uart_rec_len;
        }
        g_last_byte_flag = 0;
    }
    else
    {
        if (g_last_byte_flag)
        {
            // 有遗留字节，且总长度为奇数
            Int_flash_write_with_last();
            g_last_byte = g_uart_rec_buff[g_uart_rec_len - 1];
            g_uart_rec_offset += g_uart_rec_len;
        }
        else
        {
            // 无遗留字节，且长度为奇数
            Int_flash_write_no_last();
            g_last_byte = g_uart_rec_buff[g_uart_rec_len - 1];
            g_uart_rec_offset += g_uart_rec_len - 1;
        }
        g_last_byte_flag = 1;
    }
}

/**
 * @brief Bootloader初始化函数
 * @details 初始化串口中断接收，清除标志位，避免启动前接收数据导致溢出
 */

void Int_bootloader_init(void)
{
    // 清除串口溢出标志和空闲帧标志
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    // 启动串口空闲中断接收
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, g_uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
}
/**
 * @brief 串口空闲中断回调函数
 * @param huart UART句柄
 * @param Size 接收数据长度
 * @details 接收完成后将数据写入Flash，处理奇偶长度数据的拼接
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        // 保存本次接收的数据长度
        g_uart_rec_len = Size;
        g_uart_rec_full_len += g_uart_rec_len;

        uart_rx_finish = 1; // 只打标志！

        // 更新最后接收时间
        last_receive_time = HAL_GetTick();

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, g_uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
    }
}

// 跳转到app
void Int_bootloader_jump_to_app()
{

    typedef void (*pFunc)(void);

    uint32_t app_stack_top = *(volatile uint32_t *)(APP_START_ADDRESS);
    uint32_t app_reset_interrupt = *(volatile uint32_t *)(APP_START_ADDRESS + 4);
    // 1.1校验栈顶地址
    if ((app_stack_top & 0xffff0000) != APP_TOP_ADDR)
    {
        printf("stack addr error\n");
        return;
    }
    // 1.2 校验复位中断地址
    if (app_reset_interrupt < APP_START_ADDRESS || app_reset_interrupt > APP_END_ADDR)
    {
        printf("reset interrupt addr error\n");
        return;
    }

    // 2.注销bootloader中断
    //  2.1 关闭中断
    __disable_irq();

    // 2.2 设置堆栈指针
    __set_MSP(app_stack_top);

    // 2.3 重定向中断向量表
    SCB->VTOR = APP_START_ADDRESS;

    // 2.4跳转到A程序复位中断地址
    pFunc jump_to_app = (pFunc)app_reset_interrupt;
    jump_to_app();
}
