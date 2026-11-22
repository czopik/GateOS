// Crash capture that survives the panic reset (RTC no-init memory).
// esp_panic_handler is wrapped via -Wl,--wrap=esp_panic_handler (platformio.ini).
#include "crash_log.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_private/panic_internal.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/xtensa_context.h>
#include <esp_debug_helpers.h>
#include <string.h>

namespace {
constexpr uint32_t kMagic = 0xC7A5410Bu;
constexpr int kBt = 14;
constexpr int kCrumbs = 10;
constexpr int kCrumbLen = 44;

struct CrashRecord {
  uint32_t magic;
  uint32_t pc;
  uint32_t exccause;
  uint32_t excvaddr;
  uint32_t uptimeMs;
  int32_t core;
  uint32_t btCount;
  uint32_t bt[kBt];
  char reason[40];
  char task[16];
};

struct CrumbRing {
  uint32_t magic;
  uint32_t head;
  uint32_t ts[kCrumbs];
  char text[kCrumbs][kCrumbLen];
};

RTC_NOINIT_ATTR CrashRecord rtcCrash;
RTC_NOINIT_ATTR CrumbRing rtcCrumbs;

// Copies taken at boot (previous run) so new crumbs don't overwrite them.
CrashRecord g_prevCrash;
CrumbRing g_prevCrumbs;
bool g_prevCrashValid = false;
bool g_prevCrumbsValid = false;
portMUX_TYPE g_crumbMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR copyStr(char* dst, size_t n, const char* src) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}
} // namespace

extern "C" void __real_esp_panic_handler(panic_info_t* info);

extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
  rtcCrash.magic = kMagic;
  rtcCrash.core = info ? info->core : -1;
  rtcCrash.pc = info ? (uint32_t)info->addr : 0;
  rtcCrash.uptimeMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
  copyStr(rtcCrash.reason, sizeof(rtcCrash.reason), info ? info->reason : "");
  rtcCrash.exccause = 0;
  rtcCrash.excvaddr = 0;
  rtcCrash.btCount = 0;
  TaskHandle_t t = xTaskGetCurrentTaskHandleForCPU(info ? info->core : 0);
  copyStr(rtcCrash.task, sizeof(rtcCrash.task), t ? pcTaskGetName(t) : "?");
  if (info && info->frame) {
    const XtExcFrame* f = (const XtExcFrame*)info->frame;
    rtcCrash.exccause = f->exccause;
    rtcCrash.excvaddr = f->excvaddr;
    esp_backtrace_frame_t bt;
    bt.pc = f->pc;
    bt.sp = f->a1;
    bt.next_pc = f->a0;
    bt.exc_frame = nullptr;
    uint32_t n = 0;
    rtcCrash.bt[n++] = esp_cpu_process_stack_pc(bt.pc);
    while (n < (uint32_t)kBt && bt.next_pc != 0) {
      if (!esp_backtrace_get_next_frame(&bt)) break;
      rtcCrash.bt[n++] = esp_cpu_process_stack_pc(bt.pc);
    }
    rtcCrash.btCount = n;
  }
  __real_esp_panic_handler(info);
}

void crashLogBegin() {
  if (rtcCrash.magic == kMagic) {
    g_prevCrash = rtcCrash;
    g_prevCrashValid = true;
  }
  rtcCrash.magic = 0;
  if (rtcCrumbs.magic == kMagic && rtcCrumbs.head < 100000) {
    g_prevCrumbs = rtcCrumbs;
    g_prevCrumbsValid = true;
  }
  memset(&rtcCrumbs, 0, sizeof(rtcCrumbs));
  rtcCrumbs.magic = kMagic;
}

void crashLogCrumb(const char* text) {
  portENTER_CRITICAL(&g_crumbMux);
  const uint32_t i = rtcCrumbs.head % kCrumbs;
  rtcCrumbs.ts[i] = (uint32_t)(esp_timer_get_time() / 1000ULL);
  copyStr(rtcCrumbs.text[i], kCrumbLen, text);
  rtcCrumbs.head++;
  portEXIT_CRITICAL(&g_crumbMux);
}

void crashLogFill(JsonObject out) {
  out["valid"] = g_prevCrashValid;
  if (g_prevCrashValid) {
    char buf[12];
    snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)g_prevCrash.pc);
    out["pc"] = String(buf);
    out["reason"] = String(g_prevCrash.reason);
    out["task"] = String(g_prevCrash.task);
    out["core"] = g_prevCrash.core;
    out["exccause"] = g_prevCrash.exccause;
    snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)g_prevCrash.excvaddr);
    out["excvaddr"] = String(buf);
    out["uptimeMs"] = g_prevCrash.uptimeMs;
    String bt;
    for (uint32_t i = 0; i < g_prevCrash.btCount && i < (uint32_t)kBt; ++i) {
      snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)g_prevCrash.bt[i]);
      if (i) bt += ' ';
      bt += buf;
    }
    out["backtrace"] = bt;
  }
  if (g_prevCrumbsValid) {
    JsonArray arr = out.createNestedArray("prevCrumbs");
    const uint32_t head = g_prevCrumbs.head;
    const uint32_t count = head < (uint32_t)kCrumbs ? head : (uint32_t)kCrumbs;
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t i = (head - count + k) % kCrumbs;
      char line[kCrumbLen + 16];
      snprintf(line, sizeof(line), "%lu %s", (unsigned long)g_prevCrumbs.ts[i], g_prevCrumbs.text[i]);
      arr.add(String(line));
    }
  }
}
