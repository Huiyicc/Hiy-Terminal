#include "share_protocol.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#endif

namespace terminal_app::protocol {
namespace {
void number(std::string &out, uint64_t n, int bytes) {
  for (int i = bytes - 1; i >= 0; --i)
    out += static_cast<char>(n >> (i * 8));
}
uint64_t number(std::string_view &in, int bytes) {
  uint64_t result = 0;
  for (int i = 0; i < bytes; ++i)
    result = (result << 8) | static_cast<uint8_t>(in[i]);
  in.remove_prefix(bytes);
  return result;
}
}

std::string encodeInput(const hiyui::TerminalInput &input) {
  if (input.text.size() > MaxInput)
    return {};
  std::string out = "I";
  number(out, static_cast<uint8_t>(input.type), 1);
  number(out, static_cast<uint16_t>(input.key), 2);
  number(out, input.character, 4);
  number(out, input.modifiers, 1);
  number(out, input.row, 2);
  number(out, input.column, 2);
  number(out, input.button, 1);
  number(out, input.pressed, 1);
  out += input.text;
  hiyui::TerminalInput checked;
  return decodeInput(out, checked) ? out : std::string{};
}

bool decodeInput(std::string_view in, hiyui::TerminalInput &result) {
  if (in.size() < 15 || in.size() > MaxInput + 15 || in.front() != 'I')
    return false;
  in.remove_prefix(1);
  hiyui::TerminalInput input;
  input.type = static_cast<hiyui::TerminalInputType>(number(in, 1));
  input.key = static_cast<hiyui::TerminalKey>(number(in, 2));
  input.character = static_cast<uint32_t>(number(in, 4));
  input.modifiers = static_cast<uint8_t>(number(in, 1));
  input.row = static_cast<int>(number(in, 2));
  input.column = static_cast<int>(number(in, 2));
  input.button = static_cast<int>(number(in, 1));
  const auto pressed = number(in, 1);
  input.pressed = pressed != 0;
  input.text = in;
  if (input.type > hiyui::TerminalInputType::Mouse || input.modifiers > 7 ||
      pressed > 1 || input.row >= MaxRows || input.column >= MaxColumns ||
      input.button > 5 || input.character > 0x10ffff ||
      (input.character >= 0xd800 && input.character <= 0xdfff))
    return false;
  if (input.type == hiyui::TerminalInputType::Key &&
      !((input.key >= hiyui::TerminalKey::Enter &&
         input.key <= hiyui::TerminalKey::PageDown) ||
        (input.key >= hiyui::TerminalKey::F1 &&
         input.key <= hiyui::TerminalKey::F12)))
    return false;
  if (input.type != hiyui::TerminalInputType::Text &&
      input.type != hiyui::TerminalInputType::Paste && !in.empty())
    return false;
  result = input;
  return true;
}

std::string encodeFrame(const ScreenFrame &frame) {
  if (frame.size.columns < 1 || frame.size.columns > MaxColumns ||
      frame.size.rows < 1 || frame.size.rows > MaxRows ||
      frame.screen.size() > MaxFrame - 18)
    return {};
  std::string out = "S";
  number(out, frame.size.columns, 2);
  number(out, frame.size.rows, 2);
  number(out, frame.revision, 8);
  number(out, frame.running, 1);
  number(out, static_cast<uint32_t>(frame.exitCode), 4);
  out += frame.screen;
  return out;
}

bool decodeFrame(std::string_view in, ScreenFrame &result) {
  if (in.size() < 18 || in.size() > MaxFrame || in.front() != 'S')
    return false;
  in.remove_prefix(1);
  ScreenFrame frame;
  frame.size.columns = static_cast<int>(number(in, 2));
  frame.size.rows = static_cast<int>(number(in, 2));
  frame.revision = number(in, 8);
  const auto running = number(in, 1);
  frame.running = running != 0;
  frame.exitCode = static_cast<int32_t>(number(in, 4));
  if (frame.size.columns < 1 || frame.size.columns > MaxColumns ||
      frame.size.rows < 1 || frame.size.rows > MaxRows || running > 1 ||
      !frame.revision)
    return false;
  frame.screen = in;
  result = std::move(frame);
  return true;
}

bool validEndpoint(std::string_view url) {
  if (!url.starts_with("ws://") || !url.ends_with(Path) || url.size() > 2048)
    return false;
  if (url.find_first_of("@?#\\\r\n\t ") != std::string_view::npos)
    return false;
  const auto authority = url.substr(5, url.size() - 5 - (sizeof(Path) - 1));
  return !authority.empty() && authority.find('/') == std::string_view::npos;
}

std::string generateToken() {
  unsigned char bytes[32];
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw std::runtime_error("无法生成共享令牌");
#else
  std::ifstream random("/dev/urandom", std::ios::binary);
  if (!random.read(reinterpret_cast<char *>(bytes), sizeof(bytes)))
    throw std::runtime_error("无法生成共享令牌");
#endif
  constexpr char hex[] = "0123456789abcdef";
  std::string token;
  for (auto b : bytes) {
    token += hex[b >> 4];
    token += hex[b & 15];
  }
  return token;
}

}
