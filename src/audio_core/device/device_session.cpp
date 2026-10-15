// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/audio_core.h"
#include "audio_core/audio_manager.h"
#include "audio_core/device/audio_buffer.h"
#include "audio_core/device/device_session.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/guest_memory.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"
#include <array>
#include <cstdlib>

namespace AudioCore {

using namespace std::literals;
constexpr auto INCREMENT_TIME{5ms};

// Local, read-only observation of UNO 1.1.17's Storm startup states. Never reads
// strings, tokens or packet payloads, and never changes guest state.
static void TraceUnoStormState(Core::System& system, Core::Memory::Memory& memory) {
    const char* probe = std::getenv("NEXTENDO_UNO_PROBE");
    if (!probe || std::string_view{probe} != "1" ||
        system.GetApplicationProcessProgramID() != 0x0100B6E012EBE000ULL) {
        return;
    }
    // This private diagnostic requires the known module mapping and instruction
    // fingerprint. Fail closed for another game version or module layout.
    constexpr u64 main_base = 0x80004000;
    constexpr u64 signature = main_base + 0x250AD30;
    constexpr std::array<u32, 4> expected{0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xF00306F3};
    if (!memory.IsValidVirtualAddressRange(signature, 16)) {
        return;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (memory.Read32(signature + index * 4) != expected[index]) {
            return;
        }
    }
    // The Online singleton retains numeric manager errors even when the menu
    // transition is too short to observe through socket calls.
    const auto valid = [&](u64 address, u64 size) {
        return address != 0 && memory.IsValidVirtualAddressRange(address, size);
    };
    // Check only the three known Storm configuration keys. Never emit URL
    // contents, arbitrary cache strings, account fields or native addresses.
    const auto pointer = [&](u64 address) -> u64 {
        return valid(address, 8) ? memory.Read64(address) : 0;
    };
    const u64 engine_type = pointer(main_base + 0x8005598);
    const u64 engine_fields = pointer(engine_type ? engine_type + 0xB8 : 0);
    const u64 controller = pointer(engine_fields ? engine_fields + 0x50 : 0);
    const u64 facade = pointer(controller ? controller + 0x68 : 0);
    const u64 native_handle = pointer(facade ? facade + 0x30 : 0);
    const u64 native_facade = pointer(native_handle);
    const u64 cache = pointer(native_facade ? native_facade + 0x20 : 0);
    if (valid(cache, 0x168)) {
        const auto string_shape = [&](u64 wrapper) -> std::pair<u64, u64> {
            const u64 value = pointer(wrapper);
            if (!valid(value, 0x28)) return {0, 0};
            const u8 head = memory.Read8(value + 0x10);
            const u64 size = (head & 1) ? memory.Read64(value + 0x18) : head >> 1;
            const u64 data = (head & 1) ? memory.Read64(value + 0x20) : value + 0x11;
            if (size > 2048 || (size && !valid(data, size))) return {0, 0};
            return {data, size};
        };
        constexpr std::array<std::string_view, 3> keys{
            "detectProvisioningUrl", "traversalProvisioningUrl", "traversalUrl"};
        // 0 = absent, 1 = present but empty, 2+ = value length + 1.
        // Keep structural counters with the key shapes so an unreadable tree
        // cannot silently be reported as an empty configuration.
        std::array<u64, 6> shapes{};
        std::array<u64, 64> pending{};
        size_t count = 0;
        const u64 root = pointer(cache + 0x158);
        if (root) pending[count++] = root;
        for (size_t visited = 0; count && visited < 64; ++visited) {
            const u64 node = pending[--count];
            if (!valid(node, 0x30)) {
                ++shapes[5];
                continue;
            }
            ++shapes[3];
            const auto [data, length] = string_shape(node + 0x20);
            if (data && length) ++shapes[4];
            for (size_t k = 0; k < keys.size(); ++k) {
                if (length != keys[k].size() || !data) continue;
                bool matches = true;
                for (size_t j = 0; j < length; ++j) {
                    if (memory.Read8(data + j) != static_cast<u8>(keys[k][j])) {
                        matches = false;
                        break;
                    }
                }
                if (matches) shapes[k] = string_shape(node + 0x28).second + 1;
            }
            for (const u64 offset : {0ULL, 8ULL}) {
                const u64 child = pointer(node + offset);
                if (child && child != node && count < pending.size()) pending[count++] = child;
            }
        }
        static std::optional<std::array<u64, 6>> prior_shapes;
        static unsigned shape_events = 0;
        if (shape_events < 32 && (!prior_shapes || *prior_shapes != shapes)) {
            prior_shapes = shapes;
            ++shape_events;
            LOG_INFO(Service_Audio, "[UNO diagnostic] Storm config shapes detect={} traversal={} direct={} "
                                    "nodes={} decoded_keys={} unreadable_nodes={} "
                                    "(0 absent; 1 empty; otherwise length+1)",
                     shapes[0], shapes[1], shapes[2], shapes[3], shapes[4], shapes[5]);
        }
    }
    const u64 online_slot = main_base + 0x80035F0;
    if (valid(online_slot, 8)) {
        const u64 online_type = memory.Read64(online_slot);
        if (valid(online_type, 0xC0)) {
            const u64 online_fields = memory.Read64(online_type + 0xB8);
            if (valid(online_fields, 8)) {
                const u64 online = memory.Read64(online_fields);
                if (valid(online, 0x88)) {
                    const u64 flow = memory.Read64(online + 0x18);
                    if (valid(flow, 0xD8)) {
                        std::array<u64, 6> flow_state{
                            memory.Read8(flow + 0x20), memory.Read8(flow + 0x21),
                            memory.Read32(flow + 0x28), memory.Read64(flow + 0x30),
                            memory.Read32(flow + 0x40), 255};
                        const u64 machine = memory.Read64(flow + 0x10);
                        if (valid(machine, 0x38)) {
                            const u64 current = memory.Read64(machine + 0x30);
                            constexpr std::array<u32, 12> ids{0, 1, 4, 5, 2, 3, 7, 6, 8, 9, 10, 11};
                            for (size_t i = 0; current && i < ids.size(); ++i) {
                                if (current == memory.Read64(flow + 0x78 + i * 8)) {
                                    flow_state[5] = ids[i];
                                    break;
                                }
                            }
                        }
                        static std::optional<std::array<u64, 6>> previous_flow;
                        static unsigned flow_events = 0;
                        if (flow_events < 128 && (!previous_flow || *previous_flow != flow_state)) {
                            previous_flow = flow_state;
                            ++flow_events;
                            LOG_INFO(Service_Audio,
                                     "[UNO diagnostic] Flow requesting={} in_error={} code={} "
                                     "reason={} module={} phase={}",
                                     flow_state[0], flow_state[1], flow_state[2], flow_state[3],
                                     flow_state[4], flow_state[5]);
                        }
                    }
                    const std::array<u64, 3> offsets{0x50, 0x60, 0x78};
                    std::array<u64, 14> state{};
                    state[0] = memory.Read8(online + 0x10);
                    state[1] = memory.Read8(online + 0x38);
                    for (size_t i = 0; i < offsets.size(); ++i) {
                        const u64 object = memory.Read64(online + offsets[i]);
                        if (!valid(object, 0x60)) {
                            continue;
                        }
                        const bool player = i == 2;
                        const u64 error = object + (player ? 0x38 : 0x28);
                        state[2 + i * 4] = memory.Read8(object + (player ? 0x31 : 0x20));
                        state[3 + i * 4] = memory.Read32(error);
                        state[4 + i * 4] = memory.Read64(error + 8);
                        state[5 + i * 4] = memory.Read32(error + 0x18);
                    }
                    static std::optional<std::array<u64, 14>> previous_online;
                    static unsigned online_events = 0;
                    if (online_events < 128 && (!previous_online || *previous_online != state)) {
                        previous_online = state;
                        ++online_events;
                        LOG_INFO(Service_Audio,
                                 "[UNO diagnostic] Online initialized={} modules_initialized={} "
                                 "network_error={} network_code={} network_reason={} network_module={} "
                                 "platform_error={} platform_code={} platform_reason={} platform_module={} "
                                 "player_error={} player_code={} player_reason={} player_module={}",
                                 state[0], state[1], state[2], state[3], state[4], state[5],
                                 state[6], state[7], state[8], state[9], state[10], state[11],
                                 state[12], state[13]);
                    }
                }
            }
        }
    }
    const u64 type_slot = main_base + 0x80055A8;
    if (!memory.IsValidVirtualAddressRange(type_slot, 8)) {
        return;
    }
    const u64 type_info = memory.Read64(type_slot);
    if (!type_info || !memory.IsValidVirtualAddressRange(type_info, 0xC0)) {
        return;
    }
    const u64 fields = memory.Read64(type_info + 0xB8);
    if (!fields || !memory.IsValidVirtualAddressRange(fields, 12)) {
        return;
    }
    const std::array<u32, 4> state{memory.Read8(fields), memory.Read8(fields + 1),
                                  memory.Read32(fields + 4), memory.Read32(fields + 8)};
    if (state[0] > 1 || state[1] > 1 || state[2] > 3 || state[3] > 3) {
        return;
    }
    static std::optional<std::array<u32, 4>> previous;
    static unsigned events = 0;
    if (events < 128 && (!previous || *previous != state)) {
        previous = state;
        ++events;
        LOG_INFO(Service_Audio, "[UNO diagnostic] Storm initialized={} echo_initialized={} "
                               "punch_state={} echo_state={}",
                 state[0], state[1], state[2], state[3]);
    }
}

DeviceSession::DeviceSession(Core::System& system_)
    : system{system_}, thread_event{Core::Timing::CreateEvent(
                           "AudioOutSampleTick",
                           [this](s64 time, std::chrono::nanoseconds) { return ThreadFunc(); })} {}

DeviceSession::~DeviceSession() {
    Finalize();
}

Result DeviceSession::Initialize(std::string_view name_, SampleFormat sample_format_,
                                 u16 channel_count_, size_t session_id_, Kernel::KProcess* handle_,
                                 u64 applet_resource_user_id_, Sink::StreamType type_) {
    if (stream) {
        Finalize();
    }
    name = fmt::format("{}-{}", name_, session_id_);
    type = type_;
    sample_format = sample_format_;
    channel_count = channel_count_;
    session_id = session_id_;
    handle = handle_;
    handle->Open();
    applet_resource_user_id = applet_resource_user_id_;

    if (type == Sink::StreamType::In) {
        sink = &system.AudioCore().GetInputSink();
    } else {
        sink = &system.AudioCore().GetOutputSink();
    }
    stream = sink->AcquireSinkStream(system, channel_count, name, type);
    diagnostic_tick_count = 0;
    LOG_INFO(Service_Audio, "GenesisDiag Initialize session={} type={} carried_played={}",
             session_id, static_cast<u32>(type), played_sample_count.load());
    initialized = true;
    return ResultSuccess;
}

void DeviceSession::Finalize() {
    if (initialized) {
        Stop();
        sink->CloseStream(stream);
        stream = nullptr;
    }

    if (handle) {
        handle->Close();
        handle = nullptr;
    }
}

void DeviceSession::Start() {
    if (stream) {
        stream->Start();
        system.CoreTiming().ScheduleLoopingEvent(std::chrono::nanoseconds::zero(), INCREMENT_TIME,
                                                 thread_event);
    }
}

void DeviceSession::Stop() {
    if (stream) {
        stream->Stop();
        system.CoreTiming().UnscheduleEvent(thread_event, {});
    }
}

void DeviceSession::ClearBuffers() {
    if (stream) {
        stream->ClearQueue();
    }
}

void DeviceSession::AppendBuffers(std::span<const AudioBuffer> buffers) {
    for (const auto& buffer : buffers) {
        Sink::SinkBuffer new_buffer{
            .frames = buffer.size / (channel_count * sizeof(s16)),
            .frames_played = 0,
            .tag = buffer.tag,
            .consumed = false,
        };

        tmp_samples.resize_destructive(buffer.size / sizeof(s16));
        if (type == Sink::StreamType::In) {
            stream->AppendBuffer(new_buffer, tmp_samples);
        } else {
            Core::Memory::CpuGuestMemory<s16, Core::Memory::GuestMemoryFlags::UnsafeRead> samples(
                handle->GetMemory(), buffer.samples, buffer.size / sizeof(s16));
            stream->AppendBuffer(new_buffer, samples);
        }
    }
}

void DeviceSession::ReleaseBuffer(const AudioBuffer& buffer) const {
    if (type == Sink::StreamType::In) {
        auto samples{stream->ReleaseBuffer(buffer.size / sizeof(s16))};
        handle->GetMemory().WriteBlockUnsafe(buffer.samples, samples.data(), buffer.size);
    }
}

bool DeviceSession::IsBufferConsumed(const AudioBuffer& buffer) const {
    return played_sample_count >= buffer.end_timestamp;
}

void DeviceSession::SetVolume(f32 volume) const {
    if (stream) {
        stream->SetSystemVolume(volume);
    }
}

u64 DeviceSession::GetPlayedSampleCount() const {
    return played_sample_count;
}

std::optional<std::chrono::nanoseconds> DeviceSession::ThreadFunc() {
    played_sample_count = stream->GetExpectedPlayedSampleCount();
    if (type == Sink::StreamType::Out && diagnostic_tick_count % 40 == 0) {
        TraceUnoStormState(system, handle->GetMemory());
    }
    if (type == Sink::StreamType::Out && ++diagnostic_tick_count % 400 == 0) {
        LOG_INFO(Service_Audio, "GenesisDiag Tick session={} played={}", session_id,
                 played_sample_count.load());
    }
    if (type == Sink::StreamType::Out) {
        system.AudioCore().GetAudioManager().SetEvent(Event::Type::AudioOutManager, true);
    } else {
        system.AudioCore().GetAudioManager().SetEvent(Event::Type::AudioInManager, true);
    }
    return std::nullopt;
}

void DeviceSession::SetRingSize(u32 ring_size) {
    stream->SetRingSize(ring_size);
}

} // namespace AudioCore
