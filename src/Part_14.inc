        ApplyAxisRotationToMatrix(*mirrorUpper, mirrorShoulder, upperRot);
        ApplyAxisRotationToMatrix(*mirrorLower, mirrorShoulder, upperRot);
        ApplyAxisRotationToMatrix(*mirrorHand, mirrorShoulder, upperRot);

        mirrorElbow = Vec3(mirrorLower->pos.x, mirrorLower->pos.y, mirrorLower->pos.z);
        mirrorHandPos = Vec3(mirrorHand->pos.x, mirrorHand->pos.y, mirrorHand->pos.z);
        const CVector currentLowerDir = Sub3(mirrorHandPos, mirrorElbow);
        const CVector reflectedLowerDir = ReflectDirection(Sub3(nativeHandPos, nativeElbow), mirrorNormal);
        const CVector targetLowerDir = BlendDirection(currentLowerDir, reflectedLowerDir, blend);
        const AxisRotation lowerRot = MakeAxisRotation(currentLowerDir, targetLowerDir, BasisFromRwMatrix(mirrorLower));

        ApplyAxisRotationToMatrix(*mirrorLower, mirrorElbow, lowerRot);
        ApplyAxisRotationToMatrix(*mirrorHand, mirrorElbow, lowerRot);
        return IsFiniteRwMatrix(*mirrorUpper) && IsFiniteRwMatrix(*mirrorLower) && IsFiniteRwMatrix(*mirrorHand);
    }

    static bool ReplayNativeGunIKIntoOppositeArm(CPed* ped, RpHAnimHierarchy* hierarchy, float blend) {
        if (!ped || !hierarchy || blend <= 0.0f)
            return false;

        // Do not call CPedIK::PointGunInDirection here. That full routine also runs
        // VC's torso/root gun-aim path after the arm phase, and v8 was therefore
        // bending the body while trying to solve only the opposite hand. Use the
        // exact arm-only subroutine called by the native routine at 0x52EC80, with
        // its hard-coded right-arm frame slots temporarily redirected to the left arm.
        AnimBlendFrameData** slotUpper = GetPedFrameSlotPtrIdb(ped, 4);   // native gun upper arm: ped+0x1B8
        AnimBlendFrameData** slotHand = GetPedFrameSlotPtrIdb(ped, 6);    // native gun hand:      ped+0x1C0
        AnimBlendFrameData** slotLower = GetPedFrameSlotPtrIdb(ped, 14);  // native gun forearm:   ped+0x1E0
        AnimBlendFrameData** slotProbe = GetPedFrameSlotPtrIdb(ped, 16);  // native gun clavicle:  ped+0x1E8
        if (!slotUpper || !slotHand || !slotLower || !slotProbe)
            return false;

        AnimBlendFrameData* savedUpper = *slotUpper;
        AnimBlendFrameData* savedHand = *slotHand;
        AnimBlendFrameData* savedLower = *slotLower;
        AnimBlendFrameData* savedProbe = *slotProbe;

        AnimBlendFrameData* leftUpper = FindPedFrameDataByNodeId(ped, BONE_SUPPERARML);
        AnimBlendFrameData* leftLower = FindPedFrameDataByNodeId(ped, BONE_SLOWERARML);
        AnimBlendFrameData* leftHand = FindPedFrameDataByNodeId(ped, BONE_SLHAND);
        AnimBlendFrameData* leftProbe = FindOppositeFrameForNativeSlot(ped, 16, BONE_LCLAVICLE);
        if (!leftUpper || !leftLower || !leftHand || !leftProbe)
            return false;

        float aimAngle = 0.0f;
        float aimSlope = 0.0f;
        float localArmAngle = 0.0f;
        if (!ReadPedFloatIdb(ped, OFF_PED_AIM_ANGLE, aimAngle))
            return false;
        if (!ReadPedFloatIdb(ped, OFF_PED_AIM_SLOPE, aimSlope))
            aimSlope = 0.0f;
        if (!ComputePointGunUsingArmLocalAngle(ped, aimAngle, localArmAngle))
            return false;

        CPedIK* ik = reinterpret_cast<CPedIK*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_IK);
        PedIKPointGunInDirectionUsingArmFn pointGunArm =
            reinterpret_cast<PedIKPointGunInDirectionUsingArmFn>(ADDR_PEDIK_POINT_GUN_IN_DIRECTION_USING_ARM);
        if (!IsReadableAddress(ik, 0x28) || !pointGunArm)
            return false;

        // The arm-only routine still updates CPedIK's cached limb orientations and flags
        // at this+0x14..0x24. Those fields belong to VC's real/native gun arm on the next
        // tick. Preserve them so the replay leaves only the intended left-arm HAnim quat
        // edits and the ped dirty bit, not a poisoned IK controller state.
        uint8_t savedIkState[0x24] = {};
        std::memcpy(savedIkState, reinterpret_cast<uint8_t*>(ik) + 4, sizeof(savedIkState));

        *slotUpper = leftUpper;
        *slotHand = leftHand;
        *slotLower = leftLower;
        *slotProbe = leftProbe;
        const bool moved = pointGunArm(ik, localArmAngle, aimSlope);
        *slotProbe = savedProbe;
        *slotLower = savedLower;
        *slotHand = savedHand;
        *slotUpper = savedUpper;
        std::memcpy(reinterpret_cast<uint8_t*>(ik) + 4, savedIkState, sizeof(savedIkState));

        if (!gLoggedFirstNativeIkReplay) {
            char line[520];
            std::snprintf(line, sizeof(line),
                "DualWieldVC v12: arm-only CPedIK replay used for opposite arm blend=%.2f worldAngle=%.3f localAngle=%.3f slope=%.3f leftUpper=%p leftLower=%p leftHand=%p leftProbe=%p moved=%d.",
                blend, aimAngle, localArmAngle, aimSlope, leftUpper, leftLower, leftHand, leftProbe, moved ? 1 : 0);
            Log(line);
            gLoggedFirstNativeIkReplay = true;
        }
        return moved;
    }

