#pragma once
#include <cstdint>
typedef int gpio_num_t; typedef int esp_err_t;
#define ESP_OK 0
enum { GPIO_MODE_INPUT, GPIO_PULLUP_ONLY, GPIO_INTR_ANYEDGE };
inline int gpio_reset_pin(gpio_num_t){return 0;} inline int gpio_set_direction(gpio_num_t,int){return 0;}
inline int gpio_set_pull_mode(gpio_num_t,int){return 0;} inline int gpio_install_isr_service(int){return 0;}
inline int gpio_set_intr_type(gpio_num_t,int){return 0;} inline int gpio_isr_handler_add(gpio_num_t,void(*)(void*),void*){return 0;}
inline int gpio_intr_enable(gpio_num_t){return 0;}
