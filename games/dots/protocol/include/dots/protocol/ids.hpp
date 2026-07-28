#pragma once

#include "mycore/core/strong_id.hpp"

#include <cstdint>

namespace dots::protocol {

struct ClientIdTag;
struct EntityIdTag;
struct InputSequenceIdTag;
struct PlayerOwnerIdTag;
struct AuthorityReceiptSequenceIdTag;
struct SnapshotIdTag;

using AuthorityReceiptSequenceId =
    mycore::core::StrongId<AuthorityReceiptSequenceIdTag, std::uint32_t>;
using ClientId = mycore::core::StrongId<ClientIdTag, std::uint32_t>;
using EntityId = mycore::core::StrongId<EntityIdTag, std::uint32_t>;
using InputSequenceId = mycore::core::StrongId<InputSequenceIdTag, std::uint32_t>;
using PlayerOwnerId = mycore::core::StrongId<PlayerOwnerIdTag, std::uint32_t>;
using SnapshotId = mycore::core::StrongId<SnapshotIdTag, std::uint32_t>;

} // namespace dots::protocol
