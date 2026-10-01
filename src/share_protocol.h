#ifndef HIYUI_APP_SHARE_PROTOCOL_H
#define HIYUI_APP_SHARE_PROTOCOL_H

#include "terminal_session.h"

namespace terminal_app::protocol {

constexpr size_t MaxInput = 64 * 1024;
constexpr size_t MaxFrame = 8 * 1024 * 1024;
constexpr int MaxColumns = 512;
constexpr int MaxRows = 128;
inline constexpr char Path[] = "/terminal";
inline constexpr char Name[] = "hiyui-terminal.v1";

std::string encodeInput(const hiyui::TerminalInput &);
bool decodeInput(std::string_view, hiyui::TerminalInput &);
std::string encodeFrame(const ScreenFrame &);
bool decodeFrame(std::string_view, ScreenFrame &);
bool validEndpoint(std::string_view);
std::string generateToken();

}
#endif
