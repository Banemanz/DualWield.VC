        chain.valid = true;
        return true;
    }

    // Vice City's one-handed gun animation is partial: it supplies the large weapon-arm pose only
    // on the stock arm, then its gun IK adds the final correction. v3's successful look
    // came from mirroring the FINAL shoulder->elbow and elbow->hand geometry. This port keeps
    // that exact idea. For skinned peds we perform the same operation on the final HAnim
    // matrix array instead of pretending AnimBlendFrameData::m_pFrame is an RwFrame.

    static CVector BlendDirection(const CVector& from, const CVector& to, float t) {
        const CVector a = Normalize3(from, Vec3(0.0f, 1.0f, 0.0f));
        const CVector b = Normalize3(to, a);
        const float clamped = ClampFloat(t, 0.0f, 1.0f);
        return Normalize3(Add3(Scale3(a, 1.0f - clamped), Scale3(b, clamped)), b);
    }

    static float UpdateAimBlendOncePerFrame(bool active) {
        if (!active) {
            gAimBlend = 0.0f;
            gAimBlendFrame = CTimer::m_FrameCounter;
            return 0.0f;
        }

        const unsigned int frame = CTimer::m_FrameCounter;
        if (frame != gAimBlendFrame) {
            gAimBlendFrame = frame;
            if (gConfig.aimBlendFrames <= 1)
                gAimBlend = 1.0f;
            else
                gAimBlend = ClampFloat(gAimBlend + 1.0f / static_cast<float>(gConfig.aimBlendFrames), 0.0f, 1.0f);
        }
        return gAimBlend;
    }

    static bool ShouldMirrorAimPose(CPed* ped, bool nativeArmRaised, float& blend) {
        blend = 0.0f;
        if (!ped || !nativeArmRaised) {
            gAimGraceFramesRemaining = 0;
            UpdateAimBlendOncePerFrame(false);
            return false;
        }

        CWeapon* activeWeapon = ped->GetWeapon();
        if (!activeWeapon || activeWeapon->m_eWeaponState == WEAPONSTATE_RELOADING ||
            activeWeapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO || IsPedMovingForArmIK(ped)) {
            gAimGraceFramesRemaining = 0;
            UpdateAimBlendOncePerFrame(false);
            return false;
        }

        bool active = false;
        if (gConfig.armIKRequiresFire) {
            active = ped->bFiringWeapon || activeWeapon->m_eWeaponState == WEAPONSTATE_FIRING ||
                ped->m_ePedState == PEDSTATE_ATTACK;
        } else {
            active = ped->bIsAimingGun || ped->bIsPointingGunAt ||
                ped->bFiringWeapon || ped->m_ePedState == PEDSTATE_AIMGUN ||
                ped->m_ePedState == PEDSTATE_ATTACK;
        }

        if (active) {
            gAimGraceFramesRemaining = gConfig.aimGraceFrames;
        }
        else if (gAimGraceFramesRemaining > 0) {
            --gAimGraceFramesRemaining;
            active = true;
        }

        if (!active) {
            UpdateAimBlendOncePerFrame(false);
            return false;
        }

        blend = UpdateAimBlendOncePerFrame(true);
        return blend > 0.0f;
    }

    static void MirrorNativeGunArmPose(CPed* ped, const MirroredArmChain& chain, float blend) {
        if (!ped || !chain.valid || blend <= 0.0f)
            return;

        CVector nativeShoulder, nativeElbow, nativeHand;
        CVector mirrorShoulder, mirrorElbow, mirrorHand;
        if (!GetFrameWorldPosition(chain.nativeUpper, nativeShoulder) ||
            !GetFrameWorldPosition(chain.nativeLower, nativeElbow) ||
            !GetFrameWorldPosition(chain.nativeHand, nativeHand) ||
            !GetFrameWorldPosition(chain.mirrorUpper, mirrorShoulder) ||
            !GetFrameWorldPosition(chain.mirrorLower, mirrorElbow) ||
            !GetFrameWorldPosition(chain.mirrorHand, mirrorHand))
            return;

        const CVector mirrorNormal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));

        const CVector currentUpperDir = Sub3(mirrorElbow, mirrorShoulder);
        const CVector mirroredUpperDir = ReflectDirection(Sub3(nativeElbow, nativeShoulder), mirrorNormal);
        const CVector desiredUpperDir = BlendDirection(currentUpperDir, mirroredUpperDir, blend);

        Basis3 upperWorld = GetFrameWorldBasis(chain.mirrorUpper);
        upperWorld = RotateBasisFromTo(upperWorld, currentUpperDir, desiredUpperDir);
        SetFrameWorldBasis(chain.mirrorUpper, upperWorld);

        if (!GetFrameWorldPosition(chain.mirrorLower, mirrorElbow) ||
            !GetFrameWorldPosition(chain.mirrorHand, mirrorHand))
            return;

        const CVector currentLowerDir = Sub3(mirrorHand, mirrorElbow);
        const CVector mirroredLowerDir = ReflectDirection(Sub3(nativeHand, nativeElbow), mirrorNormal);
        const CVector desiredLowerDir = BlendDirection(currentLowerDir, mirroredLowerDir, blend);

        Basis3 lowerWorld = GetFrameWorldBasis(chain.mirrorLower);
        lowerWorld = RotateBasisFromTo(lowerWorld, currentLowerDir, desiredLowerDir);
        SetFrameWorldBasis(chain.mirrorLower, lowerWorld);
        RwFrameGetLTM(chain.mirrorHand);
    }

