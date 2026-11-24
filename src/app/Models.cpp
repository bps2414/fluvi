// SPDX-License-Identifier: GPL-3.0-only
#include "Models.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "fluvi/common/I18n.h"
#include "fluvi/core/Log.h"

namespace fluvi::app {

using json = nlohmann::json;

namespace {

bool Lower(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace

ui::PowerState MapPower(std::string_view p) {
    if (Lower(p, "On")) return ui::PowerState::On;
    if (Lower(p, "ConnectedStandby") || Lower(p, "Standby") || Lower(p, "Sleep") || Lower(p, "InstantOn")) return ui::PowerState::Standby;
    if (Lower(p, "Off") || Lower(p, "ShuttingDown")) return ui::PowerState::Off;
    return ui::PowerState::Unknown;
}

std::string ConsoleTypeName(std::string_view type) {
    std::string out;
    for (size_t i = 0; i < type.size(); ++i) {
        const char c = type[i];
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        if (i > 0 && upper && std::islower(static_cast<unsigned char>(type[i - 1]))) out += ' ';
        if (c == '_') {
            out += ' ';
            continue;
        }
        out += c;
    }
    return out;
}

ui::ConsolesModel MapConsoles(const std::vector<xhome::Console>& consoles, const ConsolesContext& context) {
    ui::ConsolesModel m;
    m.accountName = context.accountName;
    m.refreshing = context.refreshing;
    m.error = context.error;
    for (const xhome::Console& c : consoles) {
        ui::ConsoleItem item;
        item.id = c.serverId;
        item.type = ConsoleTypeName(c.consoleType);
        item.name = c.name.empty() ? (item.type.empty() ? c.serverId : item.type) : c.name;
        item.power = MapPower(c.powerState);
        item.busy = !context.busyId.empty() && context.busyId == c.serverId;
        m.consoles.push_back(std::move(item));
    }
    return m;
}

ui::ConnectStage MapStage(StreamStage stage) {
    switch (stage) {
    case StreamStage::Provisioning: return ui::ConnectStage::Provisioning;
    case StreamStage::ReadyToConnect: return ui::ConnectStage::ReadyToConnect;
    case StreamStage::Connecting: return ui::ConnectStage::Connecting;
    case StreamStage::Connected: return ui::ConnectStage::Connected;
    }
    return ui::ConnectStage::Provisioning;
}

// Every sentence below is a translated key. The raw server text (status, codes, messages) goes to the log only.
std::string DescribeError(const std::exception& error) {
    FLUVI_WARN("app", "error detail: {}", error.what());
    using i18n::Tr;
    if (dynamic_cast<const xhome::NetworkError*>(&error)) return Tr("app.err.network");
    if (const auto* service = dynamic_cast<const xhome::ServiceError*>(&error)) {
        // Xbox account problems come as an "XErr" code in the body.
        const json body = json::parse(service->body, nullptr, /*allow_exceptions=*/false);
        const auto xe = body.is_object() ? body.find("XErr") : body.end();
        const uint64_t xerr = body.is_object() && xe != body.end() && xe->is_number_unsigned() ? xe->get<uint64_t>() : 0;
        switch (xerr) {
        case 2148916233: return Tr("app.err.xbox.noProfile");
        case 2148916235: return Tr("app.err.xbox.region");
        case 2148916236: case 2148916237: return Tr("app.err.xbox.adult");
        case 2148916238: return Tr("app.err.xbox.child");
        default: break;
        }
        const int status = service->status;
        if (status == 401 || status == 403) return Tr("app.err.denied");
        if (status == 404) return Tr("app.err.notFound");
        if (status == 429) return Tr("app.err.tooMany");
        if (status >= 500) return Tr("app.err.server");
        return Tr("app.err.service");
    }
    return Tr("app.err.generic");
}

// Known failures get a sentence with the action to take; unknown ones a generic one (the code stays in the log).
std::string DescribeSessionFailure(const std::string& errorCode, const std::string& errorMessage) {
    FLUVI_WARN("app", "session failure detail: {} {}", errorCode, errorMessage);
    using i18n::Tr;
    const auto has = [&](const char* s) { return errorCode.find(s) != std::string::npos || errorMessage.find(s) != std::string::npos; };
    // The console is not registered with the push service: off (energy saving), still booting, or remote features off.
    if (has("WNSError") || has("WaitingForServerToRegister") || has("ErrorCallingWNS")) return Tr("app.err.consoleOff");
    if (has("Busy") || has("AlreadyInSession") || has("InUse")) return Tr("app.err.consoleBusy");
    if (has("Timeout") || has("TimedOut")) return Tr("app.err.consoleTimeout");
    return Tr("app.err.consoleRefused");
}

std::string DescribeEnded(const std::string& reason) {
    using i18n::Tr;
    if (reason == "connection lost") return Tr("app.ended.lost");
    if (reason == "KickForServerShutdown") return Tr("app.ended.shutdown");
    if (reason == "session expired") return Tr("app.ended.expired");
    return Tr("app.ended.generic");
}

std::string DescribeSignInFailure(xhome::SignInPoll result, const std::string& detail) {
    using i18n::Tr;
    switch (result) {
    case xhome::SignInPoll::Expired: return Tr("app.signin.codeExpired");
    case xhome::SignInPoll::Declined: return Tr("app.signin.declined");
    case xhome::SignInPoll::Failed:
        if (!detail.empty()) FLUVI_WARN("app", "sign-in failure detail: {}", detail);
        return Tr("app.signin.failed");
    default: return {};
    }
}

} // namespace fluvi::app
