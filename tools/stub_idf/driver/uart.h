#pragma once
#include <cstddef>
typedef int uart_port_t; enum { UART_NUM_1=1, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, UART_HW_FLOWCTRL_DISABLE, UART_SCLK_DEFAULT, UART_PIN_NO_CHANGE=-1, UART_SIGNAL_RXD_INV };
struct uart_config_t { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; };
inline int uart_driver_install(int,int,int,int,void*,int){return 0;} inline int uart_param_config(int,uart_config_t*){return 0;}
inline int uart_set_pin(int,int,int,int,int){return 0;} inline int uart_set_line_inverse(int,int){return 0;}
inline int uart_set_rx_full_threshold(int,int){return 0;} inline int uart_set_rx_timeout(int,int){return 0;}
extern int n_written; inline int uart_write_bytes(int,const char*,size_t n){n_written++;return n;}
inline int uart_read_bytes(int,unsigned char*,int,int){return 0;}
