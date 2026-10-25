// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <type_traits>
#include <queue>
#include <unordered_set>

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::IR {

template <typename Pred>
auto BreadthFirstSearch(const Value& value, Pred&& pred)
    -> std::invoke_result_t<Pred, const Inst*> {
    if (value.IsImmediate()) {
        // Nothing to do with immediates
        return std::nullopt;
    }
    // Breadth-first search visiting the right most arguments first. Visited instructions are a small
    // vector for short searches and a hash set past 32: a linear search per step made long searches
    // quadratic (seconds of pipeline building for shaders with many global memory accesses).
    boost::container::small_vector<const Inst*, 32> visited;
    std::unordered_set<const Inst*> visited_set;
    const auto first_visit = [&](const Inst* inst) {
        if (!visited_set.empty()) {
            return visited_set.insert(inst).second;
        }
        if (std::ranges::find(visited, inst) != visited.end()) {
            return false;
        }
        visited.push_back(inst);
        if (visited.size() > 32) {
            visited_set.insert(visited.begin(), visited.end());
        }
        return true;
    };
    std::queue<const Inst*> queue;
    queue.push(value.InstRecursive());

    while (!queue.empty()) {
        // Pop one instruction from the queue
        const Inst* const inst{queue.front()};
        queue.pop();
        if (const std::optional result = pred(inst)) {
            // This is the instruction we were looking for
            return result;
        }
        // Visit the right most arguments first
        for (size_t arg = inst->NumArgs(); arg--;) {
            const Value arg_value{inst->Arg(arg)};
            if (arg_value.IsImmediate()) {
                continue;
            }
            // Queue instruction if it hasn't been visited
            const Inst* const arg_inst{arg_value.InstRecursive()};
            if (first_visit(arg_inst)) {
                queue.push(arg_inst);
            }
        }
    }
    // SSA tree has been traversed and the result hasn't been found
    return std::nullopt;
}

} // namespace Shader::IR
