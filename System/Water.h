#ifndef _Water_H
#define _Water_H
#include "stm32f10x.h"

void ADC1_Init(void);
uint16_t ADC1_GetValue(uint8_t channel);

#endif

