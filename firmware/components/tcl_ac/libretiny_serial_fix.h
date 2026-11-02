// Link-time patch for a LibreTiny ring-buffer race on the Realtek Serial core.
//
// The core drains its shared RingBufferN from the main task while the UART RX
// interrupt fills it. Both sides update the volatile _numElems counter without
// a critical section (core api/Serial/Serial.cpp, RingBuffer.h): when the RX
// interrupt lands between the load and the store of the main task's
// _numElems--, that increment is lost and the counter permanently undercounts
// the queued bytes. Bytes then stay stuck in the buffer and every later frame
// read is shifted by the lost amount; a 5-byte shift observed on the tested
// TAC-12CHSD dropped the AC link until reboot.
//
// ac-common.yaml adds -Wl,--wrap for the mangled SerialClass::read() symbol,
// so every call (including the HardwareSerial vtable entry used by ESPHome's
// LibreTiny UART component) lands in the __wrap function below. It holds a
// FreeRTOS critical section across the read, the same fix as upstream PR
// libretiny-eu/libretiny#432 (issues #154, #419); drop this wrap once a
// LibreTiny release ships it. On RTL8710C the critical section raises BASEPRI
// to configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (2), which masks the UART
// interrupt (UART_IRQPri 4). Bytes arriving meanwhile wait in the hardware
// FIFO and are stored by the interrupt right after exit.
#ifndef FIRMWARE_LIBRETINY_SERIAL_FIX_H_
#define FIRMWARE_LIBRETINY_SERIAL_FIX_H_

#if defined(LIBRETINY) && __has_include(<FreeRTOS.h>)

#include <FreeRTOS.h>
#include <task.h>

extern "C" int __real__ZN11SerialClass4readEv(void *self);

extern "C" int __wrap__ZN11SerialClass4readEv(void *self) {
  taskENTER_CRITICAL();
  const int value = __real__ZN11SerialClass4readEv(self);
  taskEXIT_CRITICAL();
  return value;
}

#endif  // LIBRETINY && FreeRTOS.h

#endif  // FIRMWARE_LIBRETINY_SERIAL_FIX_H_
