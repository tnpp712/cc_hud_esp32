#pragma once
#include <cstdint>
#include <cstddef>
extern uint32_t preview_ms;
inline uint32_t millis(){return preview_ms;}
inline long random(long a,long b){return (a+b)/2;}
struct PreviewSerial{void println(const char*){}};
inline PreviewSerial Serial;
