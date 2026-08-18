    static CVector Scale3(const CVector& v, float s) {
        return Vec3(v.x * s, v.y * s, v.z * s);
    }

    static CVector Sub3(const CVector& a, const CVector& b) {
        return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    static CVector Cross3(const CVector& a, const CVector& b) {
        return Vec3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x);
    }

    static CVector Normalize3(const CVector& v, const CVector& fallback) {
        const float lenSq = Dot3(v, v);
        if (!IsFiniteFloat(lenSq) || lenSq <= 0.000001f)
            return fallback;
        const float invLen = 1.0f / std::sqrt(lenSq);
        return Scale3(v, invLen);
    }

    static float ClampFloat(float v, float lo, float hi) {
        if (v < lo)
            return lo;
        if (v > hi)
            return hi;
        return v;
    }

    static Basis3 OrthonormalizeBasis(const Basis3& in) {
        Basis3 out;
        out.right = Normalize3(in.right, Vec3(1.0f, 0.0f, 0.0f));
        CVector up = Sub3(in.up, Scale3(out.right, Dot3(in.up, out.right)));
        out.up = Normalize3(up, Vec3(0.0f, 1.0f, 0.0f));
        out.at = Normalize3(Cross3(out.right, out.up), in.at);
        if (Dot3(out.at, in.at) < 0.0f) {
            out.at = Scale3(out.at, -1.0f);
            out.up = Normalize3(Cross3(out.at, out.right), out.up);
        }
        return out;
    }

    static Basis3 BasisFromRwMatrix(const RwMatrix* m) {
        Basis3 out;
        if (!m) {
            out.right = Vec3(1.0f, 0.0f, 0.0f);
            out.up = Vec3(0.0f, 1.0f, 0.0f);
            out.at = Vec3(0.0f, 0.0f, 1.0f);
            return out;
        }
        out.right = Vec3(m->right.x, m->right.y, m->right.z);
        out.up = Vec3(m->up.x, m->up.y, m->up.z);
        out.at = Vec3(m->at.x, m->at.y, m->at.z);
        return OrthonormalizeBasis(out);
    }

    // A RenderWare frame basis maps local directions into its parent's space:
    // world = right*x + up*y + at*z. Compose the modelling matrices manually so
    // this still works immediately after Vice City writes IK rotations directly.
    static CVector BasisTransformDirection(const Basis3& basis, const CVector& local) {
        return Vec3(
            basis.right.x * local.x + basis.up.x * local.y + basis.at.x * local.z,
            basis.right.y * local.x + basis.up.y * local.y + basis.at.y * local.z,
            basis.right.z * local.x + basis.up.z * local.y + basis.at.z * local.z);
    }

    static CVector BasisInverseTransformDirection(const Basis3& basis, const CVector& parent) {
        return Vec3(
            Dot3(parent, basis.right),
            Dot3(parent, basis.up),
            Dot3(parent, basis.at));
    }

    static Basis3 ComposeBasis(const Basis3& parent, const Basis3& child) {
        Basis3 out;
        out.right = BasisTransformDirection(parent, child.right);
        out.up = BasisTransformDirection(parent, child.up);
        out.at = BasisTransformDirection(parent, child.at);
        return OrthonormalizeBasis(out);
    }

    static Basis3 GetFrameWorldBasis(RwFrame* frame) {
        Basis3 identity = BasisFromRwMatrix(0);
        if (!IsFrameReadable(frame) || !RwFrameGetMatrix(frame))
            return identity;

        Basis3 result = BasisFromRwMatrix(RwFrameGetMatrix(frame));
        RwFrame* current = frame;
        for (int depth = 0; depth < 32; ++depth) {
            RwFrame* parent = RwFrameGetParent(current);
            if (!parent)
                return result;
            if (parent == current || !IsFrameReadable(parent) || !RwFrameGetMatrix(parent))
                return identity;
            result = ComposeBasis(BasisFromRwMatrix(RwFrameGetMatrix(parent)), result);
            current = parent;
        }
        return identity;
    }

    static bool GetFrameWorldPosition(RwFrame* frame, CVector& out) {
        RwMatrix world;
        if (!GetFrameWorldMatrixManual(frame, world))
            return false;
        out.x = world.pos.x;
        out.y = world.pos.y;
        out.z = world.pos.z;
        return IsFiniteVector(out);
    }

    static CVector ReflectDirection(const CVector& v, const CVector& unitPlaneNormal) {
        return Sub3(v, Scale3(unitPlaneNormal, 2.0f * Dot3(v, unitPlaneNormal)));
    }

    static CVector RotateAroundAxis(const CVector& v, const CVector& unitAxis, float c, float s) {
        // Rodrigues' rotation formula, operating entirely in world space.
        return Add3(
            Add3(Scale3(v, c), Scale3(Cross3(unitAxis, v), s)),
            Scale3(unitAxis, Dot3(unitAxis, v) * (1.0f - c)));
    }

    static Basis3 RotateBasisFromTo(const Basis3& basis, const CVector& fromVector, const CVector& toVector) {
        const CVector from = Normalize3(fromVector, Vec3(0.0f, 1.0f, 0.0f));
        const CVector to = Normalize3(toVector, from);
        const float d = ClampFloat(Dot3(from, to), -1.0f, 1.0f);

