#pragma once
#include "FreeRTOS.h"
#include <cassert>
namespace memory_test {
inline unsigned stack_reads{0};
}
inline char *pcTaskGetName(TaskHandle_t task) {
    assert(task == nullptr);
    static char name[] = "synthetic_calling_task";
    return name;
}
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    assert(task == nullptr);
    ++memory_test::stack_reads;
    return 4097;
}
