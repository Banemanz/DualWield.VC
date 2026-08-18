    static void RenderStandaloneLeftWeapon(CPed* ped) {
        if (!ped || !IsEligiblePlayer(ped) || gLeft.owner != ped || !IsAtomicAliveForOwner(ped))
            return;

        if (gLeft.skinnedSource) {
            RpHAnimHierarchy* hierarchy = 0;
            const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
            if (!GetSkinnedPedHierarchy(ped, hierarchy) || !GetSkinBoneMatrix(hierarchy, handNode)) {
                DestroyLeftWeapon();
                return;
            }
        }
        else {
            RwFrame* currentHand = GetPedFrameSafe(ped, SecondHandFrameIndex());
            if (!currentHand || currentHand != gLeft.sourceHandFrame) {
                DestroyLeftWeapon();
                return;
            }
        }

        if (!UpdateLeftWeaponWorldTransform())
            return;

        // Standalone atomic: no RpClumpAddAtomic, no HAnim ownership ambiguity. Its own
        // normal render callback/pipeline still runs, so shader mods can process it as a
        // regular weapon atomic without ped-clump walkers seeing it as a body skin.
        if (!RenderAtomicViaVcCallback(gLeft.atomic))
            RpAtomicRender(gLeft.atomic);
    }

    static bool GetSecondHandWorldMatrix(CPed* ped, RwMatrix& out) {
        if (!ped)
            return false;
        RpHAnimHierarchy* hierarchy = 0;
        if (GetSkinnedPedHierarchy(ped, hierarchy)) {
            const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
            RwMatrix* hand = GetSkinBoneMatrix(hierarchy, handNode);
            if (!hand)
                return false;
            out = *hand;
            return IsFiniteRwMatrix(out);
        }
        RwFrame* handFrame = GetPedFrameSafe(ped, SecondHandFrameIndex());
        return handFrame && GetFrameWorldMatrixManual(handFrame, out);
    }

    static bool TransformPointByRwMatrix(const CVector& local, const RwMatrix& matrix, CVector& out) {
        if (!IsFiniteRwMatrix(matrix) || !IsFiniteVector(local))
            return false;
        out.x = matrix.right.x * local.x + matrix.up.x * local.y + matrix.at.x * local.z + matrix.pos.x;
        out.y = matrix.right.y * local.x + matrix.up.y * local.y + matrix.at.y * local.z + matrix.pos.y;
        out.z = matrix.right.z * local.x + matrix.up.z * local.y + matrix.at.z * local.z + matrix.pos.z;
        return IsFiniteVector(out);
    }

    static bool ComputeLeftFireSource(CPed* ped, const CVector* rightSource, CVector& out) {
        if (!ped)
            return false;

        // SA's FireGun(ped, isLeftHand) transforms CWeaponInfo::m_vecFireOffset through
        // the chosen hand bone. Do the same for VC instead of mirroring the native muzzle.
        RwMatrix handWorld;
        if (!GetSecondHandWorldMatrix(ped, handWorld)) {
            if (!rightSource || !IsFiniteVector(*rightSource))
                return false;
            out = *rightSource;
            return true;
        }

        CVector offset = Vec3(0.0f, 0.0f, 0.0f);
        CWeapon* weapon = ped->GetWeapon();
        CWeaponInfo* info = weapon ? CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType) : 0;
        if (info)
            offset = info->m_vecFireOffset;

        if (!TransformPointByRwMatrix(offset, handWorld, out))
            return false;

        const CVector& pedPos = ped->GetPosition();
        const float dx = out.x - pedPos.x, dy = out.y - pedPos.y, dz = out.z - pedPos.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        return IsFiniteFloat(distSq) && distSq <= 16.0f;
    }

    // Vice City CPed::Attack has two direct CWeapon::Fire calls. Each site gets its
    // own wrapper so a pre-existing hook at either call can be chained independently.
    static bool AttackFireHookImpl(CallPatch& patch, CWeapon* weapon,
        CEntity* shooter, CVector* rightSource) {
        WeaponFireFn fireFn = reinterpret_cast<WeaponFireFn>(
            patch.previousTarget);
        if (!fireFn)
            return false;

        const bool firedRight = fireFn(weapon, shooter, rightSource);
        if (!firedRight || !gConfig.doubleShot || !shooter || !weapon)
            return firedRight;

        CPed* ped = reinterpret_cast<CPed*>(shooter);
        if (!IsEligiblePlayer(ped, weapon) || gLeft.owner != ped || !IsAtomicAliveForOwner(ped))
            return firedRight;

        if (weapon->m_eWeaponState != WEAPONSTATE_READY &&
            weapon->m_eWeaponState != WEAPONSTATE_FIRING)
            return firedRight;
        if (weapon->m_nAmmoInClip <= 0)
            return firedRight;

        // Materialize the current HAnim matrices when needed, mirror the final native
        // firing-arm pose, and refresh the private weapon transform before its muzzle is used.
        PrepareOppositePoseForFire(ped);

