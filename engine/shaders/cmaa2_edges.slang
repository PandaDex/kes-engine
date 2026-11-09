// CMAA2 pass 1: edge detection, shape candidates, clears the blend list heads.
#include "cmaa2.slang"

[shader("compute")]
[numthreads(CMAA2_CS_INPUT_KERNEL_SIZE_X, CMAA2_CS_INPUT_KERNEL_SIZE_Y, 1)]
void csMain(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    EdgesColor2x2CS(groupID, groupThreadID);
}
