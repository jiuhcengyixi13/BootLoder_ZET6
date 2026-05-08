#ifndef __INT_BOOTLADER_H
#define __INT_BOOTLADER_H

#include "usart.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define BOOTLOADER_UART_REC_BUFF_LEN 512

// 串口接受->准备接受A程序

//程序写入的起始位置=>A区的的起始地址 假设B区16k A区512-16k 0x7c000总大小
#define APP_START_ADDRESS 0x08004000

void Int_bootloader_init(void);
void Int_flash_erase(void);
void Int_flash_write_halfword(void);
#endif
