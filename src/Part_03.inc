    static void BuildSiblingPath(char* out, size_t outSize, const char* filename) {
        if (!out || outSize == 0)
            return;
        out[0] = '\0';

        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, MAX_PATH);
        char* slashA = std::strrchr(modulePath, '\\');
        char* slashB = std::strrchr(modulePath, '/');
        char* slash = slashA;
        if (!slash || (slashB && slashB > slash))
            slash = slashB;
        if (slash)
            *(slash + 1) = '\0';
        else
            modulePath[0] = '\0';

        std::snprintf(out, outSize, "%s%s", modulePath, filename);
        out[outSize - 1] = '\0';
    }

    static void Log(const char* text) {
        if (!text || !gLogPath[0])
            return;
        FILE* f = std::fopen(gLogPath, "a");
        if (!f)
            return;
        std::fprintf(f, "%s\n", text);
        std::fclose(f);
    }

    static bool ReadBool(const char* key, bool fallback) {
        return GetPrivateProfileIntA("DualWield", key, fallback ? 1 : 0, gIniPath) != 0;
    }

    static float ReadFloat(const char* key, float fallback) {
        char fallbackText[64] = {};
        char value[64] = {};
        std::snprintf(fallbackText, sizeof(fallbackText), "%.6f", fallback);
        GetPrivateProfileStringA("DualWield", key, fallbackText, value, sizeof(value), gIniPath);
        float parsed = fallback;
        if (std::sscanf(value, "%f", &parsed) != 1)
            return fallback;
        return parsed;
    }

    static void LoadConfig() {
        gConfig.enabled = ReadBool("Enabled", true);
        gConfig.pistol = ReadBool("Pistol", true);
        gConfig.uzi = ReadBool("Uzi", true);
        gConfig.tec9 = ReadBool("Tec9", true);
        gConfig.doubleShot = ReadBool("DoubleShot", true);
        // v12: fixed VC ownership split from the IDB, plus an SA-style local-quaternion
        // pose mirror. The second side is not configurable because stock CPed::Render
        // hardwires the native weapon to frame slot 6 / HAnim 24.
        gConfig.leftArmIK = ReadBool("QuaternionPoseMirror", true);
        gConfig.attachCloneToActualHand = ReadBool("AttachCloneToActualHand", true);
        gConfig.secondHandRight = false; // IDB: stock VC weapon is hardwired to m_apFrames[6]/HAnim 24; second side is slot 5/HAnim 34.
        gConfig.mirrorWeaponOnly = false;
        gConfig.armIKRequiresFire = ReadBool("ArmIKRequiresFire", false);
        gConfig.armIKWhileMoving = ReadBool("ArmIKWhileMoving", true);
        gConfig.armIKMoveSpeedLimit = ReadFloat("ArmIKMoveSpeedLimit", 0.20f);
        if (gConfig.armIKMoveSpeedLimit < 0.0f) gConfig.armIKMoveSpeedLimit = 0.0f;
        if (gConfig.armIKMoveSpeedLimit > 0.50f) gConfig.armIKMoveSpeedLimit = 0.50f;
        gConfig.armAimBlend = ReadFloat("MirrorAimBlend", 1.00f);
        gConfig.armHoldBlend = ReadFloat("MirrorHoldBlend", 0.90f);
        gConfig.armReloadBlend = ReadFloat("MirrorReloadBlend", 0.90f);
        gConfig.armReachScale = ReadFloat("ArmReachScale", 0.92f);
        gConfig.armSideBias = ReadFloat("ArmSideBias", 0.02f);
        gConfig.armUpBias = ReadFloat("ArmUpBias", 0.02f);
        gConfig.aimPitchScale = ReadFloat("AimPitchScale", 1.0f);
        gConfig.aimPitchOffset = ReadFloat("AimPitchOffset", 0.0f);
        if (gConfig.armAimBlend < 0.0f) gConfig.armAimBlend = 0.0f;
        if (gConfig.armAimBlend > 1.0f) gConfig.armAimBlend = 1.0f;
        if (gConfig.armHoldBlend < 0.0f) gConfig.armHoldBlend = 0.0f;
        if (gConfig.armHoldBlend > 1.0f) gConfig.armHoldBlend = 1.0f;
        if (gConfig.armReloadBlend < 0.0f) gConfig.armReloadBlend = 0.0f;
        if (gConfig.armReloadBlend > 1.0f) gConfig.armReloadBlend = 1.0f;
        if (gConfig.armReachScale < 0.50f) gConfig.armReachScale = 0.50f;
        if (gConfig.armReachScale > 1.15f) gConfig.armReachScale = 1.15f;
        gConfig.chainExistingCallHooks = ReadBool("ChainExistingCallHooks", true);
        gConfig.aimGraceFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimGraceFrames", 3, gIniPath));
        if (gConfig.aimGraceFrames < 0) gConfig.aimGraceFrames = 0;
        if (gConfig.aimGraceFrames > 12) gConfig.aimGraceFrames = 12;
        gConfig.aimBlendFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimBlendFrames", 2, gIniPath));
        if (gConfig.aimBlendFrames < 0) gConfig.aimBlendFrames = 0;
        if (gConfig.aimBlendFrames > 8) gConfig.aimBlendFrames = 8;
        gConfig.offsetX = ReadFloat("LeftGripOffsetX", 0.04f);
        gConfig.offsetY = ReadFloat("LeftGripOffsetY", -0.05f);
        gConfig.offsetZ = ReadFloat("LeftGripOffsetZ", 0.0f);
        gConfig.rotX = ReadFloat("LeftGripRotationX", 180.0f);
        gConfig.rotY = ReadFloat("LeftGripRotationY", 0.0f);
        gConfig.rotZ = ReadFloat("LeftGripRotationZ", 0.0f);
    }

