#pragma once
#include <cstdint>
#include <map>
#include <string>
// In-memory stand-in for NVS: the harness is a fresh boot every run.
class Preferences {
public:
  bool begin(const char *, bool = false) { return true; }
  void end() {}
  uint32_t getUInt(const char *k, uint32_t d = 0)  { return has(k) ? (uint32_t)m_[k] : d; }
  uint8_t  getUChar(const char *k, uint8_t d = 0)  { return has(k) ? (uint8_t)m_[k] : d; }
  bool     getBool(const char *k, bool d = false)  { return has(k) ? (bool)m_[k] : d; }
  void putUInt(const char *k, uint32_t v)  { m_[k] = v; }
  void putUChar(const char *k, uint8_t v)  { m_[k] = v; }
  void putBool(const char *k, bool v)      { m_[k] = v ? 1 : 0; }
private:
  bool has(const char *k) { return m_.count(k) != 0; }
  std::map<std::string, uint32_t> m_;
};
