        gLeft.owner = ped;
        gLeft.ownerClump = GetPedClumpIdb(ped);
        gLeft.atomic = atomic;
        gLeft.helperFrame = helper;
        gLeft.sourceHandFrame = sourceHand;
        gLeft.skinnedSource = skinned;
        gLeft.modelId = GetNativeWeaponModelIdIdb(ped);
        gLeft.weaponType = weapon->m_eWeaponType;

        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v12: cloned opposite weapon type=%d model=%d native=%p clone=%p mode=%s.",
            static_cast<int>(weapon->m_eWeaponType), GetNativeWeaponModelIdIdb(ped),
            nativeAtomic, atomic, skinned ? ("slot5/SLhand HAnim") : ("slot5/SLhand RwFrame"));
        Log(line);
        return true;
    }

    static void LogCreateFailureOccasionally(CPed* ped, CWeapon* weapon, bool skinned) {
        const unsigned int frame = CTimer::m_FrameCounter;
        if (gLoggedCreateFailure && frame - gCreateFailureFrame < 300)
            return;
        gLoggedCreateFailure = true;
        gCreateFailureFrame = frame;

        char line[512];
        RpHAnimHierarchy* hierarchy = 0;
        const bool hasHierarchy = GetSkinnedPedHierarchy(ped, hierarchy);
        const void* nativeWeapon = ped ? reinterpret_cast<void*>(GetNativeWeaponAtomicIdb(ped)) : 0;
        std::snprintf(line, sizeof(line),
            "DualWieldVC v12: create deferred. ped=%p state=%d inVeh=%d weapon=%d modelField=%d cloneModel=%d skinned=%d hierarchy=%p nativeWeapon=%p.",
            ped, ped ? static_cast<int>(ped->m_ePedState) : -1,
            ped ? static_cast<int>(ped->m_bInVehicle) : -1,
            weapon ? static_cast<int>(weapon->m_eWeaponType) : -1,
            ped ? GetNativeWeaponModelIdIdb(ped) : -999,
            ped ? GetNativeWeaponModelIdIdb(ped) : -999,
            skinned ? 1 : 0, hasHierarchy ? hierarchy : 0, nativeWeapon);
        Log(line);
    }

    static void UpdateLeftWeapon(CPed* ped) {
        if (!IsEligiblePlayer(ped)) {
            if (gLeft.owner == ped)
                DestroyLeftWeapon();
            return;
        }

        if (gLeft.owner && gLeft.owner != ped)
            DestroyLeftWeapon();

        if (gLeft.owner == ped && gLeft.ownerClump && gLeft.ownerClump != GetPedClumpIdb(ped))
            DestroyLeftWeapon();

        CWeapon* weapon = ped->GetWeapon();
        const int modelId = GetNativeWeaponModelIdIdb(ped);
        RpHAnimHierarchy* hierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);

        if (!weapon || !NativeWeaponModelReady(ped)) {
            DestroyLeftWeapon();
            LogCreateFailureOccasionally(ped, weapon, skinned);
            return;
        }

        if (IsAtomicAliveForOwner(ped)) {
            if (gLeft.weaponType != weapon->m_eWeaponType ||
                gLeft.modelId != modelId || gLeft.skinnedSource != skinned) {
                DestroyLeftWeapon();
            } else if (!skinned) {
                RwFrame* currentSourceHand = GetPedFrameSafe(ped, SecondHandFrameIndex());
                if (!currentSourceHand || gLeft.sourceHandFrame != currentSourceHand)
                    DestroyLeftWeapon();
                else {
                    UpdateLeftWeaponWorldTransform();
                    return;
                }
            } else {
                const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
                if (!hierarchy || !GetSkinBoneMatrix(hierarchy, handNode))
                    DestroyLeftWeapon();
                else {
                    UpdateLeftWeaponWorldTransform();
                    return;
                }
            }
        } else if (gLeft.atomic || gLeft.helperFrame) {
            DestroyLeftWeapon();
        }

        if (!CreateLeftWeapon(ped))
            LogCreateFailureOccasionally(ped, weapon, skinned);
    }

    struct Basis3 {
        CVector right;
        CVector up;
        CVector at;
    };

    struct MirroredArmChain {
        bool valid;
        RwFrame* nativeUpper;
        RwFrame* nativeLower;
        RwFrame* nativeHand;
        RwFrame* mirrorUpper;
        RwFrame* mirrorLower;
        RwFrame* mirrorHand;

        MirroredArmChain()
            : valid(false), nativeUpper(0), nativeLower(0), nativeHand(0),
            mirrorUpper(0), mirrorLower(0), mirrorHand(0) {
        }
    };

    static CVector Vec3(float x, float y, float z) {
        CVector v;
        v.x = x;
        v.y = y;
        v.z = z;
        return v;
    }

    static float Dot3(const CVector& a, const CVector& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static CVector Add3(const CVector& a, const CVector& b) {
        return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
    }

