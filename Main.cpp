/*
    DualWieldVC v17 - Vice City MAC-10 second-gun ownership fix
    GTA Vice City 1.0 EN dual-wield support for the stock Pistol, Tec-9 and MAC-10/Ingram.

    v14 grip fix:
      - The added weapon copies VC's correctly-facing native weapon orientation unchanged.
      - Translation is anchored to the actual opposite hand.
      - No reflected weapon basis and no SA hardcoded 180-degree grip are used.
      - The second muzzle is transformed through the clone's final world matrix so the
        bullet path cannot disagree with the visible barrel.

    Reverse-engineering anchors (GTA Vice City 1.0 EN, IDB MD5 C2E036B50183BBB24DED0387D4B71213):
      CPed::Attack()                          0x52B070
        -> CWeapon::Fire call #1             0x52B749
        -> CWeapon::Fire call #2             0x52B9B4
      CWeapon::Fire(CEntity*, CVector*)      0x5D45E0
      CWeapon::FireInstantHit(...)           0x5D1140
      CPedIK::PointGunInDirectionUsingArm    0x52E7B0
      CPedIK::PointGunInDirection(...)       0x52EC20
      CPed::AddWeaponModel(int)              0x4FFE40
      CPed::Render()                         0x4FE0F0
        -> CEntity::Render CALL              0x4FE216
      CPed::PreRender()                      0x4FE4C0
        -> CEntity::UpdateRpHAnim CALL       0x4FE511
      CEntity::Render()                      0x4887D0
      CEntity::UpdateRpHAnim()               0x489330

    Vice City ped frame table:
      m_apFrames[3] = Supperarml
      m_apFrames[4] = Supperarmr
      m_apFrames[5] = SLhand
      m_apFrames[6] = SRhand

    Vice City HAnim arm/hand tags:
      right upper/lower/hand = 22/23/24
      left  upper/lower/hand = 32/33/34

    Design:
      - This is an SA-style backport controller, not a render-only hack.
      - The native VC weapon remains owned by CPed/CWeapon. The second weapon is a
        clone of the current native weapon atomic, held outside Tommy's clump on a
        private helper frame.
      - VC 1.0 stock weapon ownership is taken directly from CPed::Render: the native
        weapon is hardwired to m_apFrames[6] / HAnim 24, so the added weapon is always
        attached to m_apFrames[5] / HAnim 34. There is no left/right guess INI.
      - The clone is anchored to the selected hand bone every frame. Its orientation
        copies the native weapon basis so both barrels face the same world direction.
      - The second shot muzzle is computed like SA CTaskSimpleUseGun::FireGun: transform
        CWeaponInfo::m_vecFireOffset through the selected hand bone. It is not a
        reflected copy of the native muzzle.
      - CPed::PreRender's CEntity::UpdateRpHAnim CALL is patched directly. VC first
        bakes the normal body pose, then v14 mirrors the native weapon-arm segment directions into the
        opposite arm by rotating its HAnim interpolation quaternions, rerunning VC's own UpdateRpHAnim to bake a coherent skin pose.
      - No VC CPedIK::PointGunInDirection* calls are replayed for the second arm. The
        v8/v9 slot-remap approach poisoned the native one-arm controller and caused
        cross-body/dab poses.
      - Both native CWeapon::Fire CALL sites inside CPed::Attack are signature-resolved
        at runtime. The native/right shot runs first; after success, the same native
        Fire path runs once more using the opposite-hand muzzle.
      - CALL hooks are opcode/target validated, chain pre-existing hooks when enabled,
        and restore only when this ASI still owns the call site.

    Binary authority/validation target: classic GTA Vice City 1.0 EN matching the supplied IDB.
    The fire call sites are resolved by surrounding machine-code signatures with the verified
    1.0 addresses only as a fallback; render and PreRender integration patch verified VC CALL sites directly.
*/

#ifndef GTAVC
#error DualWieldVC must be built as a GTA Vice City plugin (GTAVC).
#endif
#ifdef GTASA
#error DualWieldVC cannot be built or linked as a GTA San Andreas plugin.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

#include "plugin.h"
#include "RenderWare.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPedIK.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "CTimer.h"

using namespace plugin;

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace DualWieldVC {

    static const uintptr_t ADDR_ATTACK_FIRE_CALL_1 = 0x52B749;
    static const uintptr_t ADDR_ATTACK_FIRE_CALL_2 = 0x52B9B4;
    static const uintptr_t ADDR_PED_RENDER_ENTITY_CALL = 0x4FE216;
    static const uintptr_t ADDR_ENTITY_RENDER = 0x4887D0;
    static const uintptr_t ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL = 0x4FE511;
    static const uintptr_t ADDR_ENTITY_UPDATE_RPHANIM = 0x489330;
    static const uintptr_t ADDR_PEDIK_POINT_GUN_IN_DIRECTION_USING_ARM = 0x52E7B0;
    static const uintptr_t ADDR_PEDIK_POINT_GUN_IN_DIRECTION = 0x52EC20;
    static const uintptr_t ADDR_GET_ANIM_HIERARCHY_FROM_SKIN_CLUMP = 0x57F250;
    static const uintptr_t ADDR_RP_HANIM_HIERARCHY_GET_MATRIX_ARRAY = 0x646370;
    static const uintptr_t ADDR_MODEL_INFO_PTRS = 0x92D4C8; // CModelInfo::ms_modelInfoPtrs (VC 1.0 EN IDB)

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
    typedef RpHAnimHierarchy* (__cdecl* GetAnimHierarchyFromSkinClumpFn)(RpClump*);
    typedef RwMatrix* (__cdecl* RpHAnimHierarchyGetMatrixArrayFn)(RpHAnimHierarchy*);

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
        bool tec9;
        bool mac10;
        bool doubleShot;
        bool leftArmIK;
        bool mirrorWeaponOnly;
        bool attachCloneToActualHand;
        bool copyNativeGripBasis;
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
            : enabled(true), pistol(true), tec9(true), mac10(true), doubleShot(true), leftArmIK(true), mirrorWeaponOnly(false),
            attachCloneToActualHand(true), copyNativeGripBasis(true), secondHandRight(false), armIKRequiresFire(false), armIKWhileMoving(true), armIKMoveSpeedLimit(0.20f),
            armAimBlend(1.00f), armHoldBlend(0.90f), armReloadBlend(0.90f), armReachScale(0.92f),
            armSideBias(0.02f), armUpBias(0.02f), aimPitchScale(1.0f), aimPitchOffset(0.0f),
            chainExistingCallHooks(true), aimGraceFrames(3), aimBlendFrames(2),
            offsetX(0.0f), offsetY(0.0f), offsetZ(0.0f),
            rotX(0.0f), rotY(0.0f), rotZ(0.0f) {
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

    static void BuildSiblingPath(char* out, size_t outSize, const char* filename) {
        if (!out || outSize == 0)
            return;
        out[0] = '\0';

        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, MAX_PATH);
        char* slashA = std::strrchr(modulePath, '\\');
        char* slashB = std::strrchr(modulePath, '/');
        char* slash = slashA;
        if (!slash || (slashB && slashB > slash))
            slash = slashB;
        if (slash)
            *(slash + 1) = '\0';
        else
            modulePath[0] = '\0';

        std::snprintf(out, outSize, "%s%s", modulePath, filename);
        out[outSize - 1] = '\0';
    }

    static void Log(const char* text) {
        if (!text || !gLogPath[0])
            return;
        FILE* f = std::fopen(gLogPath, "a");
        if (!f)
            return;
        std::fprintf(f, "%s\n", text);
        std::fclose(f);
    }

    static bool ReadBool(const char* key, bool fallback) {
        return GetPrivateProfileIntA("DualWield", key, fallback ? 1 : 0, gIniPath) != 0;
    }

    static float ReadFloat(const char* key, float fallback) {
        char fallbackText[64] = {};
        char value[64] = {};
        std::snprintf(fallbackText, sizeof(fallbackText), "%.6f", fallback);
        GetPrivateProfileStringA("DualWield", key, fallbackText, value, sizeof(value), gIniPath);
        float parsed = fallback;
        if (std::sscanf(value, "%f", &parsed) != 1)
            return fallback;
        return parsed;
    }

    static void LoadConfig() {
        gConfig.enabled = ReadBool("Enabled", true);
        gConfig.pistol = ReadBool("Pistol", true);
        gConfig.tec9 = ReadBool("Tec9", true);
        gConfig.mac10 = ReadBool("Mac10", true);
        gConfig.doubleShot = ReadBool("DoubleShot", true);
        // v14: fixed VC ownership split from the IDB, plus an SA-style local-quaternion
        // pose mirror. The second side is not configurable because stock CPed::Render
        // hardwires the native weapon to frame slot 6 / HAnim 24.
        gConfig.leftArmIK = ReadBool("QuaternionPoseMirror", true);
        gConfig.attachCloneToActualHand = ReadBool("AttachCloneToActualHand", true);
        gConfig.copyNativeGripBasis = ReadBool("CopyNativeGripBasis", true);
        gConfig.secondHandRight = false; // IDB: stock VC weapon is hardwired to m_apFrames[6]/HAnim 24; second side is slot 5/HAnim 34.
        gConfig.mirrorWeaponOnly = false;
        gConfig.armIKRequiresFire = ReadBool("ArmIKRequiresFire", false);
        gConfig.armIKWhileMoving = ReadBool("ArmIKWhileMoving", true);
        gConfig.armIKMoveSpeedLimit = ReadFloat("ArmIKMoveSpeedLimit", 0.20f);
        if (gConfig.armIKMoveSpeedLimit < 0.0f) gConfig.armIKMoveSpeedLimit = 0.0f;
        if (gConfig.armIKMoveSpeedLimit > 0.50f) gConfig.armIKMoveSpeedLimit = 0.50f;
        gConfig.armAimBlend = ReadFloat("MirrorAimBlend", 1.00f);
        gConfig.armHoldBlend = ReadFloat("MirrorHoldBlend", 0.90f);
        gConfig.armReloadBlend = ReadFloat("MirrorReloadBlend", 0.90f);
        gConfig.armReachScale = ReadFloat("ArmReachScale", 0.92f);
        gConfig.armSideBias = ReadFloat("ArmSideBias", 0.02f);
        gConfig.armUpBias = ReadFloat("ArmUpBias", 0.02f);
        gConfig.aimPitchScale = ReadFloat("AimPitchScale", 1.0f);
        gConfig.aimPitchOffset = ReadFloat("AimPitchOffset", 0.0f);
        if (gConfig.armAimBlend < 0.0f) gConfig.armAimBlend = 0.0f;
        if (gConfig.armAimBlend > 1.0f) gConfig.armAimBlend = 1.0f;
        if (gConfig.armHoldBlend < 0.0f) gConfig.armHoldBlend = 0.0f;
        if (gConfig.armHoldBlend > 1.0f) gConfig.armHoldBlend = 1.0f;
        if (gConfig.armReloadBlend < 0.0f) gConfig.armReloadBlend = 0.0f;
        if (gConfig.armReloadBlend > 1.0f) gConfig.armReloadBlend = 1.0f;
        if (gConfig.armReachScale < 0.50f) gConfig.armReachScale = 0.50f;
        if (gConfig.armReachScale > 1.15f) gConfig.armReachScale = 1.15f;
        gConfig.chainExistingCallHooks = ReadBool("ChainExistingCallHooks", true);
        gConfig.aimGraceFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimGraceFrames", 3, gIniPath));
        if (gConfig.aimGraceFrames < 0) gConfig.aimGraceFrames = 0;
        if (gConfig.aimGraceFrames > 12) gConfig.aimGraceFrames = 12;
        gConfig.aimBlendFrames = static_cast<int>(GetPrivateProfileIntA("DualWield", "AimBlendFrames", 2, gIniPath));
        if (gConfig.aimBlendFrames < 0) gConfig.aimBlendFrames = 0;
        if (gConfig.aimBlendFrames > 8) gConfig.aimBlendFrames = 8;
        // v14 deliberately uses new key names so a stale v12 INI cannot reapply the
        // SA 180-degree X correction that caused the visibly wrong weapon angle in VC.
        gConfig.offsetX = ReadFloat("GripTrimOffsetX", 0.0f);
        gConfig.offsetY = ReadFloat("GripTrimOffsetY", 0.0f);
        gConfig.offsetZ = ReadFloat("GripTrimOffsetZ", 0.0f);
        gConfig.rotX = ReadFloat("GripTrimRotationX", 0.0f);
        gConfig.rotY = ReadFloat("GripTrimRotationY", 0.0f);
        gConfig.rotZ = ReadFloat("GripTrimRotationZ", 0.0f);
    }

    static RpClump* GetPedClumpIdb(CPed* ped);
    static int GetNativeWeaponModelIdIdb(CPed* ped);

    // VC weapon model IDs from the stock model table. MODEL_UZI (282) is intentionally
    // NOT dual-wielded in v17; MODEL_INGRAMSL (283) is the MAC-10/Ingram visual.
    static const int MODELID_COLT45 = 274;
    static const int MODELID_TEC9 = 281;
    static const int MODELID_INGRAMSL = 283;

    static int ResolveEquippedWeaponModelId(CPed* ped, CWeapon* weapon) {
        const int pedModel = GetNativeWeaponModelIdIdb(ped);
        if (pedModel >= 0)
            return pedModel;
        if (!weapon)
            return -1;
        CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
        return info ? static_cast<int>(info->m_nModelId) : -1;
    }

    static bool IsMac10Weapon(CPed* ped, CWeapon* weapon) {
        if (!weapon)
            return false;
        const int modelId = ResolveEquippedWeaponModelId(ped, weapon);
        // Prefer the actual visual model. Keep the type-24 check as the stock VC fallback.
        return modelId == MODELID_INGRAMSL ||
            weapon->m_eWeaponType == WEAPONTYPE_SILENCED_INGRAM;
    }

    static bool IsDualWeapon(CPed* ped, CWeapon* weapon) {
        if (!weapon)
            return false;

        // Model 283 is the MAC-10/Ingram visual and is accepted even if another mod has
        // altered the weapon enum bookkeeping. Model 282 (Uzi) is deliberately excluded.
        const int modelId = ResolveEquippedWeaponModelId(ped, weapon);
        if (modelId == MODELID_INGRAMSL)
            return gConfig.mac10;

        switch (weapon->m_eWeaponType) {
        case WEAPONTYPE_PISTOL:
            return gConfig.pistol;
        case WEAPONTYPE_TEC9:
            return gConfig.tec9;
        case WEAPONTYPE_SILENCED_INGRAM:
            return gConfig.mac10;
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
        return IsDualWeapon(ped, active);
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

    static bool IsFiniteVector(const CVector& v) {
        return IsFiniteFloat(v.x) && IsFiniteFloat(v.y) && IsFiniteFloat(v.z);
    }

    static bool IsFiniteRwVector(const RwV3d& v) {
        return IsFiniteFloat(v.x) && IsFiniteFloat(v.y) && IsFiniteFloat(v.z);
    }

    static bool IsFiniteRwMatrix(const RwMatrix& m) {
        return IsFiniteRwVector(m.right) && IsFiniteRwVector(m.up) &&
            IsFiniteRwVector(m.at) && IsFiniteRwVector(m.pos);
    }

    static bool IsReadableAddress(const void* ptr, size_t bytes = sizeof(void*)) {
        if (!ptr || bytes == 0)
            return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t finish = begin + bytes;
        const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        return finish >= begin && begin >= regionBegin && finish <= regionEnd;
    }

    static bool IsFrameReadable(RwFrame* frame) {
        // We only need enough of RwFrame to safely use its object/parent/modelling fields.
        return IsReadableAddress(frame, 0x50);
    }

    static RpClump* GetPedClumpIdb(CPed* ped) {
        if (!ped || !IsReadableAddress(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ped) + OFF_ENTITY_RW_CLUMP), sizeof(RpClump*)))
            return 0;
        return *reinterpret_cast<RpClump**>(reinterpret_cast<uintptr_t>(ped) + OFF_ENTITY_RW_CLUMP);
    }

    static AnimBlendFrameData* GetPedFrameDataIdb(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 18)
            return 0;
        const uintptr_t slot = reinterpret_cast<uintptr_t>(ped) + OFF_PED_FRAME_ARRAY + sizeof(void*) * static_cast<uintptr_t>(index);
        if (!IsReadableAddress(reinterpret_cast<const void*>(slot), sizeof(AnimBlendFrameData*)))
            return 0;
        return *reinterpret_cast<AnimBlendFrameData**>(slot);
    }

    static RpAtomic* GetNativeWeaponAtomicIdb(CPed* ped) {
        if (!ped || !IsReadableAddress(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_CUR_WEAPON_ATOMIC), sizeof(RpAtomic*)))
            return 0;
        return *reinterpret_cast<RpAtomic**>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_CUR_WEAPON_ATOMIC);
    }

    static int GetNativeWeaponModelIdIdb(CPed* ped) {
        if (!ped || !IsReadableAddress(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_WEAPON_MODEL_ID), sizeof(int)))
            return -1;
        return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(ped) + OFF_PED_WEAPON_MODEL_ID);
    }

    static RwMatrix* GetHAnimMatrixArrayNative(RpHAnimHierarchy* hierarchy) {
        if (!hierarchy)
            return 0;
        RpHAnimHierarchyGetMatrixArrayFn getMatrixArray =
            reinterpret_cast<RpHAnimHierarchyGetMatrixArrayFn>(ADDR_RP_HANIM_HIERARCHY_GET_MATRIX_ARRAY);
        RwMatrix* matrices = getMatrixArray(hierarchy);
        return IsReadableAddress(matrices, sizeof(RwMatrix)) ? matrices : 0;
    }

    struct SkinProbe {
        RpAtomic* atomic;
        RpHAnimHierarchy* hierarchy;
        SkinProbe() : atomic(0), hierarchy(0) {}
    };

    static RpAtomic* FindSkinnedAtomicCB(RpAtomic* atomic, void* data) {
        SkinProbe* probe = reinterpret_cast<SkinProbe*>(data);
        if (!probe || !atomic)
            return atomic;
        RpGeometry* geometry = RpAtomicGetGeometry(atomic);
        if (!geometry)
            return atomic;
        RpSkin* skin = RpSkinGeometryGetSkin(geometry);
        if (!skin)
            return atomic;

        RpHAnimHierarchy* hierarchy = RpSkinAtomicGetHAnimHierarchy(atomic);
        if (!hierarchy)
            return atomic;

        probe->atomic = atomic;
        probe->hierarchy = hierarchy;
        return 0; // stop once the actual skinned body atomic is found
    }

    static bool GetSkinnedPedHierarchy(CPed* ped, RpHAnimHierarchy*& hierarchy) {
        hierarchy = 0;
        if (!ped || !GetPedClumpIdb(ped))
            return false;

        // This is the same helper CPed::Render calls at 0x4FE22C before rendering
        // m_pCurWeaponAtomic. The v3 port tried to infer the hierarchy from RpSkin
        // plugin data and missed stock Tommy, which forced the code into the RwFrame
        // fallback and made weapon creation defer forever.
        GetAnimHierarchyFromSkinClumpFn getHierarchy =
            reinterpret_cast<GetAnimHierarchyFromSkinClumpFn>(ADDR_GET_ANIM_HIERARCHY_FROM_SKIN_CLUMP);
        RpHAnimHierarchy* h = getHierarchy(GetPedClumpIdb(ped));
        if (!h)
            return false;
        // Do not trust SDK struct-member layout here. VC's own CPed::Render only needs
        // the hierarchy pointer, RpHAnimIDGetIndex and RpHAnimHierarchyGetMatrixArray.
        // v4 rejected valid stock hierarchies because it validated fields through the
        // Plugin-SDK layout before the stock render code had materialized them.
        if (!IsReadableAddress(h, 0x14))
            return false;
        if (!GetHAnimMatrixArrayNative(h))
            return false;

        hierarchy = h;
        return true;
    }

    static bool IsSkinnedPed(CPed* ped) {
        RpHAnimHierarchy* hierarchy = 0;
        return GetSkinnedPedHierarchy(ped, hierarchy);
    }

    static RwMatrix* GetSkinBoneMatrix(RpHAnimHierarchy* hierarchy, int boneId) {
        if (!hierarchy)
            return 0;
        const int index = RpHAnimIDGetIndex(hierarchy, boneId);
        if (index < 0 || index > 128)
            return 0;
        RwMatrix* matrices = GetHAnimMatrixArrayNative(hierarchy);
        if (!matrices)
            return 0;
        RwMatrix* matrix = &matrices[index];
        return IsReadableAddress(matrix, sizeof(RwMatrix)) && IsFiniteRwMatrix(*matrix) ? matrix : 0;
    }

    static bool NativeWeaponModelReady(CPed* ped) {
        return ped && GetNativeWeaponAtomicIdb(ped) != 0 && GetNativeWeaponModelIdIdb(ped) >= 0;
    }

    static bool IsExecutableCodePointer(const void* pointer) {
        if (!pointer)
            return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(pointer, &mbi, sizeof(mbi)))
            return false;
        const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return mbi.State == MEM_COMMIT &&
            (mbi.Protect & executable) != 0 &&
            (mbi.Protect & PAGE_GUARD) == 0;
    }

    static RpAtomic* CreateWeaponAtomicViaVcModelFactory(int modelId) {
        if (modelId < 0 || modelId > 10000)
            return 0;

        void** modelInfoPtrs = reinterpret_cast<void**>(ADDR_MODEL_INFO_PTRS);
        if (!IsReadableAddress(modelInfoPtrs + modelId, sizeof(void*)))
            return 0;

        void* modelInfo = modelInfoPtrs[modelId];
        if (!IsReadableAddress(modelInfo, sizeof(void*)))
            return 0;

        void** vtable = *reinterpret_cast<void***>(modelInfo);
        if (!IsReadableAddress(vtable, 0x10))
            return 0;

        void* createInstance = vtable[3]; // +0x0C, exactly as CPed::AddWeaponModel(0x4FFE40) does.
        if (!IsExecutableCodePointer(createInstance))
            return 0;

        typedef RpAtomic* (__thiscall* CreateInstanceFn)(void*);
        return reinterpret_cast<CreateInstanceFn>(createInstance)(modelInfo);
    }

    static RwFrame* GetAtomicFrameIdb(RpAtomic* atomic) {
        if (!atomic)
            return 0;
        const uintptr_t frameSlot = reinterpret_cast<uintptr_t>(atomic) + 4;
        if (!IsReadableAddress(reinterpret_cast<const void*>(frameSlot), sizeof(RwFrame*)))
            return 0;
        RwFrame* frame = *reinterpret_cast<RwFrame**>(frameSlot);
        return IsFrameReadable(frame) ? frame : 0;
    }

    static RwFrame* GetPedFrameSafe(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 18)
            return 0;
        AnimBlendFrameData* data = GetPedFrameDataIdb(ped, index);
        if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
            return 0;
        RwFrame* frame = data->m_pFrame;
        if (!IsFrameReadable(frame))
            return 0;
        return frame;
    }

    static int GetPedFrameNodeIdSafe(CPed* ped, int index, int fallbackNodeId) {
        if (!ped || index < 0 || index >= 18)
            return fallbackNodeId;
        AnimBlendFrameData* data = GetPedFrameDataIdb(ped, index);
        if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
            return fallbackNodeId;
        const int nodeId = static_cast<int>(data->m_nNodeId);
        return nodeId > 0 ? nodeId : fallbackNodeId;
    }


    static AnimBlendFrameData** GetPedFrameSlotPtrIdb(CPed* ped, int index) {
        if (!ped || index < 0 || index >= 18)
            return 0;
        const uintptr_t slot = reinterpret_cast<uintptr_t>(ped) + OFF_PED_FRAME_ARRAY + sizeof(void*) * static_cast<uintptr_t>(index);
        if (!IsReadableAddress(reinterpret_cast<const void*>(slot), sizeof(AnimBlendFrameData*)))
            return 0;
        return reinterpret_cast<AnimBlendFrameData**>(slot);
    }

    static AnimBlendFrameData* FindPedFrameDataByNodeId(CPed* ped, int nodeId) {
        if (!ped || nodeId <= 0)
            return 0;
        for (int i = 0; i < 18; ++i) {
            AnimBlendFrameData* data = GetPedFrameDataIdb(ped, i);
            if (!IsReadableAddress(data, sizeof(AnimBlendFrameData)))
                continue;
            if (static_cast<int>(data->m_nNodeId) == nodeId)
                return data;
        }
        return 0;
    }

    static int MirrorArmBoneId(int nodeId, int fallback) {
        switch (nodeId) {
        case BONE_RCLAVICLE: return BONE_LCLAVICLE;
        case BONE_SUPPERARMR: return BONE_SUPPERARML;
        case BONE_SLOWERARMR: return BONE_SLOWERARML;
        case BONE_SRHAND: return BONE_SLHAND;
        case BONE_LCLAVICLE: return BONE_RCLAVICLE;
        case BONE_SUPPERARML: return BONE_SUPPERARMR;
        case BONE_SLOWERARML: return BONE_SLOWERARMR;
        case BONE_SLHAND: return BONE_SRHAND;
        default: return fallback;
        }
    }

    static AnimBlendFrameData* FindOppositeFrameForNativeSlot(CPed* ped, int nativeSlotIndex, int fallbackMirrorBone) {
        AnimBlendFrameData* native = GetPedFrameDataIdb(ped, nativeSlotIndex);
        int nodeId = fallbackMirrorBone;
        if (IsReadableAddress(native, sizeof(AnimBlendFrameData)))
            nodeId = MirrorArmBoneId(static_cast<int>(native->m_nNodeId), fallbackMirrorBone);
        AnimBlendFrameData* mirrored = FindPedFrameDataByNodeId(ped, nodeId);
        if (mirrored)
            return mirrored;
        return FindPedFrameDataByNodeId(ped, fallbackMirrorBone);
    }

    static bool ReadPedFloatIdb(CPed* ped, uintptr_t offset, float& out) {
        if (!ped)
            return false;
        const uintptr_t address = reinterpret_cast<uintptr_t>(ped) + offset;
        if (!IsReadableAddress(reinterpret_cast<const void*>(address), sizeof(float)))
            return false;
        out = *reinterpret_cast<float*>(address);
        return IsFiniteFloat(out);
    }

    static bool ReadPedVectorIdb(CPed* ped, uintptr_t offset, CVector& out) {
        if (!ped)
            return false;
        const uintptr_t address = reinterpret_cast<uintptr_t>(ped) + offset;
        if (!IsReadableAddress(reinterpret_cast<const void*>(address), sizeof(CVector)))
            return false;
        out = *reinterpret_cast<CVector*>(address);
        return IsFiniteVector(out);
    }

    static bool IsPedMovingForArmIK(CPed* ped) {
        if (!ped || gConfig.armIKWhileMoving || gConfig.armIKMoveSpeedLimit <= 0.0f)
            return false;
        CVector speed;
        if (!ReadPedVectorIdb(ped, OFF_PHYSICAL_MOVE_SPEED, speed))
            return false;
        const float limitSq = gConfig.armIKMoveSpeedLimit * gConfig.armIKMoveSpeedLimit;
        const float speedSq = speed.x * speed.x + speed.y * speed.y;
        return IsFiniteFloat(speedSq) && speedSq > limitSq;
    }

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
    static CVector Cross3(const CVector& a, const CVector& b);
    static CVector Normalize3(const CVector& v, const CVector& fallback);

    static CVector ReflectDirectionAcrossSagittalPlane(CPed* ped, const CVector& v) {
        const CVector n = Normalize3(ped ? ped->GetRight() : Vec3(1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f));
        return Sub3(v, Scale3(n, 2.0f * Dot3(v, n)));
    }

    // v14 grip rule: the weapon model is NOT mirrored geometry. Both guns must use the
    // same world-space weapon basis so their barrels point the same way. The opposite
    // hand supplies only the attachment position. v13 reflected the native basis, which
    // created a mathematically valid rotation but rolled/pitched the clone through the
    // wrist/forearm because a reflection cannot represent holding the same asymmetric
    // weapon model in the other hand with a proper rotation matrix.
    static bool BuildOppositeGripWorld(CPed* ped, const RwMatrix& nativeHand, const RwMatrix& secondHand, RwMatrix& out) {
        if (!ped || !IsFiniteRwMatrix(nativeHand) || !IsFiniteRwMatrix(secondHand))
            return false;

        // Copy the exact orientation VC already uses for the correctly-facing native gun.
        // Only move the origin to the actual opposite-hand bone. Optional GripTrim* is
        // applied later in PutWeaponFrameOnWorldMatrix, in weapon-local space.
        out = nativeHand;
        out.pos = secondHand.pos;
        return IsFiniteRwMatrix(out);
    }

    static bool PutWeaponFrameOnSkinnedHand(CPed* ped, RwFrame* helperFrame) {
        if (!ped || !helperFrame)
            return false;
        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        const int secondNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, secondNode);
        if (!secondHand)
            return false;

        if (!gConfig.copyNativeGripBasis)
            return PutWeaponFrameOnWorldMatrix(helperFrame, *secondHand);

        const int nativeNode = GetPedFrameNodeIdSafe(ped, NativeHandFrameIndex(), NativeHandBoneId());
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, nativeNode);
        if (!nativeHand)
            return false;

        RwMatrix gripWorld;
        if (!BuildOppositeGripWorld(ped, *nativeHand, *secondHand, gripWorld))
            return false;
        return PutWeaponFrameOnWorldMatrix(helperFrame, gripWorld);
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

        if (!gConfig.copyNativeGripBasis)
            return PutWeaponFrameOnHand(gLeft.helperFrame, gLeft.sourceHandFrame);

        RwFrame* nativeFrame = GetPedFrameSafe(gLeft.owner, NativeHandFrameIndex());
        if (!nativeFrame)
            return false;
        RwMatrix nativeHand, secondHand, gripWorld;
        if (!GetFrameWorldMatrixManual(nativeFrame, nativeHand) ||
            !GetFrameWorldMatrixManual(gLeft.sourceHandFrame, secondHand) ||
            !BuildOppositeGripWorld(gLeft.owner, nativeHand, secondHand, gripWorld))
            return false;
        return PutWeaponFrameOnWorldMatrix(gLeft.helperFrame, gripWorld);
    }

    static bool CreateLeftWeapon(CPed* ped) {
        if (!ped || !GetPedClumpIdb(ped))
            return false;

        CWeapon* weapon = ped->GetWeapon();
        if (!weapon || !IsDualWeapon(ped, weapon))
            return false;

        const int modelId = ResolveEquippedWeaponModelId(ped, weapon);
        if (modelId < 0)
            return false;
        const bool mac10 = IsMac10Weapon(ped, weapon);

        RpHAnimHierarchy* skinHierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, skinHierarchy);

        RwFrame* sourceHand = 0;
        if (!skinned) {
            sourceHand = GetPedFrameSafe(ped, SecondHandFrameIndex());
            if (!sourceHand)
                return false;
        }

        RpAtomic* nativeAtomic = GetNativeWeaponAtomicIdb(ped);
        RpAtomic* atomic = 0;
        bool usedModelFactory = false;

        // v17: MAC-10 owns an explicit second-gun creation path. It is NOT merely an
        // arm-eligible weapon. Instantiate MODEL_INGRAMSL (283) through the same
        // CModelInfo virtual CreateInstance path used by CPed::AddWeaponModel. This does
        // not depend on RpAtomicClone or on m_pCurWeaponAtomic being cloneable.
        if (mac10) {
            const int factoryModelId = (modelId == MODELID_INGRAMSL) ? modelId : MODELID_INGRAMSL;
            atomic = CreateWeaponAtomicViaVcModelFactory(factoryModelId);
            usedModelFactory = atomic != 0;
        }

        // Pistol/Tec-9 retain the proven native-atomic clone path. MAC-10 may fall back
        // to it only when a replacement model prevents stock model instancing.
        if (!atomic && nativeAtomic)
            atomic = RpAtomicClone(nativeAtomic);
        if (!atomic)
            return false;

        RwFrame* helper = 0;
        if (usedModelFactory) {
            // CSimpleModelInfo::CreateInstance (the VC vtable +0x0C path) already
            // clones the model template AND allocates/attaches a private RwFrame.
            // Reuse that frame instead of allocating another one and leaking the
            // factory-created frame.
            helper = GetAtomicFrameIdb(atomic);
            if (!helper) {
                RpAtomicDestroy(atomic);
                return false;
            }
        }
        else {
            helper = RwFrameCreate();
            if (!helper) {
                RpAtomicDestroy(atomic);
                return false;
            }

            // RpAtomicClone(nativeAtomic) may inherit the native weapon's frame
            // association. Re-parent only the clone; never destroy the inherited
            // frame because it belongs to the native weapon.
            RpAtomicSetFrame(atomic, helper);
        }

        bool posed = false;
        if (skinned) {
            const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
            RwMatrix* hand = GetSkinBoneMatrix(skinHierarchy, handNode);
            posed = hand && PutWeaponFrameOnWorldMatrix(helper, *hand);
        }
        else {
            posed = PutWeaponFrameOnHand(helper, sourceHand);
        }
        if (!posed) {
            RpAtomicSetFrame(atomic, 0);
            RpAtomicDestroy(atomic);
            RwFrameDestroy(helper);
            return false;
        }

        gLeft.owner = ped;
        gLeft.ownerClump = GetPedClumpIdb(ped);
        gLeft.atomic = atomic;
        gLeft.helperFrame = helper;
        gLeft.sourceHandFrame = sourceHand;
        gLeft.skinnedSource = skinned;
        gLeft.modelId = modelId;
        gLeft.weaponType = weapon->m_eWeaponType;

        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v17: created opposite weapon type=%d model=%d native=%p second=%p source=%s mode=%s.",
            static_cast<int>(weapon->m_eWeaponType), modelId,
            nativeAtomic, atomic, usedModelFactory ? "VC model factory" : "RpAtomicClone",
            skinned ? ("slot5/SLhand HAnim") : ("slot5/SLhand RwFrame"));
        Log(line);
        return true;
    }

    static void LogCreateFailureOccasionally(CPed* ped, CWeapon* weapon, bool skinned) {
        const unsigned int frame = CTimer::m_FrameCounter;
        if (gLoggedCreateFailure && frame - gCreateFailureFrame < 300)
            return;
        gLoggedCreateFailure = true;
        gCreateFailureFrame = frame;

        char line[512];
        RpHAnimHierarchy* hierarchy = 0;
        const bool hasHierarchy = GetSkinnedPedHierarchy(ped, hierarchy);
        const void* nativeWeapon = ped ? reinterpret_cast<void*>(GetNativeWeaponAtomicIdb(ped)) : 0;
        const int resolvedModel = ped ? ResolveEquippedWeaponModelId(ped, weapon) : -999;
        const bool mac10 = ped && weapon ? IsMac10Weapon(ped, weapon) : false;
        std::snprintf(line, sizeof(line),
            "DualWieldVC v17: create deferred. ped=%p state=%d inVeh=%d weapon=%d modelField=%d resolvedModel=%d mac10=%d skinned=%d hierarchy=%p nativeWeapon=%p.",
            ped, ped ? static_cast<int>(ped->m_ePedState) : -1,
            ped ? static_cast<int>(ped->m_bInVehicle) : -1,
            weapon ? static_cast<int>(weapon->m_eWeaponType) : -1,
            ped ? GetNativeWeaponModelIdIdb(ped) : -999, resolvedModel, mac10 ? 1 : 0,
            skinned ? 1 : 0, hasHierarchy ? hierarchy : 0, nativeWeapon);
        Log(line);
    }

    static void UpdateLeftWeapon(CPed* ped) {
        if (!IsEligiblePlayer(ped)) {
            if (gLeft.owner == ped)
                DestroyLeftWeapon();
            return;
        }

        if (gLeft.owner && gLeft.owner != ped)
            DestroyLeftWeapon();

        if (gLeft.owner == ped && gLeft.ownerClump && gLeft.ownerClump != GetPedClumpIdb(ped))
            DestroyLeftWeapon();

        CWeapon* weapon = ped->GetWeapon();
        const int modelId = ResolveEquippedWeaponModelId(ped, weapon);
        RpHAnimHierarchy* hierarchy = 0;
        const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);
        const bool mac10 = IsMac10Weapon(ped, weapon);
        const bool haveNativeAtomic = GetNativeWeaponAtomicIdb(ped) != 0;

        // MAC-10 can create its explicit second visual from MODEL_INGRAMSL even when
        // the native atomic is not usable as a cloning source. Pistol/Tec-9 still
        // require the native atomic because their second visual is cloned from it.
        if (!weapon || modelId < 0 || (!mac10 && !haveNativeAtomic)) {
            DestroyLeftWeapon();
            LogCreateFailureOccasionally(ped, weapon, skinned);
            return;
        }

        if (IsAtomicAliveForOwner(ped)) {
            if (gLeft.weaponType != weapon->m_eWeaponType ||
                gLeft.modelId != modelId || gLeft.skinnedSource != skinned) {
                DestroyLeftWeapon();
            }
            else if (!skinned) {
                RwFrame* currentSourceHand = GetPedFrameSafe(ped, SecondHandFrameIndex());
                if (!currentSourceHand || gLeft.sourceHandFrame != currentSourceHand)
                    DestroyLeftWeapon();
                else {
                    UpdateLeftWeaponWorldTransform();
                    return;
                }
            }
            else {
                const int handNode = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), SecondHandBoneId());
                if (!hierarchy || !GetSkinBoneMatrix(hierarchy, handNode))
                    DestroyLeftWeapon();
                else {
                    UpdateLeftWeaponWorldTransform();
                    return;
                }
            }
        }
        else if (gLeft.atomic || gLeft.helperFrame) {
            DestroyLeftWeapon();
        }

        if (!CreateLeftWeapon(ped))
            LogCreateFailureOccasionally(ped, weapon, skinned);
    }

    struct Basis3 {
        CVector right;
        CVector up;
        CVector at;
    };

    struct MirroredArmChain {
        bool valid;
        RwFrame* nativeUpper;
        RwFrame* nativeLower;
        RwFrame* nativeHand;
        RwFrame* mirrorUpper;
        RwFrame* mirrorLower;
        RwFrame* mirrorHand;

        MirroredArmChain()
            : valid(false), nativeUpper(0), nativeLower(0), nativeHand(0),
            mirrorUpper(0), mirrorLower(0), mirrorHand(0) {
        }
    };

    static CVector Vec3(float x, float y, float z) {
        CVector v;
        v.x = x;
        v.y = y;
        v.z = z;
        return v;
    }

    static float Dot3(const CVector& a, const CVector& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static CVector Add3(const CVector& a, const CVector& b) {
        return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
    }

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
        }
        else {
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
                "DualWieldVC v17: arm-only CPedIK replay used for opposite arm blend=%.2f worldAngle=%.3f localAngle=%.3f slope=%.3f leftUpper=%p leftLower=%p leftHand=%p leftProbe=%p moved=%d.",
                blend, aimAngle, localArmAngle, aimSlope, leftUpper, leftLower, leftHand, leftProbe, moved ? 1 : 0);
            Log(line);
            gLoggedFirstNativeIkReplay = true;
        }
        return moved;
    }

    static CVector Lerp3(const CVector& a, const CVector& b, float t) {
        return Add3(a, Scale3(Sub3(b, a), ClampFloat(t, 0.0f, 1.0f)));
    }

    static float Distance3(const CVector& a, const CVector& b) {
        const CVector d = Sub3(a, b);
        const float lenSq = Dot3(d, d);
        if (!IsFiniteFloat(lenSq) || lenSq <= 0.0f)
            return 0.0f;
        return std::sqrt(lenSq);
    }

    static CVector GetPedRightSafe(CPed* ped) {
        if (!ped)
            return Vec3(1.0f, 0.0f, 0.0f);
        CVector r = ped->GetRight();
        r.z = 0.0f;
        return Normalize3(r, Vec3(1.0f, 0.0f, 0.0f));
    }

    static CVector GetAimDirectionWorld(CPed* ped) {
        float aimAngle = 0.0f;
        float aimSlope = 0.0f;
        if (ReadPedFloatIdb(ped, OFF_PED_AIM_ANGLE, aimAngle)) {
            ReadPedFloatIdb(ped, OFF_PED_AIM_SLOPE, aimSlope);
            aimSlope = aimSlope * gConfig.aimPitchScale + gConfig.aimPitchOffset;
            if (aimSlope > 1.20f) aimSlope = 1.20f;
            if (aimSlope < -1.20f) aimSlope = -1.20f;
            const float c = std::cos(aimSlope);
            return Normalize3(Vec3(-std::sin(aimAngle) * c, std::cos(aimAngle) * c, std::sin(aimSlope)), Vec3(0.0f, 1.0f, 0.0f));
        }

        // Fallback from the ped's right vector if the raw aim angle is not readable.
        const CVector right = GetPedRightSafe(ped);
        return Normalize3(Vec3(-right.y, right.x, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    }

    static bool IsReloadingActiveWeapon(CPed* ped) {
        CWeapon* weapon = ped ? ped->GetWeapon() : 0;
        return weapon && (weapon->m_eWeaponState == WEAPONSTATE_RELOADING || weapon->m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO);
    }

    static float ComputeSecondArmBlend(CPed* ped) {
        if (!ped || !gConfig.leftArmIK)
            return 0.0f;

        const bool activeAim = ped->bFiringWeapon || ped->bIsAimingGun || ped->bIsPointingGunAt ||
            ped->m_ePedState == PEDSTATE_ATTACK || ped->m_ePedState == PEDSTATE_AIMGUN || gAimGraceFramesRemaining > 0;
        if (gConfig.armIKRequiresFire && !activeAim)
            return 0.0f;
        if (!gConfig.armIKWhileMoving && IsPedMovingForArmIK(ped))
            return gConfig.armHoldBlend;
        if (IsReloadingActiveWeapon(ped))
            return gConfig.armReloadBlend;
        return activeAim ? gConfig.armAimBlend : gConfig.armHoldBlend;
    }

    // IDB-backed layout used by VC's own CPedIK::PointGunInDirectionUsingArm:
    // AnimBlendFrameData + 0x10 -> skinned HAnim interpolation frame,
    // interpolation frame + 0x08 -> RtQuat orientation. CPedIK rotates those quats
    // for the native arm, then CEntity::UpdateRpHAnim bakes them into pMatrixArray.
    struct RawRtQuat {
        float x, y, z, w; // RtQuat: imag.xyz + real
    };

    static RawRtQuat* GetPedAnimQuatIdb(CPed* ped, int frameIndex) {
        AnimBlendFrameData* data = GetPedFrameDataIdb(ped, frameIndex);
        if (!IsReadableAddress(data, 0x18))
            return 0;
        void* interp = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(data) + 0x10);
        if (!IsReadableAddress(interp, 0x18))
            return 0;
        RawRtQuat* q = reinterpret_cast<RawRtQuat*>(reinterpret_cast<uintptr_t>(interp) + 0x08);
        return IsReadableAddress(q, sizeof(RawRtQuat)) ? q : 0;
    }

    static CVector WorldAxisToParentLocal(const CVector& axisWorld, const RwMatrix& parentWorld) {
        // SA's IKChain::MoveBonesToTarget transforms the world correction axis by the
        // inverse parent rotation before post-concatenating it into the local keyframe
        // quaternion. For an orthonormal RW basis, dotting against parent basis vectors
        // is the same inverse-rotation operation.
        return Normalize3(Vec3(
            Dot3(axisWorld, Vec3(parentWorld.right.x, parentWorld.right.y, parentWorld.right.z)),
            Dot3(axisWorld, Vec3(parentWorld.up.x, parentWorld.up.y, parentWorld.up.z)),
            Dot3(axisWorld, Vec3(parentWorld.at.x, parentWorld.at.y, parentWorld.at.z))
        ), Vec3(0.0f, 0.0f, 1.0f));
    }

    static bool RotatePedAnimQuatTowardDirection(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        int frameIndex,
        int boneId,
        int parentBoneId,
        int childBoneId,
        const CVector& desiredWorldDir,
        float blend,
        float maxRadians
    ) {
        if (!ped || !hierarchy || blend <= 0.001f)
            return false;
        RawRtQuat* q = GetPedAnimQuatIdb(ped, frameIndex);
        RwMatrix* bone = GetSkinBoneMatrix(hierarchy, boneId);
        RwMatrix* parent = GetSkinBoneMatrix(hierarchy, parentBoneId);
        RwMatrix* child = GetSkinBoneMatrix(hierarchy, childBoneId);
        if (!q || !bone || !parent || !child)
            return false;

        const CVector bonePos = Vec3(bone->pos.x, bone->pos.y, bone->pos.z);
        const CVector childPos = Vec3(child->pos.x, child->pos.y, child->pos.z);
        const CVector currentDir = Normalize3(Sub3(childPos, bonePos), desiredWorldDir);
        const CVector desiredDir = Normalize3(desiredWorldDir, currentDir);
        const float dot = ClampFloat(Dot3(currentDir, desiredDir), -1.0f, 1.0f);
        if (dot > 0.99995f)
            return true;

        CVector axisWorld = Cross3(currentDir, desiredDir);
        const float axisLenSq = Dot3(axisWorld, axisWorld);
        if (!IsFiniteFloat(axisLenSq) || axisLenSq < 1.0e-8f)
            return true;
        axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));
        CVector axisLocal = WorldAxisToParentLocal(axisWorld, *parent);

        float radians = std::acos(dot) * ClampFloat(blend, 0.0f, 1.0f);
        if (maxRadians > 0.0f)
            radians = ClampFloat(radians, -maxRadians, maxRadians);
        if (std::fabs(radians) < 1.0e-5f)
            return true;

        // Use VC/RenderWare's exact quaternion combiner. rwCOMBINEPOSTCONCAT == 2,
        // matching the local-orientation update used by the game's gun IK and SA's
        // IK chain after the correction axis is transformed into parent-local space.
        typedef RawRtQuat* (__cdecl* RtQuatRotateRawFn)(RawRtQuat*, const CVector*, float, int);
        RtQuatRotateRawFn rotateQuat = reinterpret_cast<RtQuatRotateRawFn>(0x65ABD0);
        if (!rotateQuat)
            return false;
        const float degrees = radians * 57.29577951308232f;
        rotateQuat(q, &axisLocal, degrees, 2);
        // VC's native CPedIK sets bit 0x20 at CPed+0x154 after editing arm quats.
        // Mirror that bookkeeping before rebaking the hierarchy.
        if (IsReadableAddress(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ped) + 0x154), 1))
            *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(ped) + 0x154) |= 0x20;
        return IsFiniteFloat(q->x) && IsFiniteFloat(q->y) && IsFiniteFloat(q->z) && IsFiniteFloat(q->w);
    }

    static bool MirrorNativeGunArmPoseIntoSecondArm(
        CPed* ped,
        RpHAnimHierarchy* hierarchy,
        EntityUpdateRpHAnimFn updateFn,
        CEntity* entity,
        float blend
    ) {
        if (!ped || !hierarchy || !updateFn || !entity || blend <= 0.001f)
            return false;

        // Snapshot the native VC weapon arm AFTER the game's own UpdateRpHAnim. This
        // already includes VC's actual aim, locomotion and reload animation state.
        // We mirror its segment directions, rather than inventing a target elbow/pole.
        RwMatrix* nativeUpper = GetSkinBoneMatrix(hierarchy, NativeUpperBoneId());
        RwMatrix* nativeLower = GetSkinBoneMatrix(hierarchy, NativeLowerBoneId());
        RwMatrix* nativeHand = GetSkinBoneMatrix(hierarchy, NativeHandBoneId());
        if (!nativeUpper || !nativeLower || !nativeHand)
            return false;

        const CVector nShoulder = Vec3(nativeUpper->pos.x, nativeUpper->pos.y, nativeUpper->pos.z);
        const CVector nElbow = Vec3(nativeLower->pos.x, nativeLower->pos.y, nativeLower->pos.z);
        const CVector nHand = Vec3(nativeHand->pos.x, nativeHand->pos.y, nativeHand->pos.z);
        const CVector mirrorNormal = Normalize3(ped->GetRight(), Vec3(1.0f, 0.0f, 0.0f));
        const CVector desiredUpperDir = ReflectDirection(Sub3(nElbow, nShoulder), mirrorNormal);
        const CVector desiredLowerDir = ReflectDirection(Sub3(nHand, nElbow), mirrorNormal);

        // Upper arm: left upper (slot 3 / HAnim 32), parented by left clavicle 31.
        bool changed = RotatePedAnimQuatTowardDirection(
            ped, hierarchy,
            SecondUpperFrameIndex(), SecondUpperBoneId(), SecondClavicleBoneId(), SecondLowerBoneId(),
            desiredUpperDir, blend, 0.95f
        );
        if (!changed)
            return false;
        updateFn(entity);

        // Lower arm: slot 13 / HAnim 33, parented by left upper 32.
        hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        changed = RotatePedAnimQuatTowardDirection(
            ped, hierarchy,
            SecondLowerFrameIndex(), SecondLowerBoneId(), SecondUpperBoneId(), SecondHandBoneId(),
            desiredLowerDir, blend, 1.10f
        );
        if (!changed)
            return false;
        updateFn(entity);

        // One small hand-direction correction keeps the cloned weapon aligned with the
        // mirrored native hand without reflecting an entire matrix (which would have a
        // negative determinant). We only rotate the local hand quaternion.
        hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false;
        RwMatrix* secondHand = GetSkinBoneMatrix(hierarchy, SecondHandBoneId());
        RwMatrix* secondLower = GetSkinBoneMatrix(hierarchy, SecondLowerBoneId());
        nativeHand = GetSkinBoneMatrix(hierarchy, NativeHandBoneId());
        if (secondHand && secondLower && nativeHand) {
            const CVector currentAt = Normalize3(Vec3(secondHand->at.x, secondHand->at.y, secondHand->at.z), desiredLowerDir);
            const CVector desiredAt = Normalize3(ReflectDirection(Vec3(nativeHand->at.x, nativeHand->at.y, nativeHand->at.z), mirrorNormal), currentAt);
            const float dot = ClampFloat(Dot3(currentAt, desiredAt), -1.0f, 1.0f);
            if (dot < 0.99995f) {
                CVector axisWorld = Cross3(currentAt, desiredAt);
                const float axisLenSq = Dot3(axisWorld, axisWorld);
                RawRtQuat* handQ = GetPedAnimQuatIdb(ped, SecondHandFrameIndex());
                if (handQ && axisLenSq > 1.0e-8f) {
                    axisWorld = Scale3(axisWorld, 1.0f / std::sqrt(axisLenSq));
                    CVector axisLocal = WorldAxisToParentLocal(axisWorld, *secondLower);
                    float radians = std::acos(dot) * ClampFloat(blend * 0.65f, 0.0f, 1.0f);
                    radians = ClampFloat(radians, -0.70f, 0.70f);
                    typedef RawRtQuat* (__cdecl* RtQuatRotateRawFn)(RawRtQuat*, const CVector*, float, int);
                    reinterpret_cast<RtQuatRotateRawFn>(0x65ABD0)(handQ, &axisLocal, radians * 57.29577951308232f, 2);
                    if (IsReadableAddress(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ped) + 0x154), 1))
                        *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(ped) + 0x154) |= 0x20;
                    updateFn(entity);
                }
            }
        }

        return true;
    }

    static bool ApplyCurrentV3Mirror(
        CPed* ped,
        EntityUpdateRpHAnimFn updateFn,
        CEntity* entity,
        float* outBlend = 0
    ) {
        if (outBlend)
            *outBlend = 0.0f;
        if (!gConfig.leftArmIK || !IsEligiblePlayer(ped) || !IsAtomicAliveForOwner(ped) || !updateFn || !entity)
            return false;

        const float blend = ComputeSecondArmBlend(ped);
        if (blend <= 0.001f)
            return false;

        RpHAnimHierarchy* hierarchy = 0;
        if (!GetSkinnedPedHierarchy(ped, hierarchy))
            return false; // stock Tommy is skinned; do not mix the old RwFrame mirror into this path.

        const bool moved = MirrorNativeGunArmPoseIntoSecondArm(ped, hierarchy, updateFn, entity, blend);
        if (moved && outBlend)
            *outBlend = blend;
        if (moved && !gLoggedFirstNativeIkReplay) {
            const int s3 = GetPedFrameNodeIdSafe(ped, SecondUpperFrameIndex(), -1);
            const int s5 = GetPedFrameNodeIdSafe(ped, SecondHandFrameIndex(), -1);
            const int s13 = GetPedFrameNodeIdSafe(ped, SecondLowerFrameIndex(), -1);
            const int s15 = GetPedFrameNodeIdSafe(ped, SecondClavicleFrameIndex(), -1);
            const int n4 = GetPedFrameNodeIdSafe(ped, NativeUpperFrameIndex(), -1);
            const int n6 = GetPedFrameNodeIdSafe(ped, NativeHandFrameIndex(), -1);
            const int n14 = GetPedFrameNodeIdSafe(ped, NativeLowerFrameIndex(), -1);
            const int n16 = GetPedFrameNodeIdSafe(ped, NativeClavicleFrameIndex(), -1);
            char line[512];
            std::snprintf(line, sizeof(line),
                "DualWieldVC v17: quaternion pose mirror active native slots 4/14/6/16 nodes=%d/%d/%d/%d -> second slots 3/13/5/15 nodes=%d/%d/%d/%d blend=%.2f.",
                n4, n14, n6, n16, s3, s13, s5, s15, blend);
            Log(line);
            gLoggedFirstNativeIkReplay = true;
        }
        return moved;
    }

    static void ApplyPreRenderArmPose(CPed* ped, EntityUpdateRpHAnimFn updateFn, CEntity* entity) {
        // VC has already baked its native body pose once when this bridge is entered.
        // v14 rotates the opposite arm's interpolation quaternions, and each correction
        // is followed by VC's own UpdateRpHAnim so the skin matrix array stays coherent.
        const unsigned int frame = CTimer::m_FrameCounter;
        if (gLastArmPoseFrame == frame)
            return;
        gLastArmPoseFrame = frame;

        float blend = 0.0f;
        if (!ApplyCurrentV3Mirror(ped, updateFn, entity, &blend))
            return;

        ++gRenderMirrorHits;
        if (gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();

        if (!gLoggedFirstRenderMirror) {
            RpHAnimHierarchy* hierarchy = 0;
            const bool skinned = GetSkinnedPedHierarchy(ped, hierarchy);
            char line[384];
            std::snprintf(line, sizeof(line),
                "DualWieldVC v17: post-quaternion UpdateRpHAnim pose active second=slot5/SLhand blend=%.2f skinned=%d hierarchy=%p.",
                blend, skinned ? 1 : 0, hierarchy);
            Log(line);
            gLoggedFirstRenderMirror = true;
        }
    }

    static void PrepareOppositePoseForFire(CPed* ped) {
        // Do not run body posing from the attack hook: CPed::Attack may call the fire
        // hook more than once per frame, and body posing now belongs to the post-
        // UpdateRpHAnim bridge. Keep this function to synchronize the private weapon
        // transform before the selected hand muzzle is sampled.
        if (ped && gLeft.owner == ped)
            UpdateLeftWeaponWorldTransform();
    }

    static bool IsExecutablePointerLocal(const void* pointer) {
        if (!pointer)
            return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(pointer, &mbi, sizeof(mbi)))
            return false;
        const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.State == MEM_COMMIT) && ((mbi.Protect & executable) != 0) && !(mbi.Protect & PAGE_GUARD);
    }

    static bool RenderAtomicViaVcCallback(RpAtomic* atomic) {
        if (!atomic || !IsReadableAddress(atomic, 0x4C))
            return false;

        // Vice City CPed::Render does not call a generic wrapper here. At 0x4FE2CB it
        // executes: push atomic; call dword ptr [atomic + 0x48]. Use the same callback
        // slot for the standalone left-hand clone so custom weapon pipelines/shader
        // callbacks see a normal weapon atomic render, not a synthetic RenderWare call.
        void* callback = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(atomic) + 0x48);
        if (!IsExecutablePointerLocal(callback))
            return false;
        typedef void(__cdecl* AtomicRenderCallback)(RpAtomic*);
        reinterpret_cast<AtomicRenderCallback>(callback)(atomic);
        return true;
    }

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

        // v14: the visible clone's final world matrix is the muzzle authority. v12 used
        // the raw hand matrix here even though the visible weapon had a separate grip
        // correction, allowing shot direction/origin to disagree with the model.
        RwMatrix weaponWorld;
        bool haveWeaponWorld = false;
        if (gLeft.owner == ped && gLeft.helperFrame && UpdateLeftWeaponWorldTransform()) {
            RwMatrix* helper = RwFrameGetMatrix(gLeft.helperFrame);
            if (helper && IsFiniteRwMatrix(*helper)) {
                weaponWorld = *helper; // helper is a private root, therefore world-space
                haveWeaponWorld = true;
            }
        }

        if (!haveWeaponWorld && !GetSecondHandWorldMatrix(ped, weaponWorld)) {
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

        if (!TransformPointByRwMatrix(offset, weaponWorld, out))
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

        CVector leftSource;
        if (!ComputeLeftFireSource(ped, rightSource, leftSource))
            return firedRight;

        ++gSecondShotAttempts;
        const bool firedLeft = fireFn(weapon, shooter, &leftSource);
        if (firedLeft) {
            ++gSecondShotSuccess;
            if (!gLoggedFirstSecondShot) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldVC v17: second native shot succeeded at muzzle %.3f %.3f %.3f.",
                    leftSource.x, leftSource.y, leftSource.z);
                Log(line);
                gLoggedFirstSecondShot = true;
            }
        }

        return firedRight;
    }

    static bool __fastcall AttackFireHook1(CWeapon* weapon, void*, CEntity* shooter, CVector* rightSource) {
        return AttackFireHookImpl(gFirePatch1, weapon, shooter, rightSource);
    }

    static bool __fastcall AttackFireHook2(CWeapon* weapon, void*, CEntity* shooter, CVector* rightSource) {
        return AttackFireHookImpl(gFirePatch2, weapon, shooter, rightSource);
    }

    static bool DecodeRelativeCall(uintptr_t address, uintptr_t& target, int32_t& rel) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
        if (p[0] != 0xE8)
            return false;
        std::memcpy(&rel, p + 1, sizeof(rel));
        target = address + 5 + rel;
        return true;
    }

    static bool IsExecutableAddress(uintptr_t address) {
        if (!address)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;

        const DWORD prot = mbi.Protect & 0xFFu;
        return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    }

    static bool InstallCallPatch(uintptr_t address, uintptr_t expectedTarget, void* hook, CallPatch& patch);

    static bool ResolveAttackFireCalls(uintptr_t& first, uintptr_t& second) {
        first = second = 0;
        HMODULE exe = GetModuleHandleA(0);
        if (!exe)
            return false;
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const uint8_t prefix[] = { 0x8D,0x0C,0xCD,0,0,0,0,0x8D,0x0C,0x49,0x01,0xD9,0x81,0xC1,0x08,0x04,0,0,0xE8 };
        const uint8_t suffix[] = { 0x0F,0xBE,0x83,0x04,0x05,0,0 };
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t hits[4] = {};
        int hitCount = 0;

        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const uint8_t* begin = reinterpret_cast<const uint8_t*>(exe) + sec[i].VirtualAddress;
            const size_t size = sec[i].Misc.VirtualSize;
            if (size < sizeof(prefix) + 4 + sizeof(suffix))
                continue;
            for (size_t off = 0; off + sizeof(prefix) + 4 + sizeof(suffix) <= size; ++off) {
                if (std::memcmp(begin + off, prefix, sizeof(prefix)) != 0)
                    continue;
                if (std::memcmp(begin + off + sizeof(prefix) + 4, suffix, sizeof(suffix)) != 0)
                    continue;
                const uintptr_t call = reinterpret_cast<uintptr_t>(begin + off + sizeof(prefix) - 1);
                uintptr_t target = 0; int32_t rel = 0;
                if (!DecodeRelativeCall(call, target, rel) || !IsExecutableAddress(target))
                    continue;
                if (hitCount < 4)
                    hits[hitCount] = call;
                ++hitCount;
            }
        }

        if (hitCount != 2)
            return false;
        if (hits[0] > hits[1]) { uintptr_t tmp = hits[0]; hits[0] = hits[1]; hits[1] = tmp; }
        first = hits[0]; second = hits[1];
        return true;
    }

    static bool gFireHooksResolved = false;

    static bool EnsureFireHooksInstalled() {
        if (gFirePatch1.installed && gFirePatch2.installed)
            return true;
        if (gFireHooksResolved)
            return gFirePatch1.installed || gFirePatch2.installed;
        gFireHooksResolved = true;

        uintptr_t call1 = 0, call2 = 0;
        if (!ResolveAttackFireCalls(call1, call2)) {
            // Exact 1.0 fallback from the supplied Vice City IDB.
            call1 = ADDR_ATTACK_FIRE_CALL_1;
            call2 = ADDR_ATTACK_FIRE_CALL_2;
        }

        uintptr_t target1 = 0, target2 = 0; int32_t rel1 = 0, rel2 = 0;
        const bool dec1 = DecodeRelativeCall(call1, target1, rel1) && IsExecutableAddress(target1);
        const bool dec2 = DecodeRelativeCall(call2, target2, rel2) && IsExecutableAddress(target2);
        const bool ok1 = dec1 && InstallCallPatch(call1, target1, reinterpret_cast<void*>(&AttackFireHook1), gFirePatch1);
        const bool ok2 = dec2 && InstallCallPatch(call2, target2, reinterpret_cast<void*>(&AttackFireHook2), gFirePatch2);

        char line[512];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v17: fire resolve call1=%p target=%p %s; call2=%p target=%p %s.",
            reinterpret_cast<void*>(call1), reinterpret_cast<void*>(target1), ok1 ? "HOOKED" : "MISS",
            reinterpret_cast<void*>(call2), reinterpret_cast<void*>(target2), ok2 ? "HOOKED" : "MISS");
        Log(line);
        if (!ok1 && !ok2) {
            Log("DualWieldVC v17: no CPed::Attack fire hooks installed; second-shot path disabled.");
            gConfig.doubleShot = false;
        }
        return ok1 || ok2;
    }

    static bool InstallCallPatch(uintptr_t address, uintptr_t expectedTarget, void* hook, CallPatch& patch) {
        uintptr_t currentTarget = 0;
        int32_t originalRel = 0;
        if (!DecodeRelativeCall(address, currentTarget, originalRel))
            return false;

        const uintptr_t hookTarget = reinterpret_cast<uintptr_t>(hook);
        if (!hookTarget || currentTarget == hookTarget)
            return false;

        const bool chained = currentTarget != expectedTarget;
        if (chained) {
            if (!gConfig.chainExistingCallHooks || !IsExecutableAddress(currentTarget))
                return false;
        }

        const intptr_t delta = hookTarget - (address + 5);
        if (delta < INT32_MIN || delta > INT32_MAX)
            return false;
        const int32_t newRel = static_cast<int32_t>(delta);

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        std::memcpy(reinterpret_cast<void*>(address + 1), &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 5);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), 5, oldProtect, &ignored);

        patch.address = address;
        patch.previousTarget = currentTarget;
        patch.hookTarget = hookTarget;
        patch.originalRel = originalRel;
        patch.installed = true;
        patch.chained = chained;
        return true;
    }

    static void RestoreCallPatch(CallPatch& patch) {
        if (!patch.installed)
            return;

        // If another ASI patched this CALL after us, leave its newer hook intact.
        uintptr_t currentTarget = 0;
        int32_t currentRel = 0;
        if (!DecodeRelativeCall(patch.address, currentTarget, currentRel) ||
            currentTarget != patch.hookTarget) {
            patch = CallPatch();
            return;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(patch.address), 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(reinterpret_cast<void*>(patch.address + 1), &patch.originalRel, sizeof(patch.originalRel));
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patch.address), 5);
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(patch.address), 5, oldProtect, &ignored);
        }
        patch = CallPatch();
    }

    static void RunPedRenderBridgeWork(CPed* ped) {
        if (!ped || ped->m_nPedType != PEDTYPE_PLAYER1)
            return;
        UpdateLeftWeapon(ped);
    }

    static void __fastcall PedRenderBridge(CEntity* entity, void*) {
        CPed* ped = reinterpret_cast<CPed*>(entity);

        // Body arm posing must NOT happen here. By this point CPed::PreRender has
        // already called CEntity::UpdateRpHAnim and materialized the skinned body
        // matrices. v7 edited the wrong thing at this phase, which left the visible
        // arm unchanged. This bridge is now only responsible for drawing the
        // standalone second weapon after the body draw.
        EntityRenderFn renderFn = reinterpret_cast<EntityRenderFn>(gRenderPatch.previousTarget);
        if (renderFn)
            renderFn(entity);

        if (ped && ped->m_nPedType == PEDTYPE_PLAYER1) {
            UpdateLeftWeapon(ped);
            RenderStandaloneLeftWeapon(ped);
        }
    }

    static void __fastcall PedPreRenderUpdateRpHAnimBridge(CEntity* entity, void*) {
        CPed* ped = reinterpret_cast<CPed*>(entity);

        EntityUpdateRpHAnimFn updateFn = reinterpret_cast<EntityUpdateRpHAnimFn>(gPreRenderAnimPatch.previousTarget);
        if (updateFn)
            updateFn(entity);

        // SA-style backport layer: VC first bakes its normal one-gun body pose, then
        // we solve the selected second arm against that finished pose before CPed::Render.
        // This avoids poisoning VC's native CPedIK state and avoids the v8/v9 slot-remap mess.
        if (ped && ped->m_nPedType == PEDTYPE_PLAYER1 && updateFn) {
            // Materialize the second visual first. The added arm is not allowed to pose
            // unless a real second gun exists, preventing the v16 "arm only" failure.
            UpdateLeftWeapon(ped);
            ApplyPreRenderArmPose(ped, updateFn, entity);
        }
    }

    static bool gRenderHookResolved = false;

    static bool EnsureRenderBridgeInstalled() {
        if (gRenderPatch.installed)
            return true;
        if (gRenderHookResolved)
            return gRenderPatch.installed;
        gRenderHookResolved = true;

        uintptr_t target = 0; int32_t rel = 0;
        const bool decoded = DecodeRelativeCall(ADDR_PED_RENDER_ENTITY_CALL, target, rel) && IsExecutableAddress(target);
        const bool ok = decoded && InstallCallPatch(ADDR_PED_RENDER_ENTITY_CALL, ADDR_ENTITY_RENDER,
            reinterpret_cast<void*>(&PedRenderBridge), gRenderPatch);

        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v17: render bridge call=%p target=%p %s.",
            reinterpret_cast<void*>(ADDR_PED_RENDER_ENTITY_CALL), reinterpret_cast<void*>(target), ok ? "HOOKED" : "MISS");
        Log(line);
        return ok;
    }

    static bool gPreRenderHookResolved = false;

    static bool EnsurePreRenderArmBridgeInstalled() {
        if (gPreRenderAnimPatch.installed)
            return true;
        if (gPreRenderHookResolved)
            return gPreRenderAnimPatch.installed;
        gPreRenderHookResolved = true;

        uintptr_t target = 0; int32_t rel = 0;
        const bool decoded = DecodeRelativeCall(ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL, target, rel) && IsExecutableAddress(target);
        const bool ok = decoded && InstallCallPatch(ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL, ADDR_ENTITY_UPDATE_RPHANIM,
            reinterpret_cast<void*>(&PedPreRenderUpdateRpHAnimBridge), gPreRenderAnimPatch);

        char line[360];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v17: PreRender UpdateRpHAnim bridge call=%p target=%p %s.",
            reinterpret_cast<void*>(ADDR_PED_PRERENDER_UPDATE_RPHANIM_CALL), reinterpret_cast<void*>(target), ok ? "HOOKED" : "MISS");
        Log(line);
        if (!ok)
            Log("DualWieldVC v17: no PreRender arm bridge; body arm pose will not be visible.");
        return ok;
    }

    static void LogRuntimeSummary() {
        if (gLoggedRuntimeSummary)
            return;
        char line[320];
        std::snprintf(line, sizeof(line),
            "DualWieldVC v17 summary: render mirrors=%u second-shot attempts=%u success=%u skinnedMode=%d.",
            gRenderMirrorHits, gSecondShotAttempts, gSecondShotSuccess,
            gLoggedSkinnedPedMode ? 1 : 0);
        Log(line);
        gLoggedRuntimeSummary = true;
    }

    static void OnPedDestroy(CPed* ped) {
        if (ped && ped == gLeft.owner)
            DestroyLeftWeapon();
    }

    static void OnPedSetModel(CPed* ped) {
        if (!ped || ped != gLeft.owner)
            return;
        Log("DualWieldVC v17: pedSetModelEvent observed; destroying standalone second weapon before rebinding frames.");
        DestroyLeftWeapon();
    }

    class Mod {
    public:
        Mod() {
            BuildSiblingPath(gIniPath, sizeof(gIniPath), "DualWieldVC.ini");
            BuildSiblingPath(gLogPath, sizeof(gLogPath), "DualWieldVC_v17.log");
            std::remove(gLogPath);
            LoadConfig();

            Log("DualWieldVC v17: loaded. Pistol/Tec-9/MAC-10 enabled; SA-style per-hand muzzle and local-quaternion second-arm mirror active.");
            Log("DualWieldVC v17: fire hooks, PreRender arm bridge and render weapon bridge resolve on first gameProcess tick.");

            Events::gameProcessEvent += [] {
                EnsureFireHooksInstalled();
                EnsurePreRenderArmBridgeInstalled();
                EnsureRenderBridgeInstalled();
                };
            Events::pedSetModelEvent += [](CPed* ped, int) {
                OnPedSetModel(ped);
                };
            Events::pedDtorEvent += [](CPed* ped) {
                OnPedDestroy(ped);
                };
            Events::restartGameEvent += [] {
                DestroyLeftWeapon();
                };
            Events::shutdownRwEvent += [] {
                LogRuntimeSummary();
                DestroyLeftWeapon();
                RestoreCallPatch(gFirePatch1);
                RestoreCallPatch(gFirePatch2);
                RestoreCallPatch(gRenderPatch);
                RestoreCallPatch(gPreRenderAnimPatch);
                };
        }

        ~Mod() {
            // RW teardown is handled by shutdownRwEvent while the engine is alive.
            LogRuntimeSummary();
            RestoreCallPatch(gPreRenderAnimPatch);
            RestoreCallPatch(gFirePatch2);
            RestoreCallPatch(gFirePatch1);
        }
    };

    static Mod gMod;
}
