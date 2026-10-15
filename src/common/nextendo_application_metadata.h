// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/nextendo_friends.h"
namespace Common::NextendoFriends {
// Combine cached display data for one title. An incomplete friend snapshot must
// not hide a name/cover already supplied by another snapshot of that same title.
inline void MergeApplicationMetadata(std::optional<PlayHistoryEntry>& best,
                                     const PlayHistoryEntry& candidate) {
    if (!best) { best = candidate; return; }
    if (best->application_id != candidate.application_id) return;
    if (best->name.empty()) best->name = candidate.name;
    if (best->icon.empty()) best->icon = candidate.icon;
}
} // namespace Common::NextendoFriends
