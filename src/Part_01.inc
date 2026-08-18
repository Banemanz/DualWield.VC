/*
    DualWieldVC v12 - Vice City native-slot + SA-style quaternion arm backport
    GTA Vice City 1.0 EN dual-wield support for the stock Pistol, Uzi and Tec-9.

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
      - The clone is anchored to the selected hand bone every frame. It is not placed
        by mirroring the native weapon matrix.
      - The second shot muzzle is computed like SA CTaskSimpleUseGun::FireGun: transform
        CWeaponInfo::m_vecFireOffset through the selected hand bone. It is not a
        reflected copy of the native muzzle.
      - CPed::PreRender's CEntity::UpdateRpHAnim CALL is patched directly. VC first
        bakes the normal body pose, then v12 mirrors the native weapon-arm segment directions into the
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

