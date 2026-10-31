Native fork binding and commit are the next integration step, not part of
VM116 acceptance. A local draft is in lx-sandbox/pid-staging. It has not yet
been copied into the authoritative source or tested.

common/os/lwp.c calls b_lwpdata_alloc before taking p_lock, so native PID
binding can fail there and use normal fork/LWP allocation unwind. The
native PID object already exists, but the lx task hash does not. The current
registered-task publication adapter therefore needs a native-reservation
binding entry point for this path.

b_initlwp cannot drop p_lock. b_initlwp_post enters with p_lock held but may
temporarily drop it, according to the native source comment. Do not assume
the post hook is called without that lock. b_forklwp is before a final
contract_process_fork failure point and cannot commit PID1 irreversibly.

common/os/fork.c marks the end of all fork failure points after successful
contract_process_fork. Later it calls b_lwp_setrval on the child, without
pidlock or either process lock held, before making the child runnable.
lx_setrval already implements this callback. Use it to commit prepared PID
state. A failed fork must leave an unpublished/bound initial map retryable.

The staging draft separates ID reservation, binding the owned native PID
and successful commit. Bound entries stay hidden until commit. Commit must
report ancestor namespace death; the future caller must kill that child
before user execution if the namespace died after binding. Committed maps
must remain available for ancestor wait and zombie reporting. Test this
race and native allocation failure before exposing NEWPID.

Next, add process active/for-children namespace references and a retained
process PID map, plus thread PID maps and parent-owned pending fork state.
Keep the parent's map reference until fork return, even if the child exits
quickly. Native clearbrand runs both on failed construction and at reap;
freeproc holds pidlock. Worker retirement and nonleader exec need different
map lifetimes from process exit/reap. Existing native signal, wait and
orphan adoption should remain the underlying engines.
