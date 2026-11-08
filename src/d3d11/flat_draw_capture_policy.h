#pragma once
// Pure F10 admission policy. No D3D state reads or allocation outside an arm.
#include <cstdint>

namespace edvr {
// Investigation candidate, not a declaration that every draw is window glass.
// Used only by explicitly armed captures; never by rendering admission.
inline bool flatDrawColourDecalCandidate(uint64_t vs,uint64_t ps) {
    return vs==0x5B4D8E894EEDA8B4ull && ps==0x4375B72964F386CDull;
}
struct FlatDrawAdmission {
    static constexpr unsigned kQuota=256, kStride=64, kInitial=8;
    unsigned sceneEligible=0, motionEligible=0, sceneRecorded=0, motionRecorded=0;
    unsigned sceneSkipped=0, motionSkipped=0, sceneQuotaSkipped=0, motionQuotaSkipped=0;
    unsigned poolCandidates[2]{},poolSelected[2]{},poolSkipped[2]{},poolQuotaSkipped[2]{};
    unsigned priorityEligible=0,priorityRecorded=0,priorityQuotaSkipped=0;

    bool consider(bool motion,uint64_t frame,bool priority=false) {
        if(priority)++priorityEligible;
        unsigned& eligible=motion?motionEligible:sceneEligible;
        unsigned& recorded=motion?motionRecorded:sceneRecorded;
        unsigned& skipped=motion?motionSkipped:sceneSkipped;
        unsigned& quotaSkipped=motion?motionQuotaSkipped:sceneQuotaSkipped;
        const unsigned ordinal=++eligible;
        const unsigned phase=(frame&1)?0:kStride/2;
        if(!priority&&ordinal>kInitial&&(ordinal+phase)%kStride!=0){++skipped;return false;}
        if(recorded>=kQuota){++quotaSkipped;if(priority)++priorityQuotaSkipped;return false;}
        if(priority)++priorityRecorded;
        ++recorded;return true;
    }
    bool considerPool(unsigned family) {
        const unsigned ordinal=++poolCandidates[family];
        if(ordinal!=1&&ordinal%32!=0){++poolSkipped[family];return false;}
        if(poolSelected[family]>=8){++poolQuotaSkipped[family];return false;}
        ++poolSelected[family];return true;
    }
};
}
