
/**
 * @file int_bootloader.c
 * @brief Bootloader串口接收与Flash写入实现
 * @details 实现通过串口空闲中断接收应用程序，分包写入Flash，并支持超时自动跳转
 *          完整支持奇偶长度数据的正确拼接，确保Flash写入的完整性
 */

#include "Int_bootloader.h"

// 全局变量定义
uint8_t g_uart_rec_buff[BOOTLOADER_UART_REC_BUFF_LEN] = {0}; // 串口接收缓冲区（最大512字节）
uint16_t g_uart_rec_len = 0;                                 // 当前接收帧的数据长度
uint16_t g_uart_rec_full_len = 0;                            // 累计接收的总数据长度
uint32_t g_uart_rec_offset = 0;                              // Flash写入偏移量（相对于APP起始地址）
uint8_t g_last_byte_flag = 0;                                // 遗留单字节标记（1=有遗留，0=无遗留）
uint8_t g_last_byte = 0;                                     // 保存遗留的单字节（用于奇偶长度拼接）
uint8_t uart_rx_finish = 0;                                  // 接收完成标志位（1=有新数据待处理）
uint32_t last_receive_time = 0;                              // 最后一次接收时间戳（用于超时检测）

/**
 * @brief Flash页擦除函数
 * @details 根据即将写入的数据判断是否需要擦除目标Flash页
 *          只有当目标地址不全为0xff时才执行擦除操作，避免不必要的擦除
 */
void Int_flash_erase(void)
{
    uint8_t is_erase = 0;   // 是否需要擦除标志
    uint32_t page_addr = 0; // 待擦除的页地址

    // 遍历当前接收帧的所有字节，检查目标Flash地址是否全为0xff
    for (uint16_t i = 0; i < g_uart_rec_len; i++)
    {
        // 计算目标Flash地址
        volatile uint8_t *data = (volatile uint8_t *)(APP_START_ADDRESS + i + g_uart_rec_offset);

        // 如果发现非0xff字节，说明需要擦除
        if (*data != 0xff)
        {
            is_erase = 1;
            // 计算页起始地址（实现页对齐）
            page_addr = (APP_START_ADDRESS + i + g_uart_rec_offset) -
                        ((APP_START_ADDRESS + i + g_uart_rec_offset) % FLASH_PAGE_SIZE);
            break; // 找到一个非0xff字节即可退出
        }
    }

    // 如果需要擦除，则执行Flash页擦除操作
    if (is_erase)
    {
        FLASH_EraseInitTypeDef erase_init;
        erase_init.TypeErase = FLASH_TYPEERASE_PAGES; // 擦除类型：页擦除
        erase_init.Banks = FLASH_BANK_1;              // 选择Bank 1（STM32F103只有一个Bank）
        erase_init.PageAddress = page_addr;           // 擦除起始地址
        erase_init.NbPages = 1;                       // 擦除页数：1页
        uint32_t page_error = 0;                      // 擦除错误码
        HAL_FLASHEx_Erase(&erase_init, &page_error);  // 执行页擦除
    }
}
/**
 * @brief 带遗留字节的Flash写入函数
 * @details 将上次遗留的单字节与本次数据拼接后以半字为单位写入Flash
 *          适用于上次接收数据长度为奇数，遗留了一个字节的情况
 */
void Int_flash_write_with_last(void)
{
    // 以半字（2字节）为单位遍历接收缓冲区
    for (uint16_t i = 0; i < g_uart_rec_len; i += 2)
    {
        uint16_t data16;
        uint32_t flash_addr = APP_START_ADDRESS + i + g_uart_rec_offset;

        if (i == 0) // 第一个半字：需要与上次遗留的字节拼接
        {
            // 高字节：当前缓冲区第一个字节，低字节：上次遗留的字节
            data16 = (g_uart_rec_buff[i] << 8) | g_last_byte;
        }
        else // 后续半字：直接拼接当前缓冲区的两个连续字节
        {
            data16 = (uint16_t)(g_uart_rec_buff[i - 1] | g_uart_rec_buff[i] << 8);
        }

        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr, data16);
    }
}

/**
 * @brief 无遗留字节的Flash写入函数
 * @details 直接将数据以半字为单位写入Flash
 *          适用于数据长度为偶数，无需拼接遗留字节的情况
 */
void Int_flash_write_no_last(void)
{
    // 以半字（2字节）为单位遍历接收缓冲区
    for (uint16_t i = 0; i < g_uart_rec_len; i += 2)
    {
        uint16_t data16;
        uint32_t flash_addr = APP_START_ADDRESS + i + g_uart_rec_offset;

        // 确保不越界访问
        if (i + 1 < g_uart_rec_len)
        {
            // 高字节：下一个字节，低字节：当前字节（小端序）
            data16 = (uint16_t)(g_uart_rec_buff[i + 1] << 8 | g_uart_rec_buff[i]);
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr, data16);
        }
    }
}

/**
 * @brief Flash写入调度函数
 * @details 根据当前接收数据长度和遗留字节标志，选择合适的写入方式
 *          处理奇偶长度数据的正确拼接，确保数据完整性
 */
void Int_flash_write_halfword(void)
{
    // 计算总长度（当前帧长度 + 遗留字节数）的奇偶性
    if ((g_uart_rec_len + g_last_byte_flag) % 2 == 0)
    {
        // 总长度为偶数的情况
        if (g_last_byte_flag)
        {
            // 有遗留字节，且当前帧长度为奇数
            Int_flash_write_with_last();
            g_uart_rec_offset += g_uart_rec_len + 1; // 更新偏移量（包含遗留字节）
        }
        else
        {
            // 无遗留字节，且当前帧长度为偶数
            Int_flash_write_no_last();
            g_uart_rec_offset += g_uart_rec_len; // 更新偏移量
        }
        g_last_byte_flag = 0; // 清除遗留字节标志
    }
    else
    {
        // 总长度为奇数的情况
        if (g_last_byte_flag)
        {
            // 有遗留字节，且当前帧长度为偶数
            Int_flash_write_with_last();
            g_last_byte = g_uart_rec_buff[g_uart_rec_len - 1]; // 保存最后一个字节
            g_uart_rec_offset += g_uart_rec_len;
        }
        else
        {
            // 无遗留字节，且当前帧长度为奇数
            Int_flash_write_no_last();
            g_last_byte = g_uart_rec_buff[g_uart_rec_len - 1]; // 保存最后一个字节
            g_uart_rec_offset += g_uart_rec_len - 1;
        }
        g_last_byte_flag = 1; // 设置遗留字节标志
    }
}

/**
 * @brief Bootloader初始化函数
 * @details 初始化串口空闲中断接收，清除标志位，避免启动前接收数据导致溢出
 *          调用后开始监听串口数据
 */
void Int_bootloader_init(void)
{
    // 清除串口溢出标志（ORE）和空闲帧标志（IDLE）
    // 确保启动前串口状态干净，避免误触发中断
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    // 启动串口空闲中断接收
    // 当串口空闲或缓冲区满时触发中断，回调函数为 HAL_UARTEx_RxEventCallback
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, g_uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
}
/**
 * @brief 串口空闲中断回调函数
 * @param huart UART句柄指针
 * @param Size 本次接收到的数据长度
 * @details 串口空闲中断触发时调用，保存接收数据长度并设置标志位
 *          重新启动接收以继续监听下一包数据
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    // 判断是否为USART1的中断
    if (huart->Instance == USART1)
    {
        // 保存本次接收的数据长度
        g_uart_rec_len = Size;
        // 累加总接收长度（用于进度显示和完整性校验）
        g_uart_rec_full_len += g_uart_rec_len;

        // 设置接收完成标志位，通知主循环处理数据
        uart_rx_finish = 1;

        // 更新最后接收时间戳（用于超时检测）
        last_receive_time = HAL_GetTick();

        // 重新启动串口空闲中断接收（必须重新调用以继续监听）
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, g_uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
    }
}

/**
 * @brief 跳转到应用程序函数
 * @details 执行从Bootloader到Application的完整跳转流程
 *          包括：地址校验、关闭中断、设置堆栈指针、重定向中断向量表、跳转
 */
void Int_bootloader_jump_to_app(void)
{
    // 定义函数指针类型，指向无参数无返回值的函数
    typedef void (*pFunc)(void);

    // 从APP起始地址读取栈顶指针（ARM Cortex-M架构：地址0存放栈顶地址）
    uint32_t app_stack_top = *(volatile uint32_t *)(APP_START_ADDRESS);
    // 从APP起始地址+4读取复位向量地址（地址4存放复位中断向量）
    uint32_t app_reset_interrupt = *(volatile uint32_t *)(APP_START_ADDRESS + 4);

    // ========== 1. 地址校验 ==========
    // 1.1 校验栈顶地址是否在有效RAM区域
    if ((app_stack_top & 0xffff0000) != APP_TOP_ADDR)
    {
        printf("stack addr error\n");
        return; // 栈顶地址无效，不执行跳转
    }
    // 1.2 校验复位中断地址是否在有效Flash区域
    if (app_reset_interrupt < APP_START_ADDRESS || app_reset_interrupt > APP_END_ADDR)
    {
        printf("reset interrupt addr error\n");
        return; // 复位地址无效，不执行跳转
    }

    // ========== 2. 关闭中断并准备跳转 ==========
    // 2.1 关闭所有中断
    __disable_irq();

    // 2.2 设置主堆栈指针（MSP）为APP的栈顶地址
    __set_MSP(app_stack_top);

    // 2.3 重定向中断向量表基址到APP起始地址
    SCB->VTOR = APP_START_ADDRESS;

    // ========== 3. 执行跳转 ==========
    // 将复位中断地址转换为函数指针并调用
    pFunc jump_to_app = (pFunc)app_reset_interrupt;
    jump_to_app();
}
