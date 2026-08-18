    static bool PutWeaponFrameOnSkinnedHand(CPed* ped, RwFrame* helperFrame) {
        if (!ped || !helperFrame)
            return false;
        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
        RwMatrix* hand = GetSkinBoneMatrix(hierarchy, handNode);
        if (!hand)
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, *hand);
    }

    static bool GetStockWeaponHandWorldMatrix(CPed* ped, RwMatrix& out) {
        if (!ped)
            return false;

        RpHAnimHierarchy* hierarchy = 0;
        if (GetSkinnedPedHierarchy(ped, hierarchy)) {
            const int nativeHandNode = GetPedFrameNodeIdSafe(ped, NativeHandFrameIndex(), NativeHandBoneId());
            RwMatrix* hand = GetSkinBoneMatrix(hierarchy, nativeHandNode);
            if (!hand)
                return false;
            out = *hand;
            return IsFiniteRwMatrix(out);
        }

        RwFrame* nativeHand = GetPedFrameSafe(ped, NativeHandFrameIndex());
        return nativeHand && GetFrameWorldMatrixManual(nativeHand, out);
    }

    static bool MirrorWeaponOnlyMatrixAcrossPed(CPed* ped, const RwMatrix& nativeHand, RwMatrix& out) {
        if (!ped || !IsFiniteRwMatrix(nativeHand))
            return false;

        out = nativeHand;

        // Reflect only the weapon position. A full matrix reflection has negative
        // determinant and can hand RenderWare an invalid basis. Keeping the native
        // weapon basis preserves muzzle/pipeline behavior while putting the clone on
        // the opposite side of Tommy's body without touching any body bone matrices.
        const CVector normal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));
        const CVector pedPos = ped->GetPosition();
        const CVector src = Vec3(nativeHand.pos.x, nativeHand.pos.y, nativeHand.pos.z);
        const CVector rel = Sub3(src, pedPos);
        const float d = Dot3(rel, normal);
        const CVector mirrored = Sub3(src, Scale3(normal, 2.0f * d));
        out.pos.x = mirrored.x;
        out.pos.y = mirrored.y;
        out.pos.z = mirrored.z;
        return IsFiniteRwMatrix(out);
    }

    static bool PutWeaponFrameOnMirroredNativeHand(CPed* ped, RwFrame* helperFrame) {
        if (!ped || !helperFrame)
            return false;
        RwMatrix nativeHand;
        if (!GetStockWeaponHandWorldMatrix(ped, nativeHand))
            return false;
        RwMatrix mirrored;
        if (!MirrorWeaponOnlyMatrixAcrossPed(ped, nativeHand, mirrored))
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, mirrored);
    }

    static bool UpdateLeftWeaponWorldTransform() {
        if (!gLeft.owner || !gLeft.atomic || !gLeft.helperFrame)
            return false;
        if (gLeft.skinnedSource)
            return PutWeaponFrameOnSkinnedHand(gLeft.owner, gLeft.helperFrame);
        if (!gLeft.sourceHandFrame)
            return false;
        return PutWeaponFrameOnHand(gLeft.helperFrame, gLeft.sourceHandFrame);
    }

    static bool CreateLeftWeapon(CPed* ped) {
        if (!ped || !GetPedClumpIdb(ped) || !NativeWeaponModelReady(ped))
            return false;

        CWeapon* weapon = ped->GetWeapon();
        if (!weapon || !IsDualWeapon(weapon->m_eWeaponType))
            return false;

        RpHAnimHierarchy* skinHierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, skinHierarchy);

        RwFrame* sourceHand = 0;
        if (!skinned) {
            sourceHand = GetPedFrameSafe(ped, SecondHandFrameIndex());
            if (!sourceHand)
                return false;
        }

        // Clone the exact weapon atomic already created by Vice City for the player.
        // This avoids weapon-info/model-info wrappers entirely and automatically follows
        // replacement DFFs, textures, render callbacks and pipelines.
        RpAtomic* nativeAtomic = GetNativeWeaponAtomicIdb(ped);
        RpAtomic* atomic = RpAtomicClone(nativeAtomic);
        if (!atomic)
            return false;

        RwFrame* helper = RwFrameCreate();
        if (!helper) {
            RpAtomicDestroy(atomic);
            return false;
        }

        // RpAtomicClone may inherit a frame association. Re-parent only the clone; never
        // destroy that inherited frame because it can belong to the native weapon.
        RpAtomicSetFrame(atomic, helper);

        bool posed = false;
        if (skinned) {
            const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
            RwMatrix* hand = GetSkinBoneMatrix(skinHierarchy, handNode);
            posed = hand && PutWeaponFrameOnWorldMatrix(helper, *hand);
        } else {
            posed = PutWeaponFrameOnHand(helper, sourceHand);
        }
        if (!posed) {
            RpAtomicSetFrame(atomic, 0);
            RpAtomicDestroy(atomic);
            RwFrameDestroy(helper);
            return false;
        }

