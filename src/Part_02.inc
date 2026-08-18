    // Offsets below are from the supplied Vice City IDB, not inferred from GTA III.
    // CPed::Render uses [ped+0x4C] as the clump, [ped+0x1C0] as the native weapon hand
    // frame-data slot, [ped+0x1F0] as m_pCurWeaponAtomic and [ped+0x530] as m_nWepModelID.
    static const uintptr_t OFF_ENTITY_RW_CLUMP = 0x4C;
    static const uintptr_t OFF_PED_FRAME_ARRAY = 0x1A8;
    static const uintptr_t OFF_PED_CUR_WEAPON_ATOMIC = 0x1F0;
    static const uintptr_t OFF_PED_WEAPON_MODEL_ID = 0x530;
    static const uintptr_t OFF_PED_IK = 0x210;
    static const uintptr_t OFF_PED_AIM_ANGLE = 0x52C;
    static const uintptr_t OFF_PED_AIM_SLOPE = 0x6C0;
    // CPhysical starts after CEntity (0x64); m_vecMoveSpeed follows audio id and two floats.
    static const uintptr_t OFF_PHYSICAL_MOVE_SPEED = 0x70;

    typedef bool(__thiscall* WeaponFireFn)(CWeapon*, CEntity*, CVector*);
    typedef void(__thiscall* EntityRenderFn)(CEntity*);
    typedef void(__thiscall* EntityUpdateRpHAnimFn)(CEntity*);
    typedef bool(__thiscall* PedIKPointGunInDirectionFn)(CPedIK*, float, float);
    typedef bool(__thiscall* PedIKPointGunInDirectionUsingArmFn)(CPedIK*, float, float);
    typedef RpHAnimHierarchy*(__cdecl* GetAnimHierarchyFromSkinClumpFn)(RpClump*);
    typedef RwMatrix*(__cdecl* RpHAnimHierarchyGetMatrixArrayFn)(RpHAnimHierarchy*);

    enum SkinBoneTag {
        BONE_RCLAVICLE = 21,
        BONE_SUPPERARMR = 22,
        BONE_SLOWERARMR = 23,
        BONE_SRHAND = 24,
        BONE_LCLAVICLE = 31,
        BONE_SUPPERARML = 32,
        BONE_SLOWERARML = 33,
        BONE_SLHAND = 34
    };

    struct Config {
        bool enabled;
        bool pistol;
        bool uzi;
        bool tec9;
        bool doubleShot;
        bool leftArmIK;
        bool mirrorWeaponOnly;
        bool attachCloneToActualHand;
        bool secondHandRight;
        bool armIKRequiresFire;
        bool armIKWhileMoving;
        float armIKMoveSpeedLimit;
        float armAimBlend;
        float armHoldBlend;
        float armReloadBlend;
        float armReachScale;
        float armSideBias;
        float armUpBias;
        float aimPitchScale;
        float aimPitchOffset;
        bool chainExistingCallHooks;
        int aimGraceFrames;
        int aimBlendFrames;
        float offsetX;
        float offsetY;
        float offsetZ;
        float rotX;
        float rotY;
        float rotZ;

        Config()
            : enabled(true), pistol(true), uzi(true), tec9(true), doubleShot(true), leftArmIK(true), mirrorWeaponOnly(false),
            attachCloneToActualHand(true), secondHandRight(false), armIKRequiresFire(false), armIKWhileMoving(true), armIKMoveSpeedLimit(0.20f),
            armAimBlend(1.00f), armHoldBlend(0.90f), armReloadBlend(0.90f), armReachScale(0.92f),
            armSideBias(0.02f), armUpBias(0.02f), aimPitchScale(1.0f), aimPitchOffset(0.0f),
            chainExistingCallHooks(true), aimGraceFrames(3), aimBlendFrames(2),
            offsetX(0.04f), offsetY(-0.05f), offsetZ(0.0f),
            rotX(180.0f), rotY(0.0f), rotZ(0.0f) {
        }
    };

    struct LeftWeaponRuntime {
        CPed* owner;
        RpClump* ownerClump;
        RpAtomic* atomic;
        RwFrame* helperFrame;
        RwFrame* sourceHandFrame;
        bool skinnedSource;
        int modelId;
        eWeaponType weaponType;

        LeftWeaponRuntime()
            : owner(0), ownerClump(0), atomic(0), helperFrame(0), sourceHandFrame(0), skinnedSource(false), modelId(-1), weaponType(WEAPONTYPE_UNARMED) {
        }
    };

    struct CallPatch {
        uintptr_t address;
        uintptr_t previousTarget;
        uintptr_t hookTarget;
        int32_t originalRel;
        bool installed;
        bool chained;

        CallPatch()
            : address(0), previousTarget(0), hookTarget(0), originalRel(0),
            installed(false), chained(false) {
        }
    };

    static Config gConfig;
    static LeftWeaponRuntime gLeft;
    static CallPatch gFirePatch1;
    static CallPatch gFirePatch2;
    static CallPatch gRenderPatch;
    static CallPatch gPreRenderAnimPatch;
    static char gIniPath[MAX_PATH] = {};
    static char gLogPath[MAX_PATH] = {};
    static bool gLoggedFirstRenderMirror = false;
    static bool gLoggedFirstNativeIkReplay = false;
    static unsigned int gRenderMirrorHits = 0;
    static unsigned int gLastArmPoseFrame = 0xFFFFFFFFu;
    static unsigned int gSecondShotAttempts = 0;
    static unsigned int gSecondShotSuccess = 0;
    static bool gLoggedFirstSecondShot = false;
    static bool gLoggedClumpReplacement = false;
    static bool gLoggedRuntimeSummary = false;
    static int gAimGraceFramesRemaining = 0;
    static bool gLoggedSkinnedPedMode = false;
    static bool gLoggedRenderRechain = false;
    static bool gLoggedCreateFailure = false;
    static unsigned int gCreateFailureFrame = 0;
    static float gAimBlend = 0.0f;
    static unsigned int gAimBlendFrame = 0xFFFFFFFFu;

