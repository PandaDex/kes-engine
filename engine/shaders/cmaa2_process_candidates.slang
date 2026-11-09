// CMAA2 pass 3: simple and complex (Z) shape blending into the deferred blend lists.
#include "cmaa2.slang"

[shader("compute")]
[numthreads(CMAA2_PROCESS_CANDIDATES_NUM_THREADS, 1, 1)]
void csMain(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID)
{
    ProcessCandidatesCS(dispatchThreadID, groupThreadID);
}
