Minimal stand-ins for the ESP-IDF and FreeRTOS headers used by `esphome/ciat_link.h`, so that the bus task can be
tested on a PC (`tools/test_ciat_link.cpp`). They do nothing except what the test needs: a controllable clock
(`clock_us`) and a counter of UART writes (`n_written`).
