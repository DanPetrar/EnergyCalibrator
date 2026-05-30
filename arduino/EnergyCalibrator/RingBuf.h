#pragma once
#include <Arduino.h>

// PSRAM-backed circular ring buffer — runtime capacity.
// age=0 → newest entry, age=cnt-1 → oldest.

// ── per-board capacity constants ──────────────────────────────────────────────
#if defined(BOARD_DEVKITC1) || defined(BOARD_LILYGO_T7S3)
  // 16 MB flash / 8 MB PSRAM
  #define SEC_CAP_LTE  14400u   // 4 h  at 1 rec/s
  #define MIN_CAP_LTE  43200u   // 30 d at 1 rec/min
  #define SEC_CAP_ADF  86400u   // 24 h at 1 rec/s
  #define MIN_CAP_ADF   1440u   // 24 h at 1 rec/min
#else
  // ESP32-S3-Zero: 4 MB flash / 2 MB PSRAM (default)
  #define SEC_CAP_LTE   7200u   // 2 h   at 1 rec/s
  #define MIN_CAP_LTE  10080u   // 7 d   at 1 rec/min
  #define SEC_CAP_ADF  16200u   // 4.5 h at 1 rec/s
  #define MIN_CAP_ADF    270u   // 4.5 h at 1 rec/min
#endif

template<typename T>
struct RingBuf {
  T*       buf  = nullptr;
  uint32_t cap  = 0;
  uint32_t head = 0;
  uint32_t cnt  = 0;

  bool init(uint32_t capacity) {
    cap = capacity;
    buf = (T*)ps_malloc((size_t)cap * sizeof(T));
    if (!buf) { cap = 0; return false; }
    memset(buf, 0, (size_t)cap * sizeof(T));
    return true;
  }

  bool reinit(uint32_t newCap) {
    if (buf) { free(buf); buf = nullptr; }
    head = 0; cnt = 0;
    return init(newCap);
  }

  void push(const T& r) {
    if (!buf) return;
    buf[head] = r;
    if (++head == cap) head = 0;
    if (cnt < cap) cnt++;
  }

  bool get(uint32_t age, T& out) const {
    if (!buf || age >= cnt) return false;
    out = buf[(head + cap - 1u - age) % cap];
    return true;
  }

  void clear() { head = 0; cnt = 0; }
};
