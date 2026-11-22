// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: pure mappings from xhome data to what the UI shows (and the user-facing error sentences, in pt-BR).
#pragma once
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "StreamController.h"
#include "fluvi/ui/Ui.h"
#include "fluvi/xhome/Account.h"
#include "fluvi/xhome/StreamApi.h"

namespace fluvi::app {

// "On" -> On, "ConnectedStandby" / "Standby" / "Sleep" -> Standby, "Off" / "ShuttingDown" -> Off, the rest Unknown.
ui::PowerState MapPower(std::string_view powerState);
// "XboxSeriesX" -> "Xbox Series X", "XboxOneS" -> "Xbox One S"; unknown types are split at the capitals.
std::string ConsoleTypeName(std::string_view consoleType);

struct ConsolesContext {
    std::string accountName;   // shown in the account chip; empty hides it
    std::string busyId;        // console being woken / connected
    bool refreshing = false;
    std::string error;
};
ui::ConsolesModel MapConsoles(const std::vector<xhome::Console>& consoles, const ConsolesContext& context);

ui::ConnectStage MapStage(StreamStage stage);

// Sentences for the user. Never include tokens: only status codes and our own text.
std::string DescribeError(const std::exception& error);
std::string DescribeSessionFailure(const std::string& errorCode, const std::string& errorMessage);
std::string DescribeEnded(const std::string& reason);
std::string DescribeSignInFailure(xhome::SignInPoll result, const std::string& detail);

} // namespace fluvi::app
