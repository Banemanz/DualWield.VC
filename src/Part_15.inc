    static CVector Lerp3(const CVector& a, const CVector& b, float t) {
        return Add3(a, Scale3(Sub3(b, a), ClampFloat(t, 0.0f, 1.0f)));
    }

    static float Distance3(const CVector& a, const CVector& b) {
        const CVector d = Sub3(a, b);
        const float lenSq = Dot3(d, d);
        if (!IsFiniteFloat(lenSq) || lenSq <= 0.0f)
            return 0.0f;
        return std::sqrt(lenSq);
    }

    static CVector GetPedRightSafe(CPed* ped) {
        if (!ped)
            return Vec3(1.0f, 0.0f, 0.0f);
        CVector r = ped->GetRight();
        r.z = 0.0f;
        return Normalize3(r, Vec3(1.0f, 0.0f, 0.0f));
    }

    static CVector GetAimDirectionWorld(CPed* ped) {
        float aimAngle = 0.0f;
        float aimSlope = 0.0f;
        if (ReadPedFloatIdb(ped, OFF_PED_AIM_ANGLE, aimAngle)) {
            ReadPedFloatIdb(ped, OFF_PED_AIM_SLOPE, aimSlope);
            aimSlope = aimSlope * gConfig.aimPitchScale + gConfig.aimPitchOffset;
            if (aimSlope > 1.20f) aimSlope = 1.20f;
            if (aimSlope < -1.20f) aimSlope = -1.20f;
            const float c = std::cos(aimSlope);
            return Normalize3(Vec3(-std::sin(aimAngle) * c, std::cos(aimAngle) * c, std::sin(aimSlope)), Vec3(0.0f, 1.0f, 0.0f));
        }

        // Fallback from the ped's right vector if the raw aim angle is not readable.
        const CVector right = GetPedRightSafe(ped);
        return Normalize3(Vec3(-right.y, right.x, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    }

    static bool IsReloadingActiveWeapon(CPed* ped) {
        CWeapon* weapon = ped ? ped->GetWeapon() : 0;
        return weapon && (weapon->m_eWeaponState == WEAPONSTATE_RELOADING || weapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO);
    }

    static float ComputeSecondArmBlend(CPed* ped) {
        if (!ped || !gConfig.leftArmIK)
            return 0.0f;

        const bool activeAim = ped->bFiringWeapon || ped->bIsAimingGun || ped->bIsPointingGunAt ||
            ped->m_ePedState == PEDSTATE_ATTACK || ped->m_ePedState == PEDSTATE_AIMGUN || gAimGraceFramesRemaining > 0;
        if (gConfig.armIKRequiresFire && !activeAim)
            return 0.0f;
        if (!gConfig.armIKWhileMoving && IsPedMovingForArmIK(ped))
            return gConfig.armHoldBlend;
        if (IsReloadingActiveWeapon(ped))
            return gConfig.armReloadBlend;
        return activeAim ? gConfig.armAimBlend : gConfig.armHoldBlend;
    }

    // IDB-backed layout used by VC's own CPedIK::PointGunInDirectionUsingArm:
    // AnimBlendFrameData + 0x10 -> skinned HAnim interpolation frame,
    // interpolation frame + 0x08 -> RtQuat orientation. CPedIK rotates those quats
    // for the native arm, then CEntity::UpdateRpHAnim bakes them into pMatrixArray.
    struct RawRtQuat {
        float x, y, z, w; // RtQuat: imag.xyz + real
    };

    static RawRtQuat* GetPedAnimQuatIdb(CPed* ped, int frameIndex) {
        AnimBlendFrameData* data = GetPedFrameDataIdb(ped, frameIndex);
        if (!IsReadableAddress(data, 0x18))
            return 0;
        void* interp = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(data) + 0x10);
        if (!IsReadableAddress(interp, 0x18))
            return 0;
        RawRtQuat* q = reinterpret_cast<RawRtQuat*>(reinterpret_cast<uintptr_t>(interp) + 0x08);
        return IsReadableAddress(q, sizeof(RawRtQuat)) ? q : 0;
    }

    static CVector WorldAxisToParentLocal(const CVector& axisWorld, const RwMatrix& parentWorld) {
        // SA's IKChain::MoveBonesToTarget transforms the world correction axis by the
        // inverse parent rotation before post-concatenating it into the local keyframe
        // quaternion. For an orthonormal RW basis, dotting against parent basis vectors
        // is the same inverse-rotation operation.
        return Normalize3(Vec3(
            Dot3(axisWorld, Vec3(parentWorld.right.x, parentWorld.right.y, parentWorld.right.z)),
            Dot3(axisWorld, Vec3(parentWorld.up.x, parentWorld.up.y, parentWorld.up.z)),
            Dot3(axisWorld, Vec3(parentWorld.at.x, parentWorld.at.y, parentWorld.at.z))
        ), Vec3(0.0f, 0.0f, 1.0f));
    }

    static bool RotatePedAnimQuatTowardDirection(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        int frameIndex,
        int boneId,
        int parentBoneId,
        int childBoneId,
        const CVector& desiredWorldDir,
        float blend,
        float maxRadians
    ) {
        if (!ped || !hierarchy || blend <= 0.001f)
            return false;
        RawRtQuat* q = GetPedAnimQuatIdb(ped, frameIndex);
        RwMatrix* bone = GetSkinBoneMatrix(hierarchy, boneId);
        RwMatrix* parent = GetSkinBoneMatrix(hierarchy, parentBoneId);
        RwMatrix* child = GetSkinBoneMatrix(hierarchy, childBoneId);
        if (!q || !bone || !parent || !child)
            return false;

