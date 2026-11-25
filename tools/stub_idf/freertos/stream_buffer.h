#pragma once
#include <cstddef>
typedef void* StreamBufferHandle_t; inline StreamBufferHandle_t xStreamBufferCreate(int,int){return (void*)1;}
inline size_t xStreamBufferSend(StreamBufferHandle_t,const void*,size_t n,int){return n;}
inline size_t xStreamBufferReceive(StreamBufferHandle_t,void*,size_t,int){return 0;}
