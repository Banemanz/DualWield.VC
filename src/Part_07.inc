    static float LimitRadianAngleLocal(float angle) {
        const float pi = 3.14159265358979323846f;
        const float twoPi = 6.28318530717958647692f;
        if (!IsFiniteFloat(angle))
            return 0.0f;
        while (angle <= -pi) angle += twoPi;
        while (angle > pi) angle -= twoPi;
        return angle;
    }

    static bool ComputePointGunUsingArmLocalAngle(CPed* ped, float worldAimAngle, float& outLocalAngle) {
        if (!ped || !IsFiniteFloat(worldAimAngle))
            return false;

        float m10 = 0.0f;
        float m14 = 0.0f;
        // Matches CPedIK::PointGunInDirection prologue at 0x52EC33..0x52EC61:
        // heading = atan2(-*(ped+0x14), *(ped+0x18)); local = Limit(world-heading).
        // The matrix lives inline in CPlaceable; using raw IDB offsets avoids SDK layout drift.
        if (!ReadPedFloatIdb(ped, 0x14, m10) || !ReadPedFloatIdb(ped, 0x18, m14))
            return false;
        const float heading = std::atan2(-m10, m14);
        outLocalAngle = LimitRadianAngleLocal(worldAimAngle - heading);
        return IsFiniteFloat(outLocalAngle);
    }

    static RwFrame* GetFrameRootSafe(RwFrame* frame) {
        if (!IsFrameReadable(frame))
            return 0;
        RwFrame* current = frame;
        for (int depth = 0; depth < 32; ++depth) {
            RwFrame* parent = RwFrameGetParent(current);
            if (!parent)
                return current;
            if (parent == current || !IsFrameReadable(parent))
                return 0;
            current = parent;
        }
        return 0; // corrupt/cyclic hierarchy
    }

    static void BuildGripCorrection(RwMatrix& correction) {
        static RwV3d axisX = { 1.0f, 0.0f, 0.0f };
        static RwV3d axisY = { 0.0f, 1.0f, 0.0f };
        static RwV3d axisZ = { 0.0f, 0.0f, 1.0f };
        RwV3d offset = { gConfig.offsetX, gConfig.offsetY, gConfig.offsetZ };

        RwMatrixSetIdentity(&correction);
        if (gConfig.rotX != 0.0f)
            RwMatrixRotate(&correction, &axisX, gConfig.rotX, rwCOMBINEPRECONCAT);
        if (gConfig.rotY != 0.0f)
            RwMatrixRotate(&correction, &axisY, gConfig.rotY, rwCOMBINEPRECONCAT);
        if (gConfig.rotZ != 0.0f)
            RwMatrixRotate(&correction, &axisZ, gConfig.rotZ, rwCOMBINEPRECONCAT);
        RwMatrixTranslate(&correction, &offset, rwCOMBINEPRECONCAT);
    }

    // Same matrix-walk used by Vice City CPedIK frame-world composition: modelling matrix
    // followed by every parent with POSTCONCAT. This deliberately does not depend
    // on a cached LTM being dirty/clean after the mirrored IK has just edited bones.
    static bool GetFrameWorldMatrixManual(RwFrame* frame, RwMatrix& out) {
        if (!IsFrameReadable(frame) || !RwFrameGetMatrix(frame))
            return false;

        out = *RwFrameGetMatrix(frame);
        RwFrame* current = frame;
        for (int depth = 0; depth < 32; ++depth) {
            RwFrame* parent = RwFrameGetParent(current);
            if (!parent)
                return IsFiniteRwMatrix(out);
            if (parent == current || !IsFrameReadable(parent))
                return false;
            RwMatrix* parentMatrix = RwFrameGetMatrix(parent);
            if (!parentMatrix || !IsFiniteRwMatrix(*parentMatrix))
                return false;
            RwMatrixTransform(&out, parentMatrix, rwCOMBINEPOSTCONCAT);
            current = parent;
        }
        return false; // corrupt/cyclic hierarchy
    }

    static bool PutWeaponFrameOnWorldMatrix(RwFrame* helperFrame, const RwMatrix& handWorld) {
        if (!helperFrame || !IsFiniteRwMatrix(handWorld))
            return false;

        // Weapon-local -> grip correction -> hand world. The helper is a private root,
        // so its modelling matrix is already world-space. This is the same composition
        // used by v3; only the source of handWorld differs for stock vs skinned peds.
        RwMatrix weaponWorld;
        BuildGripCorrection(weaponWorld);
        RwMatrixTransform(&weaponWorld, &handWorld, rwCOMBINEPOSTCONCAT);
        if (!IsFiniteRwMatrix(weaponWorld))
            return false;

        RwMatrix* helperMatrix = RwFrameGetMatrix(helperFrame);
        if (!helperMatrix)
            return false;
        *helperMatrix = weaponWorld;
        RwFrameUpdateObjects(helperFrame);
        return true;
    }

    static bool PutWeaponFrameOnHand(RwFrame* helperFrame, RwFrame* handFrame) {
        if (!helperFrame || !handFrame)
            return false;
        RwMatrix handWorld;
        if (!GetFrameWorldMatrixManual(handFrame, handWorld))
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, handWorld);
    }

    static CVector Vec3(float x, float y, float z);
    static float Dot3(const CVector& a, const CVector& b);
    static CVector Scale3(const CVector& v, float s);
    static CVector Sub3(const CVector& a, const CVector& b);
    static CVector Normalize3(const CVector& v, const CVector& fallback);

