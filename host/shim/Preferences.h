#pragma once
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Stand-in for ESP32 NVS.
//
// In the browser it is backed by localStorage, so a best score and unlocked
// achievements survive a reload the same way they survive a power cycle on
// the board. Natively (and in the headless harness) it stays in memory, which
// is what the measured rollouts want anyway -- each run should start clean.
class Preferences {
public:
  bool begin(const char *ns, bool = false) { ns_ = ns ? ns : "p"; return true; }
  void end() {}

  uint32_t getUInt(const char *k, uint32_t d = 0)  { return get(k, d); }
  uint8_t  getUChar(const char *k, uint8_t d = 0)  { return (uint8_t)get(k, d); }
  uint16_t getUShort(const char *k, uint16_t d = 0){ return (uint16_t)get(k, d); }
  bool     getBool(const char *k, bool d = false)  { return get(k, d ? 1 : 0) != 0; }

  void putUInt(const char *k, uint32_t v)   { put(k, v); }
  void putUChar(const char *k, uint8_t v)   { put(k, v); }
  void putUShort(const char *k, uint16_t v) { put(k, v); }
  void putBool(const char *k, bool v)       { put(k, v ? 1 : 0); }

private:
  std::string key(const char *k) { return ns_ + "." + k; }

  uint32_t get(const char *k, uint32_t d) {
#ifdef __EMSCRIPTEN__
    char *raw = (char *)EM_ASM_PTR({
      try {
        var v = localStorage.getItem(UTF8ToString($0));
        if (v === null) return 0;
        return stringToNewUTF8(v);
      } catch (e) { return 0; }
    }, key(k).c_str());
    if (raw) { const uint32_t v = (uint32_t)strtoul(raw, nullptr, 10); free(raw); return v; }
    return d;
#else
    auto it = m_.find(key(k));
    return it == m_.end() ? d : it->second;
#endif
  }

  void put(const char *k, uint32_t v) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
      try { localStorage.setItem(UTF8ToString($0), UTF8ToString($1)); } catch (e) {}
    }, key(k).c_str(), std::to_string(v).c_str());
#else
    m_[key(k)] = v;
#endif
  }

  std::string ns_ = "p";
  std::map<std::string, uint32_t> m_;
};
