    static RpClump* GetPedClumpIdb(CPed* ped);

    static bool IsDualWeapon(eWeaponType type) {
        switch (type) {
        case WEAPONTYPE_PISTOL:
            return gConfig.pistol;
        case WEAPONTYPE_UZI:
            return gConfig.uzi;
        case WEAPONTYPE_TEC9:
            return gConfig.tec9;
        default:
            return false;
        }
    }

    // Vice City 1.0 CPed::Render at 0x4FE231 reads [ped+0x1C0], which is
    // m_apFrames[6], and uses that node to place m_pCurWeaponAtomic. Therefore the
    // stock weapon hand is not configurable: slot 6 / HAnim 24 (SRhand). The added
    // weapon must use the opposite stock slot 5 / HAnim 34 (SLhand).
    static int SecondHandFrameIndex() { return 5; }
    static int SecondUpperFrameIndex() { return 3; }
    static int SecondLowerFrameIndex() { return 13; }
    static int SecondClavicleFrameIndex() { return 15; }
    static int SecondUpperBoneId() { return BONE_SUPPERARML; }
    static int SecondLowerBoneId() { return BONE_SLOWERARML; }
    static int SecondHandBoneId() { return BONE_SLHAND; }
    static int SecondClavicleBoneId() { return BONE_LCLAVICLE; }
    static int NativeHandFrameIndex() { return 6; }
    static int NativeUpperFrameIndex() { return 4; }
    static int NativeLowerFrameIndex() { return 14; }
    static int NativeClavicleFrameIndex() { return 16; }
    static int NativeHandBoneId() { return BONE_SRHAND; }
    static int NativeUpperBoneId() { return BONE_SUPPERARMR; }
    static int NativeLowerBoneId() { return BONE_SLOWERARMR; }
    static int NativeClavicleBoneId() { return BONE_RCLAVICLE; }


    static bool IsUnsafeDualWieldState(CPed* ped) {
        if (!ped || ped->m_bInVehicle)
            return true;

        // Explicit physical/animation ownership flags. Keep our render-stage arm edit
        // away from falling/get-up/death transitions where a ragdoll system is likely
        // to own the same frames. Do not use bDontAcceptIKLookAts here: that flag is
        // about look-at IK and is not a reliable generic ragdoll signal.
        if (ped->bIsPedDieAnimPlaying || ped->bFallenDown ||
            ped->bGetUpAnimStarted || ped->bIsInTheAir)
            return true;

        // Never impose the mirrored gun arm while a vehicle/death/ragdoll/special-state
        // animation owns the body. This is intentionally conservative: missing one frame
        // of the second gun is preferable to fighting another skeleton controller.
        switch (ped->m_ePedState) {
        case PEDSTATE_DUMMY:
        case PEDSTATE_ON_FIRE:
        case PEDSTATE_JUMP:
        case PEDSTATE_FALL:
        case PEDSTATE_GETUP:
        case PEDSTATE_JUMP_FROM_VEHICLE:
        case PEDSTATE_DRIVING:
        case PEDSTATE_DIE:
        case PEDSTATE_DEAD:
        case PEDSTATE_CAR_JACK:
        case PEDSTATE_ENTER_CAR:
        case PEDSTATE_EXIT_CAR:
            return true;
        default:
            return false;
        }
    }

    static bool IsEligiblePlayer(CPed* ped, CWeapon* weapon = 0) {
        if (!gConfig.enabled || !ped || ped->m_nPedType != PEDTYPE_PLAYER1 || IsUnsafeDualWieldState(ped))
            return false;
        if (!GetPedClumpIdb(ped))
            return false;

        CWeapon* active = ped->GetWeapon();
        if (!active)
            return false;
        if (weapon && active != weapon)
            return false;
        return IsDualWeapon(active->m_eWeaponType);
    }

    static bool IsAtomicAliveForOwner(CPed* ped) {
        // Vice City repurposes AnimBlendFrameData::m_pFrame for skinned clumps: the
        // fill-frame path stores an HAnim interpolation-frame pointer there, not an
        // RwFrame. Only require sourceHandFrame for the stock non-skinned path.
        return ped && gLeft.owner == ped && gLeft.ownerClump && gLeft.atomic &&
            gLeft.helperFrame && (gLeft.skinnedSource || gLeft.sourceHandFrame) &&
            GetPedClumpIdb(ped) == gLeft.ownerClump;
    }

    static void DestroyLeftWeapon() {
        // This port owns both of these objects independently. The atomic is deliberately NOT
        // in the player's clump, so cleanup never calls RpClumpRemoveAtomic and remains
        // safe even if a skin/ragdoll mod replaced the player's clump this frame.
        if (gLeft.atomic) {
            RpAtomicDestroy(gLeft.atomic);
            gLeft.atomic = 0;
        }
        if (gLeft.helperFrame) {
            RwFrameDestroy(gLeft.helperFrame);
            gLeft.helperFrame = 0;
        }
        gLeft = LeftWeaponRuntime();
        gAimGraceFramesRemaining = 0;
        gAimBlend = 0.0f;
        gAimBlendFrame = 0xFFFFFFFFu;
    }

    static bool IsFiniteFloat(float v) {
        return std::isfinite(v) != 0;
    }

