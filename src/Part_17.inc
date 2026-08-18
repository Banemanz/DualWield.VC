        return true;
    }

    static bool ApplyCurrentV3Mirror(
        CPed* ped,
        EntityUpdateRpHAnimFn updateFn,
        CEntity* entity,
        float* outBlend = 0
    ) {
        if (outBlend)
            *outBlend = 0.0f;
        if (!gConfig.leftArmIK || !IsEligiblePlayer(ped) || !updateFn || !entity)
            return false;

        const float blend = ComputeSecondArmBlend(ped);
        if (blend <= 0.001f)
            return false;

        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false; // stock Tommy is skinned; do not mix the old RwFrame mirror into this path.

        const bool moved = MirrorNativeGunArmPoseIntoSecondArm(ped, hierarchy, updateFn, entity, blend);
        if (moved && outBlend)
            *outBlend = blend;
        if (moved && !gLoggedFirstNativeIkReplay) {
            const int s3 = GetPedFrameNodeIdSafe(ped, SecondUpperFrameIndex(), -1);
            const int s5 = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), -1);
            const int s13 = GetPedFrameNodeIdSafe(ped, SecondLowerFrameIndex(), -1);
            const int s15 = GetPedFrameNodeIdSafe(ped, SecondClavicleFrameIndex(), -1);
            const int n4 = GetPedFrameNodeIdSafe(ped, NativeUpperFrameIndex(), -1);
            const int n6 = GetPedFrameNodeIdSafe(ped, NativeHandFrameIndex(), -1);
            const int n14 = GetPedFrameNodeIdSafe(ped, NativeLowerFrameIndex(), -1);
            const int n16 = GetPedFrameNodeIdSafe(ped, NativeClavicleFrameIndex(), -1);
            char line[512];
            std::snprintf(line, sizeof(line),
                "DualWieldVC v12: quaternion pose mirror active native slots 4/14/6/16 nodes=%d/%d/%d/%d -> second slots 3/13/5/15 nodes=%d/%d/%d/%d blend=%.2f.",
                n4, n14, n6, n16, s3, s13, s5, s15, blend);
            Log(line);
            gLoggedFirstNativeIkReplay = true;
        }
        return moved;
    }

    static void ApplyPreRenderArmPose(CPed* ped, EntityUpdateRpHAnimFn updateFn, CEntity* entity) {
        // VC has already baked its native body pose once when this bridge is entered.
        // v12 rotates the opposite arm's interpolation quaternions, and each correction
        // is followed by VC's own UpdateRpHAnim so the skin matrix array stays coherent.
        const unsigned int frame = CTimer::m_FrameCounter;
        if (gLastArmPoseFrame == frame)
            return;
        gLastArmPoseFrame = frame;

        float blend = 0.0f;
        if (!ApplyCurrentV3Mirror(ped, updateFn, entity, &blend))
            return;

        ++gRenderMirrorHits;
        if (gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();

        if (!gLoggedFirstRenderMirror) {
            RpHAnimHierarchy* hierarchy = 0;
            const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);
            char line[384];
            std::snprintf(line, sizeof(line),
                "DualWieldVC v12: post-quaternion UpdateRpHAnim pose active second=slot5/SLhand blend=%.2f skinned=%d hierarchy=%p.",
                blend, skinned ? 1 : 0, hierarchy);
            Log(line);
            gLoggedFirstRenderMirror = true;
        }
    }

    static void PrepareOppositePoseForFire(CPed* ped) {
        // Do not run body posing from the attack hook: CPed::Attack may call the fire
        // hook more than once per frame, and body posing now belongs to the post-
        // UpdateRpHAnim bridge. Keep this function to synchronize the private weapon
        // transform before the selected hand muzzle is sampled.
        if (ped && gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();
    }

    static bool IsExecutablePointerLocal(const void* pointer) {
        if (!pointer)
            return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(pointer, &mbi, sizeof(mbi)))
            return false;
        const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.State == MEM_COMMIT) && ((mbi.Protect & executable) != 0) && !(mbi.Protect & PAGE_GUARD);
    }

    static bool RenderAtomicViaVcCallback(RpAtomic* atomic) {
        if (!atomic || !IsReadableAddress(atomic, 0x4C))
            return false;

        // Vice City CPed::Render does not call a generic wrapper here. At 0x4FE2CB it
        // executes: push atomic; call dword ptr [atomic + 0x48]. Use the same callback
        // slot for the standalone left-hand clone so custom weapon pipelines/shader
        // callbacks see a normal weapon atomic render, not a synthetic RenderWare call.
        void* callback = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(atomic) + 0x48);
        if (!IsExecutablePointerLocal(callback))
            return false;
        typedef void(__cdecl* AtomicRenderCallback)(RpAtomic*);
        reinterpret_cast<AtomicRenderCallback>(callback)(atomic);
        return true;
    }

