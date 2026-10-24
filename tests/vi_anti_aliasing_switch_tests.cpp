#include "vi_anti_aliasing_switch.hpp"
#include <iostream>
#include <sstream>
#include <string>

namespace {
struct Parsed {
  n64_video::AntiAliasing anti_aliasing;
  std::string log;
};

Parsed parse(std::string_view value) {
  std::ostringstream log;
  const auto anti_aliasing = jfg::parse_vi_anti_aliasing_switch(value, log);
  return {anti_aliasing, log.str()};
}
} // namespace

int main() {
  unsigned failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };
  using n64_video::AntiAliasing;

  const auto off = parse("off");
  check(off.anti_aliasing == AntiAliasing::off && off.log.empty(), "off is accepted");
  const auto msaa = parse("msaa");
  check(msaa.anti_aliasing == AntiAliasing::msaa && msaa.log.empty(), "msaa is accepted");
  const auto unset = parse("");
  check(unset.anti_aliasing == AntiAliasing::msaa && unset.log.empty(),
        "unset or empty defaults to msaa without a warning");

  for (const char *unknown : {"coverage", "MSAA", "on", " off"}) {
    const auto parsed = parse(unknown);
    check(parsed.anti_aliasing == AntiAliasing::msaa, "an unknown value defaults to msaa");
    check(parsed.log.find("[warning]") != std::string::npos &&
              parsed.log.find("JFG_VI_AA") != std::string::npos &&
              parsed.log.find(unknown) != std::string::npos,
          "an unknown value logs a warning naming the variable and the value");
  }

  std::cout << "VI anti-aliasing switch checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
