#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise actual friend IPC methods and name resolution with controlled boundaries."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", required=True)
parser.add_argument("--sanitizers", action="store_true")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]


def extract(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    if signature == "HLERequestContext::HLERequestContext(":
        brace = text.index(" {\n    cmd_buf[0]", start) + 1
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


friends = (repo / "src/core/hle/service/friend/friend.cpp").read_text(encoding="utf-8")
names = (repo / "src/common/nextendo_friends.cpp").read_text(encoding="utf-8")
ipc = (repo / "src/core/hle/service/hle_ipc.cpp").read_text(encoding="utf-8")
assert "ResolveName(pid, 0)" in friends, "Native profile IPC must not wait for network names"
handle = extract(friends, "    Result HandleSyncRequest(Kernel::KServerSession& session,")
event = extract(friends, "    void GetCompletionEvent(HLERequestContext& ctx)")
resolve = extract(names, "std::string ResolveName(u64 pid, int timeout_ms)")
constructor = extract(ipc, "HLERequestContext::HLERequestContext(")
destructor = extract(ipc, "HLERequestContext::~HLERequestContext()")
source = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
using u64 = std::uint64_t;
using Result = int;
#define LOG_DEBUG(...)
namespace Core::Memory { struct Memory {}; }
namespace Kernel {
struct KernelCore {};
struct KServerSession {};
struct KThread {
    int references = 1;
    void Open() { ++references; }
    void Close() { --references; }
};
}
struct HLERequestContext {
    explicit HLERequestContext(Kernel::KernelCore&, Core::Memory::Memory&,
                              Kernel::KServerSession*, Kernel::KThread*);
    ~HLERequestContext();
    Kernel::KServerSession* server_session;
    Kernel::KThread* thread;
    Kernel::KernelCore& kernel;
    Core::Memory::Memory& memory;
    std::array<int, 8> cmd_buf{};
    unsigned command = 0;
    bool deferred = false;
    bool GetIsDeferred() const { return deferred; }
    unsigned GetCommand() const { return command; }
};
namespace IPC {
struct ResponseBuilder {
    ResponseBuilder(HLERequestContext&, int, int) {}
    void Push(int) {}
    void PushCopyObjects(int) {}
};
}
constexpr int ResultSuccess = 0;
struct Event {
    bool signaled = false;
    void Signal() { signaled = true; }
    void Clear() { signaled = false; }
    int GetReadableEvent() { return 0; }
};
struct ServiceFrameworkBase {
    virtual Result HandleSyncRequest(Kernel::KServerSession&, HLERequestContext&) { return -7; }
};
struct Fixture : ServiceFrameworkBase {
    Event state;
    Event* completion_event = &state;
    // Retained solely so this test also compiles the old broken methods.
    bool is_async_session = false;
    unsigned handled_requests = 0;
    HANDLE_METHOD
    EVENT_METHOD
};
CONSTRUCTOR
DESTRUCTOR
std::mutex g_name_mutex;
std::function<std::string(u64)> g_name_resolver;
std::unordered_map<u64, std::shared_future<std::string>> g_names;
std::unordered_map<u64, std::chrono::steady_clock::time_point> g_name_attempts;
RESOLVE_METHOD
int main() {
    Kernel::KernelCore kernel;
    Kernel::KServerSession session;
    Core::Memory::Memory memory;
    Kernel::KThread thread;
    {
        HLERequestContext held(kernel, memory, &session, &thread);
        assert(thread.references == 2); // A queued/deferred request owns a reference.
        thread.Close();                 // Guest thread exits before its request completes.
        assert(thread.references == 1); // Its memory owner is still alive.
    }
    assert(thread.references == 0);
    HLERequestContext ctx(kernel, memory, &session, nullptr);
    Fixture first;
    first.GetCompletionEvent(ctx);
    assert(!first.state.signaled);
    ctx.command = 20106;
    first.HandleSyncRequest(session, ctx);
    assert(first.state.signaled); // Completion cannot depend on destroying this session.
    Fixture operation_first;
    operation_first.HandleSyncRequest(session, ctx);
    ctx.command = 0;
    operation_first.GetCompletionEvent(ctx);
    assert(operation_first.state.signaled);
    Fixture deferred;
    ctx.command = 20106;
    ctx.deferred = true;
    deferred.HandleSyncRequest(session, ctx);
    assert(!deferred.state.signaled);
    ctx.deferred = false;
    deferred.HandleSyncRequest(session, ctx);
    assert(deferred.state.signaled);
    ctx.deferred = true;
    deferred.HandleSyncRequest(session, ctx);
    assert(!deferred.state.signaled); // A reused event must not expose a previous completion.
    ctx.deferred = false;
    deferred.HandleSyncRequest(session, ctx);
    assert(deferred.state.signaled);
    Fixture subscription;
    ctx.command = 0;
    subscription.HandleSyncRequest(session, ctx);
    assert(!subscription.state.signaled);
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> calls{};
    g_name_resolver = [&](u64 pid) {
        ++calls;
        gate.wait();
        return pid == 9999 ? std::string{} : std::to_string(pid);
    };
    const auto start = std::chrono::steady_clock::now();
    assert(ResolveName(9999, 0).empty());
    for (u64 id = 1; id <= 300; ++id) assert(ResolveName(id, 0).empty());
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(750));
    assert(g_names.size() == 8); // An entire cold batch has a bounded number of requests.
    release.set_value();
    for (auto& [id, pending] : g_names) pending.wait();
    assert(calls == 8);
    assert(ResolveName(1, 0) == "1");
    for (int i = 0; i < 100; ++i) assert(ResolveName(9999, 0).empty());
    assert(calls == 8); // Failed lookups back off instead of retrying each frame.
}
'''
for marker, method in (("HANDLE_METHOD", handle), ("EVENT_METHOD", event),
                       ("RESOLVE_METHOD", resolve), ("CONSTRUCTOR", constructor),
                       ("DESTRUCTOR", destructor)):
    source = source.replace(marker, method)
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    cpp = root / "profile.cpp"
    binary = root / "profile-test.exe"
    cpp.write_text(source, encoding="utf-8")
    command = [args.compiler, "-std=c++20", "-pthread", str(cpp), "-o", str(binary)]
    if args.sanitizers:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print("PASS: live IPC completion, deferred thread lifetime, nonblocking bounded name cache")
