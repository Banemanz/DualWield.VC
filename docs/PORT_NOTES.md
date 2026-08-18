# DualWieldVC continuation notes - v12

v11 runtime proved all hooks/fire plumbing worked but appeared visually unchanged.

Root cause found in VC IDB: stock `CPed::Render` hardcodes the native weapon to `CPed+0x1C0`, which is `m_apFrames[6]` / HAnim 24. v11 defaulted `SecondHandRight=1`, selecting that same slot for the clone, muzzle and arm solver. Remove left/right configuration from future builds unless supporting a deliberately non-stock skeleton mapping.

v12 fixed mapping:
- native: frame 4 upper / 14 forearm / 6 hand / 16 clavicle -> HAnim 22/23/24/21
- second: frame 3 upper / 13 forearm / 5 hand / 15 clavicle -> HAnim 32/33/34/31

v12 arm strategy:
1. CPed::PreRender calls original CEntity::UpdateRpHAnim.
2. Snapshot native right weapon-arm segment directions from the final HAnim matrix array.
3. Reflect those directions across ped right-axis plane.
4. Rotate opposite arm interpolation quaternions (not final matrices) in parent-local axes with VC's `RtQuatRotate(0x65ABD0, rwCOMBINEPOSTCONCAT)`.
5. Set the same CPed+0x154 bit 0x20 used by native gun IK.
6. Re-run original UpdateRpHAnim after each joint correction.

This is based on VC's own CPedIK representation and SA gta-reversed's IKChain architecture (world correction axis -> inverse-parent/local axis -> postconcat local quaternion -> rebuild chain).

SA comparison also revealed its real twin-pistol second weapon render transform: copy BONE_L_HAND matrix, rotate X 180 preconcat, translate (0.04,-0.05,0) preconcat. v12 uses these as defaults.

If v12 still looks wrong, inspect `DualWieldVC_v12.log` first. The first `quaternion pose mirror active` line prints runtime node mappings; do not continue if they differ from 22/23/24/21 -> 32/33/34/31. If mappings are correct but direction is inverted, inspect the local-axis postconcat sign before changing hook phases.
