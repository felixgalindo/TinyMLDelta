/**
 * @file stm32l4xx_it.h
 * @brief Exception handlers this firmware defines. ST's startup file puts them
 *        in the vector table; nothing in C calls them.
 *
 * Author: Harshdeep Parmar
 * License: Apache-2.0
 */

#ifndef STM32L4XX_IT_H_
#define STM32L4XX_IT_H_

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

#endif /* STM32L4XX_IT_H_ */
