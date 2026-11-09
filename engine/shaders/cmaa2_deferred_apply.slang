// CMAA2 pass 5: averages the blend list of each touched pixel and writes it to the color target.
#include "cmaa2.slang"

[shader("compute")]
#if CMAA2_DEFERRED_APPLY_THREADGROUP_SWAP
[numthreads(4, CMAA2_DEFERRED_APPLY_NUM_THREADS, 1)]
#else
[numthreads(CMAA2_DEFERRED_APPLY_NUM_THREADS, 4, 1)]
#endif
void csMain(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID)
{
    DeferredColorApply2x2CS(dispatchThreadID, groupThreadID);
}
