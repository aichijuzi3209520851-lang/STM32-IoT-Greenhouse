//说明:
//	VCC--3.3V/5V
//	GND--GND
//	DATA--PA15(接DHT11)
//	Water Sensor--PA0(ADC通道)
//	Smoke DOUT--PA6
//	红灯LED--PA1(烟雾报警)
//  黄灯LED--PB0(温湿度报警)
//  蓝灯LED--PA3(水位报警)
//  烟雾报警LED--PB4(烟雾浓度报警)
//  蜂鸣器--PA7

#include "sys.h"
#include "Delay.h"
#include "DHT11.h"
#include "usart.h"
#include "OLED.h"
#include "Water.h"  // ˮλ������ͷ�ļ�
#include "Motor.h"
#include "Servo.h"
#include <string.h>  // �ַ�����������

// 硬件引脚定义
#define SMOKE_DOUT PAin(6)   // 烟雾传感器输出PA6
#define RED_LED PAout(1)     // 红灯LED(烟雾报警)
#define YELLOW_LED PBout(0)  // 黄灯LED(温湿度报警)
#define BLUE_LED PAout(3)    // 蓝灯LED(水位报警)
#define SMOKE_ALARM_LED PBout(4)  // 烟雾浓度报警LED(PB4)
#define BEEP PAout(7)        // 蜂鸣器PA7

// 全局变量
u8 temperature;  // 温度变量
u8 humidity;     // 湿度变量
uint16_t water_level;  // 水位原始值
uint8_t water_percent; // 水位百分比
char air_status[10] = "nice";  // 空气质量状态
uint8_t servo_angle = 0;  // 舵机当前角度状态(0或180)
uint8_t manual_mode = 0;  // 手动控制模式(1=串口手动控制,0=自动报警控制)
uint8_t motor_status = 0;  // 电机状态(0=停止,1=运行)

// 初始化所有报警相关硬件
void Alarm_Hardware_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);  // 使能PA端口时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);  // 使能PB端口时钟
    
    // 烟雾传感器(PA6)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;  // 上拉输入
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // 红灯LED(PA1)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;  // 推挽输出
    GPIO_Init(GPIOA, &GPIO_InitStructure);

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

// 检测空气质量状态
void Check_Air_Quality(void)
{
    if(SMOKE_DOUT == 0)
    {
        Delay_ms(50);  // 延时消抖
        if(SMOKE_DOUT == 0)
        {
            strcpy(air_status, "bad");
        }
    }
    else
    {
        strcpy(air_status, "nice");
    }
}

// 处理串口指令
void Process_Serial_Command(void)
{
    // 处理USART1的指令
    if(USART_RX_STA & 0x8000)  // 接收完成
    {
        u16 len = USART_RX_STA & 0x3FFF;  // 获取接收长度

        // 检测模式切换指令
        if(len >= 6 && strncmp((char*)USART_RX_BUF, "mode-0", 6) == 0)
        {
            manual_mode = 0;  // 切换到自动模式
        }
        else if(len >= 6 && strncmp((char*)USART_RX_BUF, "mode-1", 6) == 0)
        {
            manual_mode = 1;  // 切换到手动模式
        }
        // 只有在手动模式下才处理设备控制指令
        else if(manual_mode)
        {
            // 检测舵机控制指令
            if(len >= 6 && strncmp((char*)USART_RX_BUF, "DUOJ-0", 6) == 0)
            {
                Servo_SetAngle(0);
                servo_angle = 0;
            }
            else if(len >= 6 && strncmp((char*)USART_RX_BUF, "DUOJ-1", 6) == 0)
            {
                Servo_SetAngle(180);
                servo_angle = 180;
            }
            // 检测电机控制指令
            else if(len >= 7 && strncmp((char*)USART_RX_BUF, "MOTOR-0", 7) == 0)
            {
                Motor_SetSpeed(0);  // 停止电机
                motor_status = 0;
            }
            else if(len >= 7 && strncmp((char*)USART_RX_BUF, "MOTOR-1", 7) == 0)
            {
                Motor_SetSpeed(100);  // 启动电机全速
                motor_status = 1;
            }
        }

        USART_RX_STA = 0;  // 清除接收标志
    }

    // 处理USART3的指令(与USART1相同的逻辑)
    if(USART3_RX_STA & 0x8000)  // 接收完成
    {
        u16 len = USART3_RX_STA & 0x3FFF;  // 获取接收长度

        // 检测模式切换指令
        if(len >= 6 && strncmp((char*)USART3_RX_BUF, "mode-0", 6) == 0)
        {
            manual_mode = 0;  // 切换到自动模式
        }
        else if(len >= 6 && strncmp((char*)USART3_RX_BUF, "mode-1", 6) == 0)
        {
            manual_mode = 1;  // 切换到手动模式
        }
        // 只有在手动模式下才处理设备控制指令
        else if(manual_mode)
        {
            // 检测舵机控制指令
            if(len >= 6 && strncmp((char*)USART3_RX_BUF, "DUOJ-0", 6) == 0)
            {
                Servo_SetAngle(0);
                servo_angle = 0;
            }
            else if(len >= 6 && strncmp((char*)USART3_RX_BUF, "DUOJ-1", 6) == 0)
            {
                Servo_SetAngle(10);
                servo_angle = 10;
            }
            // 检测电机控制指令
            else if(len >= 7 && strncmp((char*)USART3_RX_BUF, "MOTOR-0", 7) == 0)
            {
                Motor_SetSpeed(0);  // 停止电机
                motor_status = 0;
            }
            else if(len >= 7 && strncmp((char*)USART3_RX_BUF, "MOTOR-1", 7) == 0)
            {
                Motor_SetSpeed(100);  // 启动电机全速
                motor_status = 1;
            }
        }

        USART3_RX_STA = 0;  // 清除接收标志
    }
}

// 处理报警逻辑,返回是否处于报警状态
uint8_t Handle_Alarms(void)
{
    // 1. 温湿度报警判断(温度>30或湿度>70)
    uint8_t temp_humi_alarm = (temperature > 26 || humidity > 85) ? 1 : 0;

    // 2. 水位报警判断(水位>50%)
    uint8_t water_alarm = (water_percent > 40) ? 1 : 0;

    // 3. 烟雾报警判断(空气质量为bad)
    uint8_t smoke_alarm = (strcmp(air_status, "bad") == 0) ? 1 : 0;

    // 控制对应LED
    YELLOW_LED = temp_humi_alarm ? 0 : 1;  // 低电平点亮
    BLUE_LED = water_alarm ? 0 : 1;
    RED_LED = smoke_alarm ? 0 : 1;
    SMOKE_ALARM_LED = smoke_alarm ? 0 : 1;  // 烟雾浓度报警LED

    // 控制蜂鸣器(任一报警即触发)
    BEEP = (temp_humi_alarm || water_alarm || smoke_alarm) ? 0 : 1;

    // 只有在自动模式下才由报警状态控制执行器
    if (!manual_mode)
    {
        // 控制电机(烟雾报警时全速转动)
        if (smoke_alarm)
        {
            Motor_SetSpeed(100);  // 全速正转
            motor_status = 1;
        }
        else
        {
            Motor_SetSpeed(0);    // 停止
            motor_status = 0;
        }

        // 控制舵机(温湿度报警时转动180度)
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

    // 返回温湿度报警状态
    return temp_humi_alarm;
}

int main(void)
{
    uint8_t temp_humi_alarm_status;  // 温湿度报警状态

    uart_init(9600);     // 初始化串口1,波特率9600
    uart3_init(9600);    // 初始化串口3,波特率9600
    DHT11_Init();        // 初始化DHT11温湿度传感器
    OLED_Init();         // 初始化OLED显示屏
    ADC1_Init();         // 初始化ADC(水位传感器)
    Alarm_Hardware_Init();// 初始化所有报警硬件
    Motor_Init();        // 初始化电机
    Servo_Init();        // 初始化舵机
    Servo_SetAngle(0);   // 舵机初始位置0度
    OLED_Clear();        // 清屏

    // 固定显示标识
    OLED_ShowString(1, 1, "Temp:  C");  // 第一行显示温度标识
    OLED_ShowString(2, 1, "Humi:  %RH");// 第二行显示湿度标识
    OLED_ShowString(3, 1, "Water:   %"); // 第三行显示水位标识
    OLED_ShowString(4, 1, "Air: ");      // 第四行显示空气质量标识

    while(1)
    {
        // 读取温湿度数据
        if(DHT11_Read_Data(&temperature, &humidity) == 0)  // 成功读取数据
        {
            // 串口打印
            printf("Temp:%dC   Humi:%d%%RH   ", temperature, humidity);
            OLED_ShowNum(1, 6, temperature, 2);  // OLED显示温度
            OLED_ShowNum(2, 6, humidity, 2);    // OLED显示湿度
            OLED_ShowString(4, 5, "       ");   // 清除错误信息
        }
        else
        {
            printf("DHT11 Read Failed!   ");            // 读取失败时提示
            OLED_ShowString(4, 5, "Error!");  // 显示错误信息
        }

        // 读取水位数据
        water_level = ADC1_GetValue(ADC_Channel_0);        // 读取水位传感器原值
        water_percent = (uint8_t)((water_level * 100U) / 4095U);  // 转换为百分比
        printf("Water:%d%%   ", water_percent);        // 串口打印水位百分比
        OLED_ShowNum(3, 7, water_percent, 3);        // OLED显示水位百分比

        // 检测空气质量
        Check_Air_Quality();
        OLED_ShowString(4, 5, air_status);        // 显示空气质量状态

        // 处理串口指令
        Process_Serial_Command();

        // 处理报警逻辑
        temp_humi_alarm_status = Handle_Alarms();

        // 打印舵机和电机状态(根据实际状态显示)
        printf("Servo:%s Motor:%s ",
               (servo_angle == 180) ? "ON " : "OFF",
               (motor_status == 1) ? "ON " : "OFF");

        // 打印Air状态,根据报警状态决定是否附加Warning(在输出Air状态后,不换行)
        if (temp_humi_alarm_status || (water_percent > 50) || (strcmp(air_status, "bad") == 0))
        {
            printf("Air:%s Warning\r\n", air_status);  // 有报警时附加Warning
        }
        else
        {
            printf("Air:%s\r\n", air_status);  // 无报警时正常打印
        }

        Delay_ms(1500);  // 1.5秒读取一次
    }
}
