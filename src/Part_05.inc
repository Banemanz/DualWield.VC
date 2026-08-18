    static bool IsFiniteVector(const CVector& v) {
        return IsFiniteFloat(v.x) && IsFiniteFloat(v.y) && IsFiniteFloat(v.z);
    }

    static bool IsFiniteRwVector(const RwV3d& v) {
        return IsFiniteFloat(v.x) && IsFiniteFloat(v.y) && IsFiniteFloat(v.z);
    }

    static bool IsFiniteRwMatrix(const RwMatrix& m) {
        return IsFiniteRwVector(m.right) && IsFiniteRwVector(m.up) &&
            IsFiniteRwVector(m.at) && IsFiniteRwVector(m.pos);
    }

    static bool IsReadableAddress(const void* ptr, size_t bytes = sizeof(void*)) {
        if (!ptr || bytes == 0)
            return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t finish = begin + bytes;
        const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        return finish >= begin && begin >= regionBegin && finish <= regionEnd;
    }

    static bool IsFrameReadable(RwFrame* frame) {
        // We only need enough of RwFrame to safely use its object/parent/modelling fields.
        return IsReadableAddress(frame, 0x50);
    }

    static RpClump* GetPedClumpIdb(CPed* ped) {
        if (!ped || !IsReadableAddress(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ped) + OFF_ENTITY_RW_CLUMP), sizeof(RpClump*)))
            return 0;
        return *reinterpret_cast<RpClump**>(reinterpret_cast<uintptr_t>(ped) + OFF_ENTITY_RW_CLUMP);
    }

    static AnimBlendFrameData* GetPedFrameDataIdb(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 18)
            return 0;
        const uintptr_t slot = reinterpret_cast<uintptr_t>(ped) + OFF_PED_FRAME_ARRAY + sizeof(void*) * static_cast<uintptr_t>(index);
        if (!IsReadableAddress(reinterpret_cast<const void*>(slot), sizeof(AnimBlendFrameData*)))
            return 0;
        return *reinterpret_cast<AnimBlendFrameData**>(slot);
    }

    static RpAtomic* GetNativeWeaponAtomicIdb(CPed* ped) {
        if (!ped || !IsReadableAddress(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_CUR_WEAPON_ATOMIC), sizeof(RpAtomic*)))
            return 0;
        return *reinterpret_cast<RpAtomic**>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_CUR_WEAPON_ATOMIC);
    }

    static int GetNativeWeaponModelIdIdb(CPed* ped) {
        if (!ped || !IsReadableAddress(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_WEAPON_MODEL_ID), sizeof(int)))
            return -1;
        return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_WEAPON_MODEL_ID);
    }

    static RwMatrix* GetHAnimMatrixArrayNative(RpHAnimHierarchy* hierarchy) {
        if (!hierarchy)
            return 0;
        RpHAnimHierarchyGetMatrixArrayFn getMatrixArray =
            reinterpret_cast<RpHAnimHierarchyGetMatrixArrayFn>(ADDR_RP_HANIM_HIERARCHY_GET_MATRIX_ARRAY);
        RwMatrix* matrices = getMatrixArray(hierarchy);
        return IsReadableAddress(matrices, sizeof(RwMatrix)) ? matrices : 0;
    }

    struct SkinProbe {
        RpAtomic* atomic;
        RpHAnimHierarchy* hierarchy;
        SkinProbe() : atomic(0), hierarchy(0) {}
    };

    static RpAtomic* FindSkinnedAtomicCB(RpAtomic* atomic, void* data) {
        SkinProbe* probe = reinterpret_cast<SkinProbe*>(data);
        if (!probe || !atomic)
            return atomic;
        RpGeometry* geometry = RpAtomicGetGeometry(atomic);
        if (!geometry)
            return atomic;
        RpSkin* skin = RpSkinGeometryGetSkin(geometry);
        if (!skin)
            return atomic;

        RpHAnimHierarchy* hierarchy = RpSkinAtomicGetHAnimHierarchy(atomic);
        if (!hierarchy)
            return atomic;

        probe->atomic = atomic;
        probe->hierarchy = hierarchy;
        return 0; // stop once the actual skinned body atomic is found
    }

    static bool GetSkinnedPedHierarchy(CPed* ped, RpHAnimHierarchy*& hierarchy) {
        hierarchy = 0;
        if (!ped || !GetPedClumpIdb(ped))
            return false;

        // This is the same helper CPed::Render calls at 0x4FE22C before rendering
        // m_pCurWeaponAtomic. The v3 port tried to infer the hierarchy from RpSkin
        // plugin data and missed stock Tommy, which forced the code into the RwFrame
        // fallback and made weapon creation defer forever.
        GetAnimHierarchyFromSkinClumpFn getHierarchy =
            reinterpret_cast<GetAnimHierarchyFromSkinClumpFn>(ADDR_GET_ANIM_HIERARCHY_FROM_SKIN_CLUMP);
        RpHAnimHierarchy* h = getHierarchy(GetPedClumpIdb(ped));
        if (!h)
            return false;
        // Do not trust SDK struct-member layout here. VC's own CPed::Render only needs
        // the hierarchy pointer, RpHAnimIDGetIndex and RpHAnimHierarchyGetMatrixArray.
        // v4 rejected valid stock hierarchies because it validated fields through the
        // Plugin-SDK layout before the stock render code had materialized them.
        if (!IsReadableAddress(h, 0x14))
            return false;
        if (!GetHAnimMatrixArrayNative(h))
            return false;

