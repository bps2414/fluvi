// SPDX-License-Identifier: GPL-3.0-only
#include "Models.h"

#include <algorithm>
#include <cctype>
#include <format>

#include <windows.h>

namespace fluvi::app {

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

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

std::string DescribeError(const std::exception& error) {
    if (dynamic_cast<const xhome::NetworkError*>(&error))
        return "Sem conexão com os servidores. Confira a internet e tente de novo.";
    if (const auto* service = dynamic_cast<const xhome::ServiceError*>(&error)) {
        const std::string xbox = ToUtf8(xhome::describeXboxError(service->body));
        if (!xbox.empty()) return xbox;
        const int status = service->status;
        if (status == 401 || status == 403) return std::format("O serviço recusou o acesso (HTTP {}). Entre de novo.", status);
        if (status == 404) return "O serviço não encontrou isso (HTTP 404). A sessão pode ter acabado.";
        if (status == 429) return "Muitas tentativas seguidas (HTTP 429). Espere um pouco e tente de novo.";
        if (status >= 500) return std::format("Os servidores estão com problema (HTTP {}). Tente de novo em instantes.", status);
        return std::format("O serviço respondeu com erro (HTTP {}).", status);
    }
    return std::format("Algo deu errado: {}", error.what());
}

std::string DescribeSessionFailure(const std::string& errorCode, const std::string& errorMessage) {
    std::string s = "O console recusou a sessão";
    if (!errorCode.empty()) s += std::format(" (código {})", errorCode);
    s += ".";
    if (!errorMessage.empty()) s += " " + errorMessage;
    return s;
}

std::string DescribeEnded(const std::string& reason) {
    if (reason == "connection lost") return "A conexão com o console caiu.";
    if (reason == "KickForServerShutdown") return "O console foi desligado ou reiniciado.";
    if (reason == "session expired") return "A sessão acabou no servidor.";
    if (reason.empty()) return "A transmissão terminou.";
    return std::format("A transmissão terminou ({}).", reason);
}

std::string DescribeSignInFailure(xhome::SignInPoll result, const std::string& detail) {
    switch (result) {
    case xhome::SignInPoll::Expired: return "O código venceu. Toque em Entrar para pedir outro.";
    case xhome::SignInPoll::Declined: return "O login foi recusado no outro aparelho.";
    case xhome::SignInPoll::Failed: return detail.empty() ? "Não foi possível entrar." : std::format("Não foi possível entrar: {}", detail);
    default: return {};
    }
}

} // namespace fluvi::app
