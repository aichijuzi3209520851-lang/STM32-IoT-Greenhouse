/**
  ******************************************************************************
  * @file    System/Delay.c
  * @brief   延时与系统时基库(裸机优化版)
  * @version V1.2
  * @date    2026-10-25
  * @note    微秒延时: DWT->CYCCNT周期计数器轮询, 不触碰SysTick,
  *          中断照常响应, 可在临界时序(DHT11)中安全使用;
  *          毫秒时基: SysTick 1ms中断累加 g_ms_tick, 供主循环分时隙调度
  * @history
  *   V1.0  2026-09-20  首次发布(注释规范化)
  *   V1.2  2026-10-25  Delay_us改DWT实现(原SysTick方案与1ms时基冲突);
  *                     新增Delay_TickInit毫秒时基与Delay_GetTick接口
  ******************************************************************************
  */
#include "stm32f10x.h"
#include "Delay.h"

/* ==================== DWT/CoreDebug寄存器定义 ====================
 * 旧版CMSIS(V3.5标准库自带core_cm3.h)未定义DWT/CoreDebug,
 * 此处按Cortex-M3系统地址直接定义, 不依赖CMSIS版本 */
#define DEMCR_REG          (*(volatile unsigned int *)0xE000EDFCU)  /* 调试异常与复用控制 */
#define DEMCR_TRCENA_BIT   (1UL << 24)                              /* DWT使能位 */
#define DWT_CTRL_REG       (*(volatile unsigned int *)0xE0001000U)  /* DWT控制寄存器 */
#define DWT_CYCCNT_REG     (*(volatile unsigned int *)0xE0001004U)  /* 周期计数器 */
#define DWT_CYCCNTENA_BIT  (1UL << 0)                                /* CYCCNT使能位 */

/* ==================== 毫秒时基(仅裸机版使用) ==================== */
static volatile uint32_t g_ms_tick = 0;  // 系统毫秒计数(SysTick中断累加)

/**
  * @brief  SysTick 1ms中断服务回调(由stm32f10x_it.c的SysTick_Handler调用)
  * @param  无
  * @retval 无
  */
void Delay_OnSysTickISR(void)
{
    g_ms_tick++;
}

/**
  * @brief  初始化延时库: 使能DWT周期计数器 + 配置SysTick 1ms中断时基
  * @param  无
  * @retval 无
  * @note   必须在SystemCoreClock配置为72MHz之后调用(工程已在启动阶段完成)
  */
void Delay_Init(void)
{
    /* ---- DWT使能: 微秒延时依赖CYCCNT ---- */
    DEMCR_REG |= DEMCR_TRCENA_BIT;     // 开放DWT外设
    DWT_CYCCNT_REG = 0;                // 清零周期计数器
    DWT_CTRL_REG |= DWT_CYCCNTENA_BIT; // 启动CYCCNT

    /* ---- SysTick 1ms时基: 72MHz/72000 = 1kHz ---- */
    SysTick_Config(SystemCoreClock / 1000);
}

/**
  * @brief  获取系统毫秒节拍
  * @param  无
  * @retval 自上电起的毫秒数(约49.7天回绕, 差值运算安全)
  */
uint32_t Delay_GetTick(void)
{
    return g_ms_tick;
}

/**
  * @brief  判断节拍是否已超时(回绕安全)
  * @param  start: 起始节拍(Delay_GetTick返回值)
  * @param  ms:     时长(毫秒)
  * @retval 1:已超时 0:未超时
  */
uint8_t Delay_IsTimeout(uint32_t start, uint32_t ms)
{
    return ((uint32_t)(Delay_GetTick() - start) >= ms) ? 1 : 0;
}

/**
  * @brief  微秒级延时(DWT实现, 不触碰SysTick, 中断照常响应)
  * @param  xus 延时时长(72MHz下每微秒72个周期)
  * @retval 无
  * @note   调用前须完成Delay_Init(); 精度±数个周期
  */
void Delay_us(uint32_t xus)
{
    uint32_t start  = DWT_CYCCNT_REG;
    uint32_t cycles = xus * (SystemCoreClock / 1000000U);  // 72 cycles/us
    while ((DWT_CYCCNT_REG - start) < cycles)
    {
        /* 忙等, 但不关闭任何中断/定时器 */
    }
}

/**
  * @brief  毫秒级延时(阻塞, 中断与DHT11无关; 长延时会喂狗式空转)
  * @param  xms 延时时长
  * @retval 无
  */
void Delay_ms(uint32_t xms)
{
    while (xms--)
    {
        Delay_us(1000);
    }
}

/**
  * @brief  秒级延时
  * @param  xs 延时时长
  * @retval 无
  */
void Delay_s(uint32_t xs)
{
    while (xs--)
    {
        Delay_ms(1000);
    }
}
