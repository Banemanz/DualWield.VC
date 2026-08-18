        if (d > 0.99999f)
            return basis;

        CVector axis = Cross3(from, to);
        float axisLenSq = Dot3(axis, axis);
        float s = 0.0f;
        float c = d;

        if (axisLenSq <= 0.000001f) {
            // 180-degree case. Pick a stable axis perpendicular to the bone and
            // derived from the bone's own orientation so it cannot randomly flip.
            axis = Cross3(from, basis.up);
            axisLenSq = Dot3(axis, axis);
            if (axisLenSq <= 0.000001f) {
                axis = Cross3(from, basis.right);
                axisLenSq = Dot3(axis, axis);
            }
            axis = Normalize3(axis, Vec3(0.0f, 0.0f, 1.0f));
            c = -1.0f;
            s = 0.0f;
        }
        else {
            const float axisLen = std::sqrt(axisLenSq);
            axis = Scale3(axis, 1.0f / axisLen);
            s = axisLen; // |cross(from,to)| == sin(angle) for normalized vectors.
        }

        Basis3 out;
        out.right = RotateAroundAxis(basis.right, axis, c, s);
        out.up = RotateAroundAxis(basis.up, axis, c, s);
        out.at = RotateAroundAxis(basis.at, axis, c, s);
        return OrthonormalizeBasis(out);
    }

    static void SetFrameWorldBasis(RwFrame* frame, const Basis3& desiredWorld) {
        if (!frame || !IsFiniteVector(desiredWorld.right) ||
            !IsFiniteVector(desiredWorld.up) || !IsFiniteVector(desiredWorld.at))
            return;

        if (!IsFrameReadable(frame))
            return;
        RwFrame* parentFrame = RwFrameGetParent(frame);
        if (parentFrame && !IsFrameReadable(parentFrame))
            return;
        const Basis3 parentWorld = parentFrame
            ? GetFrameWorldBasis(parentFrame)
            : BasisFromRwMatrix(0);

        Basis3 local;
        local.right = BasisInverseTransformDirection(parentWorld, desiredWorld.right);
        local.up = BasisInverseTransformDirection(parentWorld, desiredWorld.up);
        local.at = BasisInverseTransformDirection(parentWorld, desiredWorld.at);
        local = OrthonormalizeBasis(local);

        RwMatrix* current = RwFrameGetMatrix(frame);
        if (!current)
            return;

        // IMPORTANT: do not merely write RwFrame::modelling and later dirty only the
        // upper arm. RenderWare tracks dirty state per frame. The v2 code could leave
        // the lower arm/hand using cached LTMs, which makes the visual result look like
        // the untouched fight/locomotion pose. RwFrameTransform marks THIS frame and
        // the hierarchy root dirty exactly like RenderWare's own frame mutators.
        RwMatrix replacement = *current;
        replacement.right.x = local.right.x;
        replacement.right.y = local.right.y;
        replacement.right.z = local.right.z;
        replacement.up.x = local.up.x;
        replacement.up.y = local.up.y;
        replacement.up.z = local.up.z;
        replacement.at.x = local.at.x;
        replacement.at.y = local.at.y;
        replacement.at.z = local.at.z;
        // replacement.pos intentionally remains the model's original local translation.
        RwFrameTransform(frame, &replacement, rwCOMBINEREPLACE);
    }

    static bool CaptureMirroredArmChain(CPed* ped, MirroredArmChain& chain) {
        if (!ped || !GetPedClumpIdb(ped))
            return false;

        // Vice City 1.0 EN IDB/frame table:
        //   m_apFrames[4] = Supperarmr   (native weapon upper arm)
        //   m_apFrames[6] = SRhand       (native weapon hand)
        //   m_apFrames[3] = Supperarml   (opposite upper arm)
        //   m_apFrames[5] = SLhand       (opposite hand)
        chain.nativeUpper = GetPedFrameSafe(ped, 4);
        chain.nativeHand = GetPedFrameSafe(ped, 6);
        chain.mirrorUpper = GetPedFrameSafe(ped, 3);
        chain.mirrorHand = GetPedFrameSafe(ped, 5);
        if (!chain.nativeUpper || !chain.nativeHand || !chain.mirrorUpper || !chain.mirrorHand)
            return false;

        chain.nativeLower = RwFrameGetParent(chain.nativeHand);
        chain.mirrorLower = RwFrameGetParent(chain.mirrorHand);
        if (!IsFrameReadable(chain.nativeLower) || !IsFrameReadable(chain.mirrorLower))
            return false;
        if (chain.nativeLower == chain.nativeUpper || chain.mirrorLower == chain.mirrorUpper)
            return false;

        // A ragdoll or skin replacement can temporarily detach/rehome a limb. Only touch
        // the arm when all six frames still resolve to the same skeleton root.
        RwFrame* root = GetFrameRootSafe(chain.nativeUpper);
        if (!root || GetFrameRootSafe(chain.nativeLower) != root ||
            GetFrameRootSafe(chain.nativeHand) != root ||
            GetFrameRootSafe(chain.mirrorUpper) != root ||
            GetFrameRootSafe(chain.mirrorLower) != root ||
            GetFrameRootSafe(chain.mirrorHand) != root)
            return false;

