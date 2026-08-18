        const CVector bonePos = Vec3(bone->pos.x, bone->pos.y, bone->pos.z);
        const CVector childPos = Vec3(child->pos.x, child->pos.y, child->pos.z);
        const CVector currentDir = Normalize3(Sub3(childPos, bonePos), desiredWorldDir);
        const CVector desiredDir = Normalize3(desiredWorldDir, currentDir);
        const float dot = ClampFloat(Dot3(currentDir, desiredDir), -1.0f, 1.0f);
        if (dot > 0.99995f)
            return true;

        CVector axisWorld = Cross3(currentDir, desiredDir);
        const float axisLenSq = Dot3(axisWorld, axisWorld);
        if (!IsFiniteFloat(axisLenSq) || axisLenSq < 1.0e-8f)
            return true;
        axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));
        CVector axisLocal = WorldAxisToParentLocal(axisWorld, *parent);

        float radians = std::acos(dot) * ClampFloat(blend, 0.0f, 1.0f);
        if (maxRadians > 0.0f)
            radians = ClampFloat(radians, -maxRadians, maxRadians);
        if (std::fabs(radians) < 1.0e-5f)
            return true;

        // Use VC/RenderWare's exact quaternion combiner. rwCOMBINEPOSTCONCAT == 2,
        // matching the local-orientation update used by the game's gun IK and SA's
        // IK chain after the correction axis is transformed into parent-local space.
        typedef RawRtQuat* (__cdecl* RtQuatRotateRawFn)(RawRtQuat*, const CVector*, float, int);
        RtQuatRotateRawFn rotateQuat = reinterpret_cast<RtQuatRotateRawFn>(0x65ABD0);
        if (!rotateQuat)
            return false;
        const float degrees = radians * 57.29577951308232f;
        rotateQuat(q, &axisLocal, degrees, 2);
        // VC's native CPedIK sets bit 0x20 at CPed+0x154 after editing arm quats.
        // Mirror that bookkeeping before rebaking the hierarchy.
        if (IsReadableAddress(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ped) + 0x154), 1))
            *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(ped) + 0x154) |= 0x20;
        return IsFiniteFloat(q->x) && IsFiniteFloat(q->y) && IsFiniteFloat(q->z) && IsFiniteFloat(q->w);
    }

    static bool MirrorNativeGunArmPoseIntoSecondArm(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        EntityUpdateRpHAnimFn updateFn,
        CEntity* entity,
        float blend
    ) {
        if (!ped || !hierarchy || !updateFn || !entity || blend <= 0.001f)
            return false;

        // Snapshot the native VC weapon arm AFTER the game's own UpdateRpHAnim. This
        // already includes VC's actual aim, locomotion and reload animation state.
        // We mirror its segment directions, rather than inventing a target elbow/pole.
        RwMatrix* nativeUpper = GetSkinBoneMatrix(hierarchy, NativeUpperBoneId());
        RwMatrix* nativeLower = GetSkinBoneMatrix(hierarchy, NativeLowerBoneId());
        RwMatrix* nativeHand  = GetSkinBoneMatrix(hierarchy, NativeHandBoneId());
        if (!nativeUpper || !nativeLower || !nativeHand)
            return false;

        const CVector nShoulder = Vec3(nativeUpper->pos.x, nativeUpper->pos.y, nativeUpper->pos.z);
        const CVector nElbow    = Vec3(nativeLower->pos.x, nativeLower->pos.y, nativeLower->pos.z);
        const CVector nHand     = Vec3(nativeHand->pos.x, nativeHand->pos.y, nativeHand->pos.z);
        const CVector mirrorNormal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));
        const CVector desiredUpperDir = ReflectDirection(Sub3(nElbow, nShoulder), mirrorNormal);
        const CVector desiredLowerDir = ReflectDirection(Sub3(nHand, nElbow), mirrorNormal);

        // Upper arm: left upper (slot 3 / HAnim 32), parented by left clavicle 31.
        bool changed = RotatePedAnimQuatTowardDirection(
            ped, hierarchy,
            SecondUpperFrameIndex(), SecondUpperBoneId(), SecondClavicleBoneId(), SecondLowerBoneId(),
            desiredUpperDir, blend, 0.95f
        );
        if (!changed)
            return false;
        updateFn(entity);

        // Lower arm: slot 13 / HAnim 33, parented by left upper 32.
        hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        changed = RotatePedAnimQuatTowardDirection(
            ped, hierarchy,
            SecondLowerFrameIndex(), SecondLowerBoneId(), SecondUpperBoneId(), SecondHandBoneId(),
            desiredLowerDir, blend, 1.10f
        );
        if (!changed)
            return false;
        updateFn(entity);

        // One small hand-direction correction keeps the cloned weapon aligned with the
        // mirrored native hand without reflecting an entire matrix (which would have a
        // negative determinant). We only rotate the local hand quaternion.
        hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, SecondHandBoneId());
        RwMatrix* secondLower = GetSkinBoneMatrix(hierarchy, SecondLowerBoneId());
        nativeHand = GetSkinBoneMatrix(hierarchy, NativeHandBoneId());
        if (secondHand && secondLower && nativeHand) {
            const CVector currentAt = Normalize3(Vec3(secondHand->at.x, secondHand->at.y, secondHand->at.z), desiredLowerDir);
            const CVector desiredAt = Normalize3(ReflectDirection(Vec3(nativeHand->at.x, nativeHand->at.y, nativeHand->at.z), mirrorNormal), currentAt);
            const float dot = ClampFloat(Dot3(currentAt, desiredAt), -1.0f, 1.0f);
            if (dot < 0.99995f) {
                CVector axisWorld = Cross3(currentAt, desiredAt);
                const float axisLenSq = Dot3(axisWorld, axisWorld);
                RawRtQuat* handQ = GetPedAnimQuatIdb(ped, SecondHandFrameIndex());
                if (handQ && axisLenSq > 1.0e-8f) {
                    axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));
                    CVector axisLocal = WorldAxisToParentLocal(axisWorld, *secondLower);
                    float radians = std::acos(dot) * ClampFloat(blend * 0.65f, 0.0f, 1.0f);
                    radians = ClampFloat(radians, -0.70f, 0.70f);
                    typedef RawRtQuat* (__cdecl* RtQuatRotateRawFn)(RawRtQuat*, const CVector*, float, int);
                    reinterpret_cast<RtQuatRotateRawFn>(0x65ABD0)(handQ, &axisLocal, radians * 57.29577951308232f, 2);
                    if (IsReadableAddress(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ped) + 0x154), 1))
                        *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(ped) + 0x154) |= 0x20;
                    updateFn(entity);
                }
            }
        }

