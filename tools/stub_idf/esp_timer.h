#pragma once
#include <cstdint>
extern int64_t clock_us; inline int64_t esp_timer_get_time(){return clock_us;}
typedef void* esp_timer_handle_t; struct esp_timer_create_args_t { void(*callback)(void*); void* arg; const char* name; };
inline int esp_timer_create(esp_timer_create_args_t*, esp_timer_handle_t*){return 0;}
inline int esp_timer_start_once(esp_timer_handle_t,uint64_t){return 0;}
