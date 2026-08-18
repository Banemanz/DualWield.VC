        EntityUpdateRpHAnimFn updateFn = reinterpret_cast<EntityUpdateRpHAnimFn>(gPreRenderAnimPatch.previousTarget);
        if (updateFn)
            updateFn(entity);

        // SA-style backport layer: VC first bakes its normal one-gun body pose, then
        // we solve the selected second arm against that finished pose before CPed::Render.
        // This avoids poisoning VC's native CPedIK state and avoids the v8/v9 slot-remap mess.
        if (ped && ped->m_nPedType == PEDTYPE_PLAYER1 && updateFn)
            ApplyPreRenderArmPose(ped, updateFn, entity);
    }

    static bool gRenderHookResolved = false;

    static bool EnsureRenderBridgeInstalled() {
        if (gRenderPatch.installed)
            return true;
        if (gRenderHookResolved)
            return gRenderPatch.installed;
        gRenderHookResolved = true;

        uintptr_t target = 0; int32_t rel = 0;
        const bool decoded = DecodeRelativeCall(ADDR_PED_RENDER_ENTITY_CALL, target, rel) && IsExecutableAddress(target);
        const bool ok = decoded && InstallCallPatch(ADDR_PED_RENDER_ENTITY_CALL, ADDR_ENTITY_RENDER,
            reinterpret_cast<void*>(&PedRenderBridge), gRenderPatch);

        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v12: render bridge call=%p target=%p %s.",
            reinterpret_cast<void*>(ADDR_PED_RENDER_ENTITY_CALL), reinterpret_cast<void*>(target), ok ? "HOOKED" : "MISS");
        Log(line);
        return ok;
    }

    static bool gPreRenderHookResolved = false;

    static bool EnsurePreRenderArmBridgeInstalled() {
        if (gPreRenderAnimPatch.installed)
            return true;
        if (gPreRenderHookResolved)
            return gPreRenderAnimPatch.installed;
        gPreRenderHookResolved = true;

        uintptr_t target = 0; int32_t rel = 0;
        const bool decoded = DecodeRelativeCall(ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL, target, rel) && IsExecutableAddress(target);
        const bool ok = decoded && InstallCallPatch(ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL, ADDR_ENTITY_UPDATE_RPHANIM,
            reinterpret_cast<void*>(&PedPreRenderUpdateRpHAnimBridge), gPreRenderAnimPatch);

        char line[360];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v12: PreRender UpdateRpHAnim bridge call=%p target=%p %s.",
            reinterpret_cast<void*>(ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL), reinterpret_cast<void*>(target), ok ? "HOOKED" : "MISS");
        Log(line);
        if (!ok)
            Log("DualWieldVC v12: no PreRender arm bridge; body arm pose will not be visible.");
        return ok;
    }

    static void LogRuntimeSummary() {
        if (gLoggedRuntimeSummary)
            return;
        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v12 summary: render mirrors=%u second-shot attempts=%u success=%u skinnedMode=%d.",
            gRenderMirrorHits, gSecondShotAttempts, gSecondShotSuccess,
            gLoggedSkinnedPedMode ? 1 : 0);
        Log(line);
        gLoggedRuntimeSummary = true;
    }

    static void OnPedDestroy(CPed* ped) {
        if (ped && ped == gLeft.owner)
            DestroyLeftWeapon();
    }

    static void OnPedSetModel(CPed* ped) {
        if (!ped || ped != gLeft.owner)
            return;
        Log("DualWieldVC v12: pedSetModelEvent observed; destroying standalone second weapon before rebinding frames.");
        DestroyLeftWeapon();
    }

    class Mod {
    public:
        Mod() {
            BuildSiblingPath(gIniPath, sizeof(gIniPath), "DualWieldVC.ini");
            BuildSiblingPath(gLogPath, sizeof(gLogPath), "DualWieldVC_v12.log");
            std::remove(gLogPath);
            LoadConfig();

            Log("DualWieldVC v12: loaded. Pistol/Uzi/Tec-9 enabled; SA-style per-hand muzzle and local-quaternion second-arm mirror active.");
            Log("DualWieldVC v12: fire hooks, PreRender arm bridge and render weapon bridge resolve on first gameProcess tick.");

            Events::gameProcessEvent += [] {
                EnsureFireHooksInstalled();
                EnsurePreRenderArmBridgeInstalled();
                EnsureRenderBridgeInstalled();
                };
            Events::pedSetModelEvent += [](CPed* ped, int) {
                OnPedSetModel(ped);
                };
            Events::pedDtorEvent += [](CPed* ped) {
                OnPedDestroy(ped);
                };
            Events::restartGameEvent += [] {
                DestroyLeftWeapon();
                };
            Events::shutdownRwEvent += [] {
                LogRuntimeSummary();
                DestroyLeftWeapon();
                RestoreCallPatch(gFirePatch1);
                RestoreCallPatch(gFirePatch2);
                RestoreCallPatch(gRenderPatch);
                RestoreCallPatch(gPreRenderAnimPatch);
                };
        }

