        uintptr_t target1 = 0, target2 = 0; int32_t rel1 = 0, rel2 = 0;
        const bool dec1 = DecodeRelativeCall(call1, target1, rel1) && IsExecutableAddress(target1);
        const bool dec2 = DecodeRelativeCall(call2, target2, rel2) && IsExecutableAddress(target2);
        const bool ok1 = dec1 && InstallCallPatch(call1, target1, reinterpret_cast<void*>(&AttackFireHook1), gFirePatch1);
        const bool ok2 = dec2 && InstallCallPatch(call2, target2, reinterpret_cast<void*>(&AttackFireHook2), gFirePatch2);

        char line[512];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v12: fire resolve call1=%p target=%p %s; call2=%p target=%p %s.",
            reinterpret_cast<void*>(call1), reinterpret_cast<void*>(target1), ok1 ? "HOOKED" : "MISS",
            reinterpret_cast<void*>(call2), reinterpret_cast<void*>(target2), ok2 ? "HOOKED" : "MISS");
        Log(line);
        if (!ok1 && !ok2) {
            Log("DualWieldVC v12: no CPed::Attack fire hooks installed; second-shot path disabled.");
            gConfig.doubleShot = false;
        }
        return ok1 || ok2;
    }

    static bool InstallCallPatch(uintptr_t address, uintptr_t expectedTarget, void* hook, CallPatch& patch) {
        uintptr_t currentTarget = 0;
        int32_t originalRel = 0;
        if (!DecodeRelativeCall(address, currentTarget, originalRel))
            return false;

        const uintptr_t hookTarget = reinterpret_cast<uintptr_t>(hook);
        if (!hookTarget || currentTarget == hookTarget)
            return false;

        const bool chained = currentTarget != expectedTarget;
        if (chained) {
            if (!gConfig.chainExistingCallHooks || !IsExecutableAddress(currentTarget))
                return false;
        }

        const intptr_t delta = hookTarget - (address + 5);
        if (delta < INT32_MIN || delta > INT32_MAX)
            return false;
        const int32_t newRel = static_cast<int32_t>(delta);

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        std::memcpy(reinterpret_cast<void*>(address + 1), &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 5);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), 5, oldProtect, &ignored);

        patch.address = address;
        patch.previousTarget = currentTarget;
        patch.hookTarget = hookTarget;
        patch.originalRel = originalRel;
        patch.installed = true;
        patch.chained = chained;
        return true;
    }

    static void RestoreCallPatch(CallPatch& patch) {
        if (!patch.installed)
            return;

        // If another ASI patched this CALL after us, leave its newer hook intact.
        uintptr_t currentTarget = 0;
        int32_t currentRel = 0;
        if (!DecodeRelativeCall(patch.address, currentTarget, currentRel) ||
            currentTarget != patch.hookTarget) {
            patch = CallPatch();
            return;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(patch.address), 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(reinterpret_cast<void*>(patch.address + 1), &patch.originalRel, sizeof(patch.originalRel));
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patch.address), 5);
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(patch.address), 5, oldProtect, &ignored);
        }
        patch = CallPatch();
    }

    static void RunPedRenderBridgeWork(CPed* ped) {
        if (!ped || ped->m_nPedType != PEDTYPE_PLAYER1)
            return;
        UpdateLeftWeapon(ped);
    }

    static void __fastcall PedRenderBridge(CEntity* entity, void*) {
        CPed* ped = reinterpret_cast<CPed*>(entity);

        // Body arm posing must NOT happen here. By this point CPed::PreRender has
        // already called CEntity::UpdateRpHAnim and materialized the skinned body
        // matrices. v7 edited the wrong thing at this phase, which left the visible
        // arm unchanged. This bridge is now only responsible for drawing the
        // standalone second weapon after the body draw.
        EntityRenderFn renderFn = reinterpret_cast<EntityRenderFn>(gRenderPatch.previousTarget);
        if (renderFn)
            renderFn(entity);

        if (ped && ped->m_nPedType == PEDTYPE_PLAYER1) {
            UpdateLeftWeapon(ped);
            RenderStandaloneLeftWeapon(ped);
        }
    }

    static void __fastcall PedPreRenderUpdateRpHAnimBridge(CEntity* entity, void*) {
        CPed* ped = reinterpret_cast<CPed*>(entity);

