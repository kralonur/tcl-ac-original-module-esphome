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
// LibreTiny UART component) lands in the __wrap function below. Holding the
// SDK's UART critical section across the read is the same protection the core
// itself uses when installing the RX hook; bytes arriving meanwhile wait in
// the hardware FIFO and are stored by the interrupt right after exit.
#ifndef FIRMWARE_LIBRETINY_SERIAL_FIX_H_
#define FIRMWARE_LIBRETINY_SERIAL_FIX_H_

#if defined(LIBRETINY) && __has_include(<hal_uart.h>)

#include <hal_uart.h>

extern "C" int __real__ZN11SerialClass4readEv(void *self);

extern "C" int __wrap__ZN11SerialClass4readEv(void *self) {
  hal_uart_enter_critical();
  const int value = __real__ZN11SerialClass4readEv(self);
  hal_uart_exit_critical();
  return value;
}

#endif  // LIBRETINY && hal_uart.h

#endif  // FIRMWARE_LIBRETINY_SERIAL_FIX_H_
