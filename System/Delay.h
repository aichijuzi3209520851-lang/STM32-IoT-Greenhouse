/**
  ******************************************************************************
  * @file    System/Delay.h
  * @brief   延时与系统时基库头文件(裸机优化版)
  * @version V1.2
  * @date    2026-10-25
  ******************************************************************************
  * @history
  *   V1.0  2026-09-20  首次发布(注释规范化)
  *   V1.2  2026-10-25  新增Delay_Init/Delay_GetTick/Delay_IsTimeout接口
  ******************************************************************************
  */
#ifndef __DELAY_H
#define __DELAY_H
#include "stdint.h"

void     Delay_Init(void);                     /* DWT使能 + SysTick 1ms时基 */
void     Delay_OnSysTickISR(void);             /* SysTick中断回调(it.c调用) */
uint32_t Delay_GetTick(void);                  /* 毫秒节拍 */
uint8_t  Delay_IsTimeout(uint32_t start, uint32_t ms);  /* 回绕安全超时判断 */
void     Delay_us(uint32_t us);
void     Delay_ms(uint32_t ms);
void     Delay_s(uint32_t s);

#endif
