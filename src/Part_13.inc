    static bool NativeGunArmIsRaised(const MirroredArmChain& chain) {
        if (!chain.valid)
            return false;
        CVector shoulder, hand;
        if (!GetFrameWorldPosition(chain.nativeUpper, shoulder) ||
            !GetFrameWorldPosition(chain.nativeHand, hand))
            return false;
        return hand.z > shoulder.z - 0.25f;
    }

    struct AxisRotation {
        CVector axis;
        float c;
        float s;
        bool identity;
        AxisRotation() : axis(Vec3(0.0f, 0.0f, 1.0f)), c(1.0f), s(0.0f), identity(true) {}
    };

    static AxisRotation MakeAxisRotation(const CVector& fromVector, const CVector& toVector, const Basis3& hint) {
        AxisRotation r;
        const CVector from = Normalize3(fromVector, Vec3(0.0f, 1.0f, 0.0f));
        const CVector to = Normalize3(toVector, from);
        const float d = ClampFloat(Dot3(from, to), -1.0f, 1.0f);
        if (d > 0.99999f)
            return r;

        CVector axis = Cross3(from, to);
        float axisLenSq = Dot3(axis, axis);
        if (axisLenSq <= 0.000001f) {
            axis = Cross3(from, hint.up);
            axisLenSq = Dot3(axis, axis);
            if (axisLenSq <= 0.000001f)
                axis = Cross3(from, hint.right);
            r.axis = Normalize3(axis, Vec3(0.0f, 0.0f, 1.0f));
            r.c = -1.0f;
            r.s = 0.0f;
        }
        else {
            const float axisLen = std::sqrt(axisLenSq);
            r.axis = Scale3(axis, 1.0f / axisLen);
            r.c = d;
            r.s = axisLen;
        }
        r.identity = false;
        return r;
    }

    static CVector ApplyAxisRotation(const CVector& v, const AxisRotation& r) {
        if (r.identity)
            return v;
        return RotateAroundAxis(v, r.axis, r.c, r.s);
    }

    static void ApplyAxisRotationToMatrix(RwMatrix& matrix, const CVector& pivot, const AxisRotation& r) {
        if (r.identity)
            return;

        Basis3 basis = BasisFromRwMatrix(&matrix);
        basis.right = ApplyAxisRotation(basis.right, r);
        basis.up = ApplyAxisRotation(basis.up, r);
        basis.at = ApplyAxisRotation(basis.at, r);
        basis = OrthonormalizeBasis(basis);

        const CVector oldPos = Vec3(matrix.pos.x, matrix.pos.y, matrix.pos.z);
        const CVector newPos = Add3(pivot, ApplyAxisRotation(Sub3(oldPos, pivot), r));

        matrix.right.x = basis.right.x; matrix.right.y = basis.right.y; matrix.right.z = basis.right.z;
        matrix.up.x = basis.up.x; matrix.up.y = basis.up.y; matrix.up.z = basis.up.z;
        matrix.at.x = basis.at.x; matrix.at.y = basis.at.y; matrix.at.z = basis.at.z;
        matrix.pos.x = newPos.x; matrix.pos.y = newPos.y; matrix.pos.z = newPos.z;
    }

    static bool NativeGunArmIsRaisedSkinned(RpHAnimHierarchy* hierarchy) {
        RwMatrix* upper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* hand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        return upper && hand && hand->pos.z > upper->pos.z - 0.25f;
    }

    static bool MirrorSkinnedGunArmPose(CPed* ped, RpHAnimHierarchy* hierarchy, float blend) {
        if (!ped || !hierarchy || blend <= 0.0f)
            return false;

        RwMatrix* nativeUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARMR);
        RwMatrix* nativeLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARMR);
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, BONE_SRHAND);
        RwMatrix* mirrorUpper = GetSkinBoneMatrix(hierarchy, BONE_SUPPERARML);
        RwMatrix* mirrorLower = GetSkinBoneMatrix(hierarchy, BONE_SLOWERARML);
        RwMatrix* mirrorHand = GetSkinBoneMatrix(hierarchy, BONE_SLHAND);
        if (!nativeUpper || !nativeLower || !nativeHand || !mirrorUpper || !mirrorLower || !mirrorHand)
            return false;

        const CVector nativeShoulder = Vec3(nativeUpper->pos.x, nativeUpper->pos.y, nativeUpper->pos.z);
        const CVector nativeElbow = Vec3(nativeLower->pos.x, nativeLower->pos.y, nativeLower->pos.z);
        const CVector nativeHandPos = Vec3(nativeHand->pos.x, nativeHand->pos.y, nativeHand->pos.z);
        const CVector mirrorShoulder = Vec3(mirrorUpper->pos.x, mirrorUpper->pos.y, mirrorUpper->pos.z);
        CVector mirrorElbow = Vec3(mirrorLower->pos.x, mirrorLower->pos.y, mirrorLower->pos.z);
        CVector mirrorHandPos = Vec3(mirrorHand->pos.x, mirrorHand->pos.y, mirrorHand->pos.z);

        const CVector mirrorNormal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));

        const CVector currentUpperDir = Sub3(mirrorElbow, mirrorShoulder);
        const CVector reflectedUpperDir = ReflectDirection(Sub3(nativeElbow, nativeShoulder), mirrorNormal);
        const CVector targetUpperDir = BlendDirection(currentUpperDir, reflectedUpperDir, blend);
        const AxisRotation upperRot = MakeAxisRotation(currentUpperDir, targetUpperDir, BasisFromRwMatrix(mirrorUpper));

