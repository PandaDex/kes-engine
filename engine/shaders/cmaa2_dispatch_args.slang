// CMAA2 passes 2 and 4: indirect dispatch arguments. Dispatch(2, 1, 1) before
// ProcessCandidates, Dispatch(1, 2, 1) before DeferredApply (also resets the counters).
#include "cmaa2.slang"

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 groupID : SV_GroupID)
{
    ComputeDispatchArgsCS(groupID);
}
