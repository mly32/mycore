#include "mycore/net_transport/net_transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mycore::net_transport {
namespace {

[[nodiscard]] bool has_explicit_port(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        const auto closing_bracket = value.find(']');
        return closing_bracket != std::string_view::npos && closing_bracket + 2 < value.size() &&
               value[closing_bracket + 1] == ':';
    }
    const auto separator = value.find(':');
    return separator != std::string_view::npos && separator == value.rfind(':') && separator > 0 &&
           separator + 1 < value.size();
}

[[nodiscard]] std::optional<SteamNetworkingIPAddr> parse_native_address(std::string_view value) {
    if (!has_explicit_port(value)) {
        return std::nullopt;
    }
    SteamNetworkingIPAddr address;
    const std::string terminated{value};
    if (!address.ParseString(terminated.c_str())) {
        return std::nullopt;
    }
    return address;
}

[[nodiscard]] std::string format_native_address(const SteamNetworkingIPAddr& address) {
    std::array<char, SteamNetworkingIPAddr::k_cchMaxString> buffer{};
    address.ToString(buffer.data(), buffer.size(), true);
    return buffer.data();
}

[[nodiscard]] ConnectionState map_state(ESteamNetworkingConnectionState state) noexcept {
    switch (state) {
    case k_ESteamNetworkingConnectionState_None:
        return ConnectionState::Disconnected;
    case k_ESteamNetworkingConnectionState_Connecting:
    case k_ESteamNetworkingConnectionState_FindingRoute:
        return ConnectionState::Connecting;
    case k_ESteamNetworkingConnectionState_Connected:
        return ConnectionState::Connected;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
        return ConnectionState::Disconnected;
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        return ConnectionState::Failed;
    case k_ESteamNetworkingConnectionState_FinWait:
    case k_ESteamNetworkingConnectionState_Linger:
    case k_ESteamNetworkingConnectionState_Dead:
        return ConnectionState::Closing;
    case k_ESteamNetworkingConnectionState__Force32Bit:
        return ConnectionState::Failed;
    }
    return ConnectionState::Failed;
}

[[nodiscard]] std::size_t nonnegative_size(int value) noexcept {
    return static_cast<std::size_t>(std::max(value, 0));
}

} // namespace

NetworkAddress::NetworkAddress(std::string value)
    : value_(std::move(value)) {}

std::optional<NetworkAddress> NetworkAddress::parse(std::string_view value) {
    const auto native = parse_native_address(value);
    if (!native) {
        return std::nullopt;
    }
    return NetworkAddress{format_native_address(*native)};
}

const std::string& NetworkAddress::value() const noexcept {
    return value_;
}

std::uint16_t NetworkAddress::port() const {
    const auto native = parse_native_address(value_);
    return native ? native->m_port : 0;
}

class GameNetworkingSocketsNetwork::Impl {
public:
    explicit Impl(NetworkImpairment impairment) {
        if (impairment.outgoing_lag_milliseconds >
                static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            !std::isfinite(impairment.outgoing_loss_percent) ||
            impairment.outgoing_loss_percent < 0.0F || impairment.outgoing_loss_percent > 100.0F) {
            throw std::invalid_argument{"Invalid native network impairment settings"};
        }
        if (active_instance_ != nullptr) {
            throw std::runtime_error{
                "Only one GameNetworkingSockets network may exist per process"};
        }
        SteamDatagramErrMsg message{};
        if (!GameNetworkingSockets_Init(nullptr, message)) {
            throw std::runtime_error{"Could not initialize GameNetworkingSockets: " +
                                     std::string{message}};
        }
        sockets_ = SteamNetworkingSockets();
        if (!SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
                status_changed) ||
            !SteamNetworkingUtils()->SetGlobalConfigValueInt32(
                k_ESteamNetworkingConfig_FakePacketLag_Send,
                static_cast<std::int32_t>(impairment.outgoing_lag_milliseconds)) ||
            !SteamNetworkingUtils()->SetGlobalConfigValueFloat(
                k_ESteamNetworkingConfig_FakePacketLoss_Send, impairment.outgoing_loss_percent)) {
            GameNetworkingSockets_Kill();
            throw std::runtime_error{"Could not configure native network impairment"};
        }
        active_instance_ = this;
    }

    ~Impl() {
        endpoints_.clear();
        active_instance_ = nullptr;
        GameNetworkingSockets_Kill();
    }

    class NativeEndpoint final : public Endpoint {
    public:
        NativeEndpoint(Impl& owner, HSteamListenSocket listen_socket, HSteamNetPollGroup poll_group)
            : owner_(owner),
              listen_socket_(listen_socket),
              poll_group_(poll_group) {}

        NativeEndpoint(Impl& owner, HSteamNetConnection connection)
            : owner_(owner) {
            static_cast<void>(add_connection(connection));
        }

        ~NativeEndpoint() override {
            for (auto& [unused, connection] : connections_) {
                static_cast<void>(unused);
                if (connection.native != k_HSteamNetConnection_Invalid) {
                    owner_.sockets_->CloseConnection(
                        connection.native, 0, "Endpoint shutdown", false);
                }
            }
            if (poll_group_ != k_HSteamNetPollGroup_Invalid) {
                owner_.sockets_->DestroyPollGroup(poll_group_);
            }
            if (listen_socket_ != k_HSteamListenSocket_Invalid) {
                owner_.sockets_->CloseListenSocket(listen_socket_);
            }
        }

        [[nodiscard]] std::vector<Event> poll() override {
            owner_.run_callbacks();
            receive_messages();

            std::vector<Event> events;
            events.reserve(events_.size());
            while (!events_.empty()) {
                events.push_back(std::move(events_.front()));
                events_.pop_front();
            }
            if (listen_socket_ != k_HSteamListenSocket_Invalid) {
                for (const auto& event : events) {
                    if (const auto* disconnected = std::get_if<Disconnected>(&event)) {
                        connections_.erase(disconnected->connection.value());
                    }
                }
            }
            return events;
        }

        [[nodiscard]] SendStatus send(ConnectionHandle handle,
                                      std::span<const std::byte> payload,
                                      DeliveryMode delivery) override {
            auto* connection = find(handle);
            if (connection == nullptr) {
                return SendStatus::UnknownConnection;
            }
            if (connection->state != ConnectionState::Connected ||
                connection->native == k_HSteamNetConnection_Invalid) {
                return SendStatus::Disconnected;
            }
            if (payload.size() >
                    static_cast<std::size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend) ||
                payload.size() > std::numeric_limits<std::uint32_t>::max()) {
                return SendStatus::PayloadTooLarge;
            }

            const auto flags = delivery == DeliveryMode::Reliable
                                   ? k_nSteamNetworkingSend_ReliableNoNagle
                                   : k_nSteamNetworkingSend_UnreliableNoNagle;
            const auto result =
                owner_.sockets_->SendMessageToConnection(connection->native,
                                                         payload.data(),
                                                         static_cast<std::uint32_t>(payload.size()),
                                                         flags,
                                                         nullptr);
            switch (result) {
            case k_EResultOK:
                return SendStatus::Sent;
            case k_EResultInvalidParam:
                return SendStatus::PayloadTooLarge;
            case k_EResultInvalidState:
            case k_EResultNoConnection:
                return SendStatus::Disconnected;
            case k_EResultLimitExceeded:
            case k_EResultRateLimitExceeded:
                return SendStatus::QueueFull;
            default:
                return SendStatus::TransportFailure;
            }
        }

        [[nodiscard]] bool disconnect(ConnectionHandle handle) override {
            auto* connection = find(handle);
            if (connection == nullptr || connection->disconnect_emitted ||
                connection->native == k_HSteamNetConnection_Invalid) {
                return false;
            }
            owner_.sockets_->CloseConnection(connection->native, 0, "Local request", true);
            native_to_handle_.erase(connection->native);
            connection->native = k_HSteamNetConnection_Invalid;
            connection->state = ConnectionState::Disconnected;
            connection->disconnect_emitted = true;
            events_.push_back(Disconnected{
                .connection = handle,
                .reason = DisconnectReason::LocalRequest,
            });
            return true;
        }

        [[nodiscard]] std::optional<TransportStatistics>
        statistics(ConnectionHandle handle) const override {
            const auto* connection = find(handle);
            if (connection == nullptr) {
                return std::nullopt;
            }
            TransportStatistics result;
            result.state = connection->state;
            if (connection->native == k_HSteamNetConnection_Invalid) {
                return result;
            }

            SteamNetConnectionRealTimeStatus_t native{};
            if (owner_.sockets_->GetConnectionRealTimeStatus(
                    connection->native, &native, 0, nullptr) != k_EResultOK) {
                return result;
            }
            result.state = map_state(native.m_eState);
            if (native.m_nPing >= 0) {
                result.round_trip_time = std::chrono::milliseconds{native.m_nPing};
            }
            if (std::isfinite(native.m_flConnectionQualityLocal) &&
                native.m_flConnectionQualityLocal >= 0.0F) {
                result.packet_loss_percent =
                    std::clamp((1.0F - native.m_flConnectionQualityLocal) * 100.0F, 0.0F, 100.0F);
            }
            result.outbound_packets_per_second = native.m_flOutPacketsPerSec;
            result.outbound_bytes_per_second = native.m_flOutBytesPerSec;
            result.inbound_packets_per_second = native.m_flInPacketsPerSec;
            result.inbound_bytes_per_second = native.m_flInBytesPerSec;
            result.pending_unreliable_bytes = nonnegative_size(native.m_cbPendingUnreliable);
            result.pending_reliable_bytes = nonnegative_size(native.m_cbPendingReliable);
            result.sent_unacknowledged_reliable_bytes =
                nonnegative_size(native.m_cbSentUnackedReliable);
            if (native.m_usecQueueTime >= 0) {
                result.outbound_queue_delay = std::chrono::microseconds{native.m_usecQueueTime};
            }
            return result;
        }

        [[nodiscard]] bool owns(HSteamNetConnection native) const noexcept {
            return native_to_handle_.contains(native);
        }

        [[nodiscard]] HSteamListenSocket listen_socket() const noexcept {
            return listen_socket_;
        }

        void handle_status(const SteamNetConnectionStatusChangedCallback_t& change) {
            const auto native = change.m_hConn;
            auto* connection = find(native);
            if (connection == nullptr &&
                change.m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
                change.m_info.m_hListenSocket == listen_socket_) {
                connection = &add_connection(native);
                if (owner_.sockets_->AcceptConnection(native) != k_EResultOK ||
                    !owner_.sockets_->SetConnectionPollGroup(native, poll_group_)) {
                    fail_connection(*connection);
                    return;
                }
            }
            if (connection == nullptr) {
                return;
            }

            connection->state = map_state(change.m_info.m_eState);
            switch (change.m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connected:
                if (!connection->connected_emitted) {
                    connection->connected_emitted = true;
                    events_.push_back(Connected{.connection = connection->handle});
                }
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
                finish_connection(*connection, DisconnectReason::RemoteRequest);
                break;
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                finish_connection(*connection, DisconnectReason::TransportFailure);
                break;
            default:
                break;
            }
        }

    private:
        struct Connection {
            HSteamNetConnection native{k_HSteamNetConnection_Invalid};
            ConnectionHandle handle;
            ConnectionState state{ConnectionState::Connecting};
            bool connected_emitted{};
            bool disconnect_emitted{};
        };

        [[nodiscard]] Connection& add_connection(HSteamNetConnection native) {
            const auto handle = owner_.allocate_handle();
            native_to_handle_.emplace(native, handle.value());
            const auto [iterator, inserted] = connections_.emplace(
                handle.value(), Connection{.native = native, .handle = handle});
            if (!inserted) {
                throw std::runtime_error{"Native transport connection handle collision"};
            }
            return iterator->second;
        }

        [[nodiscard]] Connection* find(ConnectionHandle handle) noexcept {
            const auto iterator = connections_.find(handle.value());
            return iterator == connections_.end() ? nullptr : &iterator->second;
        }

        [[nodiscard]] const Connection* find(ConnectionHandle handle) const noexcept {
            const auto iterator = connections_.find(handle.value());
            return iterator == connections_.end() ? nullptr : &iterator->second;
        }

        [[nodiscard]] Connection* find(HSteamNetConnection native) noexcept {
            const auto mapping = native_to_handle_.find(native);
            if (mapping == native_to_handle_.end()) {
                return nullptr;
            }
            const auto iterator = connections_.find(mapping->second);
            return iterator == connections_.end() ? nullptr : &iterator->second;
        }

        void receive_messages() {
            if (poll_group_ != k_HSteamNetPollGroup_Invalid) {
                while (receive_one_from_poll_group()) {
                }
                return;
            }
            for (const auto& [unused, connection] : connections_) {
                static_cast<void>(unused);
                if (connection.native == k_HSteamNetConnection_Invalid) {
                    continue;
                }
                while (receive_one_from_connection(connection.native)) {
                }
            }
        }

        [[nodiscard]] bool receive_one_from_poll_group() {
            SteamNetworkingMessage_t* message{};
            const auto count =
                owner_.sockets_->ReceiveMessagesOnPollGroup(poll_group_, &message, 1);
            if (count <= 0) {
                return false;
            }
            consume_message(*message);
            message->Release();
            return true;
        }

        [[nodiscard]] bool receive_one_from_connection(HSteamNetConnection native) {
            SteamNetworkingMessage_t* message{};
            const auto count = owner_.sockets_->ReceiveMessagesOnConnection(native, &message, 1);
            if (count <= 0) {
                return false;
            }
            consume_message(*message);
            message->Release();
            return true;
        }

        void consume_message(const SteamNetworkingMessage_t& message) {
            auto* connection = find(message.m_conn);
            if (connection == nullptr || message.m_cbSize < 0) {
                return;
            }
            const auto* begin = static_cast<const std::byte*>(message.m_pData);
            events_.push_back(PayloadReceived{
                .connection = connection->handle,
                .delivery = (message.m_nFlags & k_nSteamNetworkingSend_Reliable) != 0
                                ? DeliveryMode::Reliable
                                : DeliveryMode::Unreliable,
                .payload = {begin, begin + message.m_cbSize},
            });
        }

        void fail_connection(Connection& connection) {
            connection.state = ConnectionState::Failed;
            finish_connection(connection, DisconnectReason::TransportFailure);
        }

        void finish_connection(Connection& connection, DisconnectReason reason) {
            if (connection.disconnect_emitted) {
                return;
            }
            connection.disconnect_emitted = true;
            events_.push_back(Disconnected{
                .connection = connection.handle,
                .reason = reason,
            });
            if (connection.native != k_HSteamNetConnection_Invalid) {
                owner_.sockets_->CloseConnection(connection.native, 0, "Connection ended", false);
                native_to_handle_.erase(connection.native);
                connection.native = k_HSteamNetConnection_Invalid;
            }
        }

        Impl& owner_;
        HSteamListenSocket listen_socket_{k_HSteamListenSocket_Invalid};
        HSteamNetPollGroup poll_group_{k_HSteamNetPollGroup_Invalid};
        std::unordered_map<std::uint32_t, Connection> connections_;
        std::unordered_map<HSteamNetConnection, std::uint32_t> native_to_handle_;
        std::deque<Event> events_;
    };

    [[nodiscard]] ListeningEndpoint listen(const NetworkAddress& address) {
        auto native_address = require_address(address, true);
        auto listen_socket = k_HSteamListenSocket_Invalid;
        if (native_address.m_port == 0) {
            constexpr std::uint32_t kFirstDynamicPort = 49152;
            constexpr std::uint32_t kDynamicPortCount = 65535 - kFirstDynamicPort + 1;
            constexpr std::uint32_t kMaximumAttempts = 1024;
            const auto start = next_dynamic_port_.fetch_add(kMaximumAttempts);
            for (std::uint32_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
                native_address.m_port = static_cast<std::uint16_t>(
                    kFirstDynamicPort + ((start + attempt) % kDynamicPortCount));
                listen_socket = sockets_->CreateListenSocketIP(native_address, 0, nullptr);
                if (listen_socket != k_HSteamListenSocket_Invalid) {
                    break;
                }
            }
        } else {
            listen_socket = sockets_->CreateListenSocketIP(native_address, 0, nullptr);
        }
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            throw std::runtime_error{"Could not listen on " + address.value()};
        }
        const auto poll_group = sockets_->CreatePollGroup();
        if (poll_group == k_HSteamNetPollGroup_Invalid) {
            sockets_->CloseListenSocket(listen_socket);
            throw std::runtime_error{"Could not create the server transport poll group"};
        }

        auto endpoint = std::make_unique<NativeEndpoint>(*this, listen_socket, poll_group);
        auto* endpoint_pointer = endpoint.get();
        endpoints_.push_back(std::move(endpoint));

        SteamNetworkingIPAddr resolved{};
        if (!sockets_->GetListenSocketAddress(listen_socket, &resolved)) {
            throw std::runtime_error{"Could not query the bound server address"};
        }
        const auto resolved_address = NetworkAddress::parse(format_native_address(resolved));
        if (!resolved_address) {
            throw std::runtime_error{"Could not format the bound server address"};
        }
        return ListeningEndpoint{.endpoint = endpoint_pointer, .address = *resolved_address};
    }

    [[nodiscard]] Endpoint& connect(const NetworkAddress& address) {
        const auto native_address = require_address(address, false);
        const auto connection = sockets_->ConnectByIPAddress(native_address, 0, nullptr);
        if (connection == k_HSteamNetConnection_Invalid) {
            throw std::runtime_error{"Could not connect to " + address.value()};
        }
        auto endpoint = std::make_unique<NativeEndpoint>(*this, connection);
        auto* endpoint_pointer = endpoint.get();
        endpoints_.push_back(std::move(endpoint));
        return *endpoint_pointer;
    }

private:
    [[nodiscard]] static SteamNetworkingIPAddr require_address(const NetworkAddress& address,
                                                               bool allow_zero_port) {
        const auto native = parse_native_address(address.value());
        if (!native || (!allow_zero_port && native->m_port == 0)) {
            throw std::runtime_error{"Invalid native network address: " + address.value()};
        }
        return *native;
    }

    [[nodiscard]] ConnectionHandle allocate_handle() {
        if (next_handle_ == ConnectionHandle::kInvalidValue) {
            throw std::runtime_error{"Native transport connection handles are exhausted"};
        }
        return ConnectionHandle{next_handle_++};
    }

    void run_callbacks() {
        sockets_->RunCallbacks();
    }

    static void status_changed(SteamNetConnectionStatusChangedCallback_t* change) {
        if (active_instance_ != nullptr && change != nullptr) {
            active_instance_->handle_status(*change);
        }
    }

    void handle_status(const SteamNetConnectionStatusChangedCallback_t& change) {
        for (const auto& endpoint : endpoints_) {
            if (endpoint->owns(change.m_hConn) ||
                (endpoint->listen_socket() != k_HSteamListenSocket_Invalid &&
                 endpoint->listen_socket() == change.m_info.m_hListenSocket)) {
                endpoint->handle_status(change);
                return;
            }
        }
    }

    inline static Impl* active_instance_{};
    inline static std::atomic<std::uint32_t> next_dynamic_port_{};
    ISteamNetworkingSockets* sockets_{};
    std::vector<std::unique_ptr<NativeEndpoint>> endpoints_;
    std::uint32_t next_handle_{};
};

GameNetworkingSocketsNetwork::GameNetworkingSocketsNetwork(NetworkImpairment impairment)
    : impl_(std::make_unique<Impl>(impairment)) {}

GameNetworkingSocketsNetwork::~GameNetworkingSocketsNetwork() = default;

ListeningEndpoint GameNetworkingSocketsNetwork::listen(const NetworkAddress& address) {
    return impl_->listen(address);
}

Endpoint& GameNetworkingSocketsNetwork::connect(const NetworkAddress& address) {
    return impl_->connect(address);
}

} // namespace mycore::net_transport
