# DualWieldVC v12 - native-slot + quaternion pose mirror

Target: classic GTA Vice City 1.0 EN matching the supplied VC IDB dump.
Supported weapons: Pistol, Uzi, Tec-9.

## What v11 got wrong

v11 exposed a `SecondHandRight` guess and defaulted it to 1. That directly contradicted VC's stock render path. `CPed::Render` reads `CPed + 0x1C0`, which is `m_apFrames[6]`, resolves that node in the HAnim hierarchy, copies its matrix into `m_pCurWeaponAtomic`, and renders the native weapon. Slot 6 maps to HAnim 24 (`SRhand`). Therefore the second weapon must use the opposite stock slot 5 / HAnim 34 (`SLhand`).

With v11's default, the clone, second muzzle and "second-arm" solver could all overlap the native weapon side. The log could say every hook and solver succeeded while nothing visibly changed.

## v12 arm architecture

v12 does not move final skin matrices and does not replay VC's right-arm IK against swapped slots.

VC's own `CPedIK::PointGunInDirectionUsingArm` edits HAnim interpolation-frame quaternions and later `CEntity::UpdateRpHAnim` bakes those local orientations into the matrix array. The supplied IDB shows the right gun-arm path using frame slots 16/14/4/6. `ConvertPedNode2BoneTag` maps the corresponding opposite slots 15/13/3/5 to left clavicle/forearm/upper-arm/hand.

v12 first lets VC bake its real native pose. It snapshots the native weapon arm segment directions, reflects those directions across Tommy's sagittal plane, rotates the opposite upper/lower/hand interpolation quaternions in parent-local space, and reruns VC's own `UpdateRpHAnim` after each correction. This is deliberately similar to SA's IK-chain architecture: local bone orientation correction followed by hierarchy reconstruction, rather than teleporting world-space elbow/hand matrices.

## Weapon rendering

The native weapon remains completely owned/rendered by Vice City. The second weapon is a standalone clone attached to slot 5 / HAnim 34 every frame.

The default second-weapon grip correction is taken from San Andreas' actual twin-pistol render path: X rotation 180 degrees followed by translation `(0.04, -0.05, 0.0)`.

## Log

`DualWieldVC_v12.log` is created beside the ASI.

Useful first-run lines include:

- `fire resolve ... HOOKED`
- `PreRender UpdateRpHAnim bridge ... HOOKED`
- `render bridge ... HOOKED`
- `quaternion pose mirror active native slots 4/14/6/16 ... -> second slots 3/13/5/15 ...`
- `cloned opposite weapon ... mode=slot5/SLhand HAnim`
- `second native shot succeeded ...`

The quaternion-pose line also prints the runtime node IDs for all eight frame slots. For the supplied VC executable the expected relevant mappings are native `22/23/24/21` and opposite `32/33/34/31`.

## Build

Build Win32/x86 as a GTA Vice City Plugin-SDK ASI (`plugin_vc`) using the project's pinned 10/31/2025 Plugin-SDK state. Do not link this target as GTA SA.
