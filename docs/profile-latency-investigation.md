# Native profile latency and exit crash

## Reproduction and evidence

The community tester reports minute-long blank MyPage transitions when entering,
leaving and reopening the profile. Soul also reproduced an emulator exit locally
with the beta54.3.2 binary. Windows recorded exception `c0000005`, module offset
`0x8f0049`. Symbol lookup on that exact binary locates the failing access in
`Core::Memory::Memory::Impl::WalkBlock` during `WriteBlockImpl`, dereferencing the
page-table pointer after MyPage exit. Service method/timing extraction places
`ExitProcessAndReturn` and applet pruning immediately before the exit.

A separate debugger session has not captured another exception yet. The
minute-long transition is not declared fully reproduced or solved on the tester's
machine. No account files or raw IPC buffers are included in this report.

## Changes in b1d7b6261

- `HLERequestContext` retains the client KThread until its request is destroyed.
  KThread already retains its owner KProcess. This keeps the memory object and
  page table alive for deferred replies instead of retaining a dangling pointer.
  The context cannot be copied, preventing duplicate release of that reference.
- Deferred requests from terminated clients are retired without dispatching a
  handler; reply writes reject terminated clients and propagate write failure.
- Friend completion events signal after the actual nondeferred command, including
  when the event was obtained first. Subscribing does not complete a job. Reusing
  a session clears the previous signal. Destruction is not the completion point.
- Native profile lookups read cached names immediately. The previous 1,500 ms
  wait per unknown ID could accumulate across a batch of up to 300 entries.
  At most eight name fetches remain pending; failures back off for 60 seconds.

## Verification

The added `profile-ipc-latency` test compiles the actual production IPC methods,
context constructor/destructor and name resolver with controlled boundaries. It
checks event ordering, deferred completion, reused events, thread retention after
client exit and a batch of 300 IDs behind a blocked resolver. The pre-fix context
fails the lifetime assertion. Windows passed the expanded 11-test suite and the
updated reused-event case. Linux normal and ASan/UBSan passed
[run 38766376345](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38766376345).
The signed-catalog checks passed
[run 38766376327](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38766376327).

Full emulator build and repeated native profile round trips are separate gates.
A unit pass is not a claim that the tester's complete freeze is fixed. The missing
HOME activity icon and native photo editor remain separate unresolved work.

## Windows candidate beta54.3.3

The complete Windows CLANG64 build of `b1d7b6261` passed all 332 build steps and
completed runtime deployment. Original developer files and stable executable
were restored with hashes checked. This deliberately includes the reviewed
request-lifetime fix above; unrelated build55 photo/icon experiments are absent.
Executable SHA-256: `48db169a7701af536bdaae59c10688259c6dec77248282417fef574348163b16`.
Clean runtime ZIP SHA-256: `739ee26766341cebddd56f71b5175a08a81c99a7c059421c538d4d6d2bd465c0`.
Archive content/integrity and compiled public desktop OAuth metadata checks passed.
The executable booted the native HOME lock screen. The owner completed the requested
three profile -> HOME -> profile cycles with their physical controller and confirmed
"Responde sin congelarse". No long wait or exit was reported during those cycles.
This is local owner acceptance; the community tester's separate machine should
repeat the same scenario. Photo editing and the missing HOME icon are not covered.
