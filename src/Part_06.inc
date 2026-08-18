        hierarchy = h;
        return true;
    }

    static bool IsSkinnedPed(CPed* ped) {
        RpHAnimHierarchy* hierarchy = 0;
        return GetSkinnedPedHierarchy(ped, hierarchy);
    }

    static RwMatrix* GetSkinBoneMatrix(RpHAnimHierarchy* hierarchy, int boneId) {
        if (!hierarchy)
            return 0;
        const int index = RpHAnimIDGetIndex(hierarchy, boneId);
        if (index < 0 || index > 128)
            return 0;
        RwMatrix* matrices = GetHAnimMatrixArrayNative(hierarchy);
        if (!matrices)
            return 0;
        RwMatrix* matrix = &matrices[index];
        return IsReadableAddress(matrix, sizeof(RwMatrix)) && IsFiniteRwMatrix(*matrix) ? matrix : 0;
    }

    static bool NativeWeaponModelReady(CPed* ped) {
        return ped && GetNativeWeaponAtomicIdb(ped) != 0 && GetNativeWeaponModelIdIdb(ped) >= 0;
    }

    static RwFrame* GetPedFrameSafe(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 18)
            return 0;
        AnimBlendFrameData* data = GetPedFrameDataIdb(ped, index);
        if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
            return 0;
        RwFrame* frame = data->m_pFrame;
        if (!IsFrameReadable(frame))
            return 0;
        return frame;
    }

    static int GetPedFrameNodeIdSafe(CPed* ped, int index, int fallbackNodeId) {
        if (!ped || index < 0 || index >= 18)
            return fallbackNodeId;
        AnimBlendFrameData* data = GetPedFrameDataIdb(ped, index);
        if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
            return fallbackNodeId;
        const int nodeId = static_cast<int>(data->m_nNodeId);
        return nodeId > 0 ? nodeId : fallbackNodeId;
    }


    static AnimBlendFrameData** GetPedFrameSlotPtrIdb(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 18)
            return 0;
        const uintptr_t slot = reinterpret_cast<uintptr_t>(ped) + OFF_PED_FRAME_ARRAY + sizeof(void*) * static_cast<uintptr_t>(index);
        if (!IsReadableAddress(reinterpret_cast<const void*>(slot), sizeof(AnimBlendFrameData*)))
            return 0;
        return reinterpret_cast<AnimBlendFrameData**>(slot);
    }

    static AnimBlendFrameData* FindPedFrameDataByNodeId(CPed* ped, int nodeId) {
        if (!ped || nodeId <= 0)
            return 0;
        for (int i = 0; i < 18; ++i) {
            AnimBlendFrameData* data = GetPedFrameDataIdb(ped, i);
            if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
                continue;
            if (static_cast<int>(data->m_nNodeId) == nodeId)
                return data;
        }
        return 0;
    }

    static int MirrorArmBoneId(int nodeId, int fallback) {
        switch (nodeId) {
        case BONE_RCLAVICLE: return BONE_LCLAVICLE;
        case BONE_SUPPERARMR: return BONE_SUPPERARML;
        case BONE_SLOWERARMR: return BONE_SLOWERARML;
        case BONE_SRHAND: return BONE_SLHAND;
        case BONE_LCLAVICLE: return BONE_RCLAVICLE;
        case BONE_SUPPERARML: return BONE_SUPPERARMR;
        case BONE_SLOWERARML: return BONE_SLOWERARMR;
        case BONE_SLHAND: return BONE_SRHAND;
        default: return fallback;
        }
    }

    static AnimBlendFrameData* FindOppositeFrameForNativeSlot(CPed* ped, int nativeSlotIndex, int fallbackMirrorBone) {
        AnimBlendFrameData* native = GetPedFrameDataIdb(ped, nativeSlotIndex);
        int nodeId = fallbackMirrorBone;
        if (IsReadableAddress(native, sizeof(AnimBlendFrameData)))
            nodeId = MirrorArmBoneId(static_cast<int>(native->m_nNodeId), fallbackMirrorBone);
        AnimBlendFrameData* mirrored = FindPedFrameDataByNodeId(ped, nodeId);
        if (mirrored)
            return mirrored;
        return FindPedFrameDataByNodeId(ped, fallbackMirrorBone);
    }

    static bool ReadPedFloatIdb(CPed* ped, uintptr_t offset, float& out) {
        if (!ped)
            return false;
        const uintptr_t address = reinterpret_cast<uintptr_t>(ped) + offset;
        if (!IsReadableAddress(reinterpret_cast<const void*>(address), sizeof(float)))
            return false;
        out = *reinterpret_cast<float*>(address);
        return IsFiniteFloat(out);
    }

    static bool ReadPedVectorIdb(CPed* ped, uintptr_t offset, CVector& out) {
        if (!ped)
            return false;
        const uintptr_t address = reinterpret_cast<uintptr_t>(ped) + offset;
        if (!IsReadableAddress(reinterpret_cast<const void*>(address), sizeof(CVector)))
            return false;
        out = *reinterpret_cast<CVector*>(address);
        return IsFiniteVector(out);
    }

    static bool IsPedMovingForArmIK(CPed* ped) {
        if (!ped || gConfig.armIKWhileMoving || gConfig.armIKMoveSpeedLimit <= 0.0f)
            return false;
        CVector speed;
        if (!ReadPedVectorIdb(ped, OFF_PHYSICAL_MOVE_SPEED, speed))
            return false;
        const float limitSq = gConfig.armIKMoveSpeedLimit * gConfig.armIKMoveSpeedLimit;
        const float speedSq = speed.x * speed.x + speed.y * speed.y;
        return IsFiniteFloat(speedSq) && speedSq > limitSq;
    }

