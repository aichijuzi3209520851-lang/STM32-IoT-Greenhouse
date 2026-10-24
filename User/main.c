/**
  ******************************************************************************
  * @file    User/main.c
  * @brief   智能温室环境监控系统主程序(裸机优化版)
  * @version V1.2
  * @date    2026-10-25
  * @note    输入: DHT11温湿度 / 土壤湿度(D2-5-2K) / 烟雾传感器 / 双串口指令
  *          输出: OLED显示 / 多LED+蜂鸣器报警 / 电机与舵机联动
  *          架构: SysTick 1ms时基 + 主循环分时隙调度(前后台系统)
  *            - 串口指令: 每圈检查, 响应<几毫秒(原轮询版最坏1.5s)
  *            - 报警联动: 每圈判定, 即时响应
  *            - 烟雾消抖: 50ms双采样状态机, 非阻塞(原版阻塞Delay_ms(50))
  *            - 传感器采集+串口打印: 1.5s周期时隙(协议与旧版逐字节一致)
  ******************************************************************************
  * @history
  *   V1.0  2026-09-20  注释规范化; 水位传感器替换为D2-5-2K土壤湿度模块
  *   V1.1  2026-10-25  报警阈值宏化统一(三端一致); 双串口指令解析合并
  *   V1.2  2026-10-25  裸机优化版: 分时隙调度+非阻塞消抖+即时命令响应
  *                     (本分支bare-metal-opt专用, 主线main已迁移FreeRTOS)
  ******************************************************************************
  */
//说明:
//	VCC--3.3V/5V
//	GND--GND
//	DATA--PA15(接DHT11)
//	Soil Sensor AO--PA0(ADC通道)  (D2-5-2K电容式,读数与湿度成反比)
//	Smoke DOUT--PA6
//	红灯LED--PB10(烟雾报警) [占用USART3_TX;原PA1与舵机TIM2_CH2冲突]
//  黄灯LED--PB0(温湿度报警)
//  蓝灯LED--PA3(土壤过湿报警)
//  烟雾报警LED--PB4(烟雾浓度报警)
//  蜂鸣器--PA7

#include "sys.h"
#include "Delay.h"
#include "DHT11.h"
#include "usart.h"
#include "OLED.h"
#include "Water.h"  // 土壤湿度传感器驱动头文件(D2-5-2K)
#include "Motor.h"
#include "Servo.h"
#include <string.h>  // 字符串函数库

/* ==================== 硬件引脚定义 ==================== */
#define SMOKE_DOUT PAin(6)   // 烟雾传感器输出PA6
#define RED_LED PBout(10)    // 红灯LED(烟雾报警,改接PB10,舍弃USART3_TX)
#define YELLOW_LED PBout(0)  // 黄灯LED(温湿度报警)
#define BLUE_LED PAout(3)    // 蓝灯LED(土壤过湿报警)
#define SMOKE_ALARM_LED PBout(4)  // 烟雾浓度报警LED(PB4)
#define BEEP PAout(7)        // 蜂鸣器PA7

/* ==================== 统一报警阈值 ====================
 * 三端(STM32固件/PC上位机/微信小程序)使用同一组数值,
 * 固件为权威判定源: 本地毫秒级联动, 上位机/小程序仅做展示 */
#define ALARM_TEMP_MAX   26   // 温度报警上限(°C)
#define ALARM_HUMI_MAX   85   // 空气湿度报警上限(%RH)
#define ALARM_SOIL_MAX   40   // 土壤湿度报警上限(%, 超过视为过湿)

/* ==================== 调度时隙配置 ==================== */
#define SLOT_SENSOR_PERIOD_MS   1500  // 传感器采集周期(与旧版1.5s一致)
#define SMOKE_DEBOUNCE_MS       50    // 烟雾消抖确认时长

/* ==================== 全局状态变量 ==================== */
u8 temperature;  // 温度变量
u8 humidity;     // 湿度变量
uint16_t water_level;  // 土壤湿度原始ADC值(0~4095, 越干越大)
uint8_t water_percent; // 土壤湿度百分比(0~100, 越大越湿)
char air_status[10] = "nice";  // 空气质量状态
uint8_t servo_angle = 0;  // 舵机当前角度状态(0或180)
uint8_t manual_mode = 0;  // 手动控制模式(1=串口手动控制,0=自动报警控制)
uint8_t motor_status = 0;  // 电机状态(0=停止,1=运行)

/* ==================== 烟雾消抖状态机(非阻塞) ==================== */
static uint8_t  smoke_pending = 0;      // 候选报警状态(首次采到低电平置1)
static uint32_t smoke_pending_tick = 0; // 候选开始时刻

/**
  * @brief  初始化所有报警相关硬件(传感器/报警LED/蜂鸣器)
  */
void Alarm_Hardware_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    // 烟雾传感器(PA6)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // 红灯LED(PB10)——注意PB10=USART3_TX,串口3将不能发送
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // 黄灯LED(PB0)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // 烟雾浓度报警LED(PB4)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // 蓝灯LED(PA3)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // 蜂鸣器(PA7)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // 初始状态:LED熄灭,蜂鸣器关闭
    RED_LED = 1;
    YELLOW_LED = 1;
    BLUE_LED = 1;
    SMOKE_ALARM_LED = 1;
    BEEP = 0;
}

/**
  * @brief  烟雾检测(50ms双采样消抖, 非阻塞)
  * @note   连续50ms低电平才判定bad; 高电平立即恢复nice并复位状态机
  */
void Check_Air_Quality(void)
{
    if (SMOKE_DOUT == 0)
    {
        if (smoke_pending == 0)
        {
            smoke_pending = 1;                          // 记录候选起点
            smoke_pending_tick = Delay_GetTick();
        }
        else if (Delay_IsTimeout(smoke_pending_tick, SMOKE_DEBOUNCE_MS))
        {
            strcpy(air_status, "bad");                  // 持续低电平满50ms
        }
    }
    else
    {
        smoke_pending = 0;                              // 复位消抖状态机
        strcpy(air_status, "nice");
    }
}

/**
  * @brief  解析一条完整的串口指令帧(USART1/USART3共用同一入口)
  * @param  buf: 指令帧缓冲区(不含帧尾\r\n)
  * @param  len: 指令长度
  */
static void Parse_Command_Frame(const char *buf, u16 len)
{
    if(len >= 6 && strncmp(buf, "mode-0", 6) == 0)
    {
        manual_mode = 0;
    }
    else if(len >= 6 && strncmp(buf, "mode-1", 6) == 0)
    {
        manual_mode = 1;
    }
    else if(manual_mode)
    {
        if(len >= 6 && strncmp(buf, "DUOJ-0", 6) == 0)
        {
            Servo_SetAngle(0);
            servo_angle = 0;
        }
        else if(len >= 6 && strncmp(buf, "DUOJ-1", 6) == 0)
        {
            Servo_SetAngle(180);
            servo_angle = 180;
        }
        else if(len >= 7 && strncmp(buf, "MOTOR-0", 7) == 0)
        {
            Motor_SetSpeed(0);
            motor_status = 0;
        }
        else if(len >= 7 && strncmp(buf, "MOTOR-1", 7) == 0)
        {
            Motor_SetSpeed(100);
            motor_status = 1;
        }
    }
}

/**
  * @brief  处理USART1/USART3的串口指令(每圈调用, 毫秒级响应)
  */
void Process_Serial_Command(void)
{
    if(USART_RX_STA & 0x8000)
    {
        u16 len = USART_RX_STA & 0x3FFF;
        Parse_Command_Frame((const char*)USART_RX_BUF, len);
        USART_RX_STA = 0;
    }

    if(USART3_RX_STA & 0x8000)
    {
        u16 len = USART3_RX_STA & 0x3FFF;
        Parse_Command_Frame((const char*)USART3_RX_BUF, len);
        USART3_RX_STA = 0;
    }
}

/**
  * @brief  处理报警逻辑(每圈调用, 即时联动)
  * @retval 1:温湿度报警触发; 0:未触发
  */
uint8_t Handle_Alarms(void)
{
    uint8_t temp_humi_alarm = (temperature > ALARM_TEMP_MAX || humidity > ALARM_HUMI_MAX) ? 1 : 0;
    uint8_t water_alarm = (water_percent > ALARM_SOIL_MAX) ? 1 : 0;
    uint8_t smoke_alarm = (strcmp(air_status, "bad") == 0) ? 1 : 0;

    YELLOW_LED = temp_humi_alarm ? 0 : 1;
    BLUE_LED = water_alarm ? 0 : 1;
    RED_LED = smoke_alarm ? 0 : 1;
    SMOKE_ALARM_LED = smoke_alarm ? 0 : 1;
    BEEP = (temp_humi_alarm || water_alarm || smoke_alarm) ? 0 : 1;

    if (!manual_mode)
    {
        if (smoke_alarm)
        {
            Motor_SetSpeed(100);
            motor_status = 1;
        }
        else
        {
            Motor_SetSpeed(0);
            motor_status = 0;
        }

        if (temp_humi_alarm)
        {
            Servo_SetAngle(180);
            servo_angle = 180;
        }
        else
        {
            Servo_SetAngle(0);
            servo_angle = 0;
        }
    }

    return temp_humi_alarm;
}

/**
  * @brief  传感器采集时隙: DHT11+土壤湿度+OLED数值刷新+串口协议输出
  * @note   1.5s周期执行; 串口文本与V1.1逐字节一致(上位机零改动)
  */
static void Sensor_Acquire_Slot(void)
{
    uint8_t temp_humi_alarm_status;

    /* ---- DHT11(阻塞约25ms, DWT延时, 中断照常) ---- */
    if(DHT11_Read_Data(&temperature, &humidity) == 0)
    {
        printf("Temp:%dC   Humi:%d%%RH   ", temperature, humidity);
        OLED_ShowNum(1, 6, temperature, 2);
        OLED_ShowNum(2, 6, humidity, 2);
        OLED_ShowString(4, 5, "       ");
    }
    else
    {
        printf("DHT11 Read Failed!   ");
        OLED_ShowString(4, 5, "Error!");
    }

    /* ---- 土壤湿度 ---- */
    water_level = ADC1_GetValue(ADC_Channel_0);
    water_percent = Soil_GetMoisturePercent();
    printf("Soil:%d%%(raw=%d)  ", water_percent, water_level);
    OLED_ShowNum(3, 12, water_percent, 3);

    /* ---- 报警判定与执行器联动(即时) ---- */
    temp_humi_alarm_status = Handle_Alarms();

    /* ---- 协议尾行(与旧版一致) ---- */
    printf("Servo:%s Motor:%s ",
           (servo_angle == 180) ? "ON " : "OFF",
           (motor_status == 1) ? "ON " : "OFF");

    if (temp_humi_alarm_status || (water_percent > ALARM_SOIL_MAX) || (strcmp(air_status, "bad") == 0))
    {
        printf("Air:%s Warning\r\n", air_status);
    }
    else
    {
        printf("Air:%s\r\n", air_status);
    }
}

/**
  * @brief  程序主入口(分时隙调度)
  */
int main(void)
{
    uint32_t last_sensor_tick = 0;

    uart_init(9600);
    uart3_init(9600);
    DHT11_Init();
    OLED_Init();
    ADC1_Init();
    Alarm_Hardware_Init();
    Motor_Init();
    Servo_Init();
    Servo_SetAngle(0);
    OLED_Clear();
    Delay_Init();          // DWT使能 + SysTick 1ms时基(必须在各驱动延时前)

    OLED_ShowString(1, 1, "Temp:  C");
    OLED_ShowString(2, 1, "Humi:  %RH");
    OLED_ShowString(3, 1, "Soil Water:");
    OLED_ShowChar(3, 15, '%');
    OLED_ShowString(4, 1, "Air: ");

    while(1)
    {
        /* 时隙1: 串口指令——每圈检查, 毫秒级响应(优化点) */
        Process_Serial_Command();

        /* 时隙2: 烟雾消抖——非阻塞状态机, 每圈采样(优化点) */
        Check_Air_Quality();
        OLED_ShowString(4, 5, air_status);

        /* 时隙3: 报警联动——每圈判定, 无需等采集周期(优化点) */
        Handle_Alarms();

        /* 时隙4: 传感器采集——1.5s周期(打印协议在此, 周期不变) */
        if (Delay_IsTimeout(last_sensor_tick, SLOT_SENSOR_PERIOD_MS))
        {
            last_sensor_tick = Delay_GetTick();
            Sensor_Acquire_Slot();
        }

        /* 无阻塞等待: 主循环全速轮询, 空闲由WFI让出总线(可选) */
    }
}
