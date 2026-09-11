# Viewmodel Tweaks & Ironsights - developer notes

Reverse-engineering notes for `PreyDll.dll` (the EGS 2021-08-18 build Chairloader patches Steam to).
All offsets are RVAs into that DLL. Read alongside `Src/ModMain.cpp`.

## Contents

1. [Building from source](#building-from-source)
2. [The weapon / camera pipeline](#the-weapon--camera-pipeline) - offsets, aim lock (skeleton + render side), FOV
3. [Fire animation while locked](#fire-animation-while-locked)
4. [Feel layer](#feel-layer-sprint-pose-aim-sway-view-drag) - sprint pose, aim sway, view drag
5. [Wall pull-back and the near-wall pose](#wall-pull-back-and-the-near-wall-pose)
6. [Bullet spread](#bullet-spread-shotgun-and-pistol-share-carkweaponshotgun)
7. [Reticle and the in-world screen cursor](#reticle-and-the-in-world-screen-cursor)
8. [Interaction animation](#interaction-animation-support-hand-reach) - the rig, hooks, poses, the closed-loop rules,
   examination mode, what does not work, diagnostics
9. [Robustness](#robustness)
10. [Three ABI traps](#three-abi-traps)

## Where things are in `Src/ModMain.cpp`

One file, roughly in this order: hooks, RVAs and vtable slots (top); numeric sanity helpers; the offset hook
(`OnProceduralContextUpdated`, skeleton side of the aim lock, `PushAimLock`); the skeleton cache
(`UpdateSkeletonCache`); the render side (`OnCameraUpdated`); the interaction reach (`OnInteract`,
`StartReach`, `UpdateHover`, `ApplyExamineCVars`, `UpdateInteract`, `FireDeferredInteract`,
`PushInteractReach`, `PushHandPose`, `CursorWorldPoint`, `UpdateExamZoom`, `UpdateArmsVisibility`, poses
file); convergence, spread, blend states, feel, sanitizing, camera zoom; weapons lookup, nudge keys, input
(`OnInputEvent`), weapon FOV, reticle, trace, weapons file; `RegisterCVars`, init / shutdown; the per-frame
entry points (`UpdateBeforeSystem`, `MainUpdate`, `LateUpdate`); the ImGui tabs (`Draw*`). `ModMain.h` holds
the settings struct (`ViewmodelSettings`, one cvar each), the per-weapon struct (`WeaponSettings`) and the
runtime state structs (`InteractState`, `RenderLockState`, ...).

## Building from source

Standard Chairloader DLL-mod workflow (Visual Studio 2022 + CMake + vcpkg), see
<https://github.com/thelivingdiamond/Chairloader/wiki/DLL-Modding-%E2%80%90-Mod-Project-Setup>:

```
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
  -DCHAIRLOADER_COMMON_PATH=<Chairloader repo>/Common ^
  -DMOD_DLL_PATH=<Prey>/Mods/Vee.ViewmodelTweaks
cmake --build _build --config Release
```

`Src/ModMain.cpp` contains the whole mod; `CommonMod/` and `CMake/` are the unmodified
Chairloader mod SDK glue.

## The weapon / camera pipeline

Prey is built on CryEngine 3 and kept Crysis 3's *procedural weapon animation* system
(`CProceduralWeaponAnimation`, `CWeaponOffsetStack`, `CWeaponPoseOffset`, ...). Every frame:

1. `CProceduralWeaponAnimationContext::Update()` calls
   `CProceduralWeaponAnimation::Update(float dt)` (`PreyDll.dll+0x17D31E0` in the
   Chairloader-supported build). That function blends the weapon's authored pose offsets,
   look/strafe sway, recoil, bumps and zoom offsets into two view-space `QuatT`s:
   `m_rightOffset` (`this+0x2D8`, weapon hand) and `m_leftOffset` (`this+0x2F4`, support hand).
2. The context rotates those into the aim frame (`Quat::CreateRotationVDir(aimDir)`) and
   pushes them *additively* onto the right/left hand IK joints of the first-person arms
   through an `IAnimationOperatorQueue` pose modifier.

The mod post-hooks `CProceduralWeaponAnimation::Update` and multiplies the user's offset into
both `QuatT`s (translation in view space: X right, Y forward, Z up; rotation as an
`Ang3(pitch, roll, yaw)` quaternion). The stock `g_debugWeaponOffset 2` designer tool is
left untouched (the hook backs off while it is active).

Crouch detection reads `ArkPlayer::m_stance` (`STANCE_SNEAK`). Aiming is the mod's own key
binding, read through an `IInputEventListener` registered with `gEnv->pInput` (so it is ignored
while any cursor is on screen), with an optional camera zoom pushed into `ArkPlayerZoomManager`
via `SetDesiredHFOV(cl_hfov * factor, time, false, high)` / `ClearDesiredHFOV`.

Additive offsets inherit whatever the body animation does with camera pitch (aim-up/down poses) and
the look-sway, which makes real ironsights impossible. The lock works in two steps, both deterministic
(no prediction filters, no feedback loops):

1. **Skeleton side.** After the context has pushed its additive operators, the mod pushes `eOp_Override`
   position/orientation for the weapon-hand IK joint (`IAnimationOperatorQueue` vtable slots 8/9, joint
   index at `ctx+0x18`): `camera(model space) * aimPose * (weapon relative to IK joint)^-1`. This brings
   hands, weapon bone and all effects attached to it (muzzle flash, projectile origin) to the aim pose.
   The camera is not known yet at this point - Prey evaluates the first-person skeleton first and then
   builds the camera *from* it - so this step uses the previous frame's exact camera plus this frame's
   mouse delta (`ArkPlayerCamera::m_rotation` is updated in `PrePhysicsUpdate`, before the animation).
2. **Render side.** `ArkPlayerCamera::UpdateView` (`+0x145ED50`) is where the camera becomes final:
   `GetDesiredPositionAndRotation` (`+0x145C530`) reads the `#camera` attachment of the arms character
   (`IAttachmentManager::GetInterfaceByName` = vtable +0x38, `IAttachment::GetAttModelRelative` = +0x68),
   rotates it by the entity rotation, and combines the view angles with the lean roll
   (`m_leanAngle * m_leanAmount`, `m_leanAmount` being smoothed at 10/s in `PrePhysicsUpdate`). The
   position is returned *relative to the entity* because the arms use camera-space rendering
   (`ENTITY_SLOT_RENDER_NEAREST`; `CView::Update` adds the entity position). Nothing touches the
   skeleton between this call and rendering, so the mod post-hooks it and writes the weapon's final
   transform directly:
   * The weapon is not rendered from its entity. `CArkWeapon::AttachToHand` binds the weapon's character
     as a *skeleton attachment* (`CSKELAttachment`) of the arms and clears the weapon entity's own render
     flag; it is drawn at `armsWorldMatrix * CAttachmentBONE::m_AttModelRelative`. That QuatT lives at
     `attachment+0x130` and `GetAttModelRelative()` returns a reference to it, so the mod sets it to
     `camera(model) * aimPose` (`CArkWeapon::m_pAttachment` is at `+0x2B0`).
   * The hand joints must stay on the grip, so the final absolute pose of the weapon-hand subtree (and
     the support hand when it is within 35 cm of the weapon) is moved by the same rigid delta through
     the reference `ISkeletonPose::GetAbsJointByID` (slot 24) returns; the skinning reads that buffer at
     render time. Hierarchy comes from `IDefaultSkeleton` (`GetJointCount` slot 1, `GetJointParentIDByID`
     slot 2, `GetJointNameByID` slot 6, `GetJointIDByName` slot 7; the weapon hangs off `r_handProp_jnt`,
     child of the hand joint).
   The delta is normally a few millimetres (the skeleton-side push already did the work); it is what
   used to leak through as bob/lean jitter.

Motion while locked is opt-in: the game's procedural recoil/bump and look/strafe offsets (captured from
`CRecoilOffset/CBumpOffset/CLookOffset/CStrafeOffset::Compute`) can be kept at any fraction, and a
"head-bob coupling" subtracts a fraction of the camera's own fast motion (camera position minus a
low-pass of it) so the weapon lags the head like a real viewmodel. All of it is computed from this
frame's exact data, so it cannot jitter.

The hip pose is captured the same way so the transition blends smoothly. `aimPose` is therefore
"weapon (attachment) relative to camera": x = 0 / no rotation means centered, and it is stored per weapon
class (`IEntityClass::GetName()` of the equipped `CArkWeapon`).

Some mannequin fragments (jump/land on certain weapons) contain no weapon procedural clip, so
`CProceduralWeaponAnimationContext::Update` (`PreyDll.dll+0x17D4D50`) early-outs on
`m_instanceCount < 1` and nothing is applied — the weapon visibly snapped back. The mod hooks that
function and, when enabled, forces the count to 1 for the duration of the call so the (empty) pose
stacks and the user offset still get applied.

For the FOV: the weapon is rendered in the "nearest" pass with its own FOV stored in
`r_DrawNearFoV` (`EFQ_Set/GetDrawNearFov` read/write the same float).
`ArkPlayerZoomManager::Reset()` and `SetNearFOVUnlocked()` hard-code it to **55°**, and
`ArkPlayerZoomManager::Update()` scales it proportionally to the horizontal FOV while zooming
(which is why plain `r_DrawNearFoV = x` in `game.cfg` never sticks). The mod re-applies
`vm_fov * currentHFOV / baseHFOV` every frame after the game's update, and respects the
game's "near FOV locked" state (used when the weapon must share the world FOV).


## Fire animation while locked

* The skeleton-side push drives the right IK joint through the game's own `IAnimationOperatorQueue`. With
  `eOp_Override` the fire *animation* (the pistol's kick is animated, the shotgun's is mostly the procedural
  `CWeaponRecoilOffset`) is wiped out. The push is therefore `eOp_Additive` (3, what the game uses for its
  offsets: position added, orientation pre-multiplied in model space): `final = animated (+) add`. Knowing
  `add` of the previous frame gives the animated hand of the previous frame; its deviation from a slow
  reference (frozen while a shot plays) is the kick, fed into the aim pose (weapon-local, per-weapon
  `aim_kick_scale`, gated to the shot window). The one frame of animation velocity the additive lets through
  is corrected exactly by the render-side placement.
* `CArkWeapon::FireWeapon` (`+0x16659B0`) is the shot event for every weapon.

## Feel layer (sprint pose, aim sway, view drag)

* Pure per-frame state (`UpdateFeel`, from `MainUpdate`) with two outputs: additive view-space offsets for the
  hip path (`ApplyOffset`, faded out by the aim blend) and a weapon-local post-multiplied transform for the aim
  path (`ComputeAimLocal`, applied as `extra * aimPose * local * kick` on both the skeleton and the render side).
* Sprint: `ArkPlayerMovementFSM::IsSprinting()` (`+0x1570580`); speed from `pe_status_living::vel`.
* View drag: turn rate from `ArkPlayerCamera::m_rotation` (yaw = Ang3.z, pitch = Ang3.x), a damped spring
  (sub-stepped at 8 ms) towards the rate; the offset is proportional to the spring state, clamped.
* Aim sway rotates the weapon about its own pivot (post-multiplied), so the sights leave the crosshair while the
  shot still follows the camera - which is what makes it matter with the reticle hidden.

## Wall pull-back and the near-wall pose

* One `RayWorldIntersection` along the view ray per frame (`rwi_stop_at_pierceable`, skipping the player) gives
  the hit distance used by convergence, the pull-back and the aim block. The pull-back target is
  `SmoothStep01((start - dist) / (start - full)) * weapon.wallPush`, exponentially smoothed.
* The near-wall pose reuses that state: `blend = SmoothStep01((push / weapon.wallPush - biasStart) /
  (biasFull - biasStart)) * weapon.wallPoseAmount`, so it is 0 while the weapon is free and 1 once the weapon
  has used its full pull-back (the weapon "length"). The pose is added to the hip pose (faded by the aim blend
  and the reload fade like everything else) and the hip convergence is scaled by `1 - blend *
  wallPoseConvergeFade`, since the pose deliberately points the barrel away from the impact point.
* The blend is also multiplied by a camera-pitch factor `1 - strength * SmoothStep01((|pitch| - a0) / (a1 - a0))`
  (pitch from the view ray, `asin(dir.z)`): looking steeply up or down the wall is no longer where the muzzle
  would go, so the weapon returns to the plain pull-back.

## Bullet spread (shotgun and pistol share `CArkWeaponShotgun`)

Two independent terms, and for the shotgun only the first one is normally alive:

* **The pellet cone.** `SpawnPellets` reads `ShotgunSpreadConeDegrees` through `CArkWeapon::GetStatFloat`,
  lays `nNumberOfPelletRows` x `nNumberOfPelletColumns` pellets on a grid spanning that cone at the aim point,
  and aims each pellet from the muzzle through its grid point. Shotgun archetype: 8 deg, 3x3 pellets.
* **Dispersion**, i.e. where the cone is *centred*. `ComputeAimPoint` only randomises the aim point when
  `cone == 0 && !combatFocus` (that is the pistol) **or when the "accurate shot" roll fails**. The shotgun
  archetype has `fAccurateShotChance = 1`, so its roll never fails and its whole `<Dispersion>` block (all
  values 15, all rates 0) is dead data. `ArkRegularOutcome m_accuracyOutcome` (weapon +0x4D0) holds that roll;
  `UpdateAccuracy` (0x167DDA0) fills it from `ShotgunBaseAccuracy` + player `BaseAccuracy` +
  `CombatFocusAccuracyBonus` and is called from `OnStatChange` for exactly those three stats. **If that
  outcome ever stops saying 100 %, the shotgun starts using its 15 deg dispersion and the pattern roughly
  triples** (tan15 + tan8 vs tan8) - the failure mode to look for when shotgun spread is much too wide.
* `GetDispersionMinimum` / `GetDispersionMaximum` (0x167A6B0 / 0x167A5B0) **cache their result** in
  `m_minDispersion` / `m_maxDispersion` (+0x4E4 / +0x4E8) before returning, and `UpdateDispersion` (0x167DFF0,
  virtual slot +0x188, called every frame from the player movement update) compares that cache with a fresh
  call to detect a stance/stat change, then remaps `m_weaponDispersion` (+0x4E0) from the old range into the
  new one. A hook that scales only the return value makes that test true forever, so the remap runs every
  frame and multiplies the current dispersion by the factor each time. Write the scaled value back into the
  cache.
* No shotgun weapon mod touches spread: Power/Damage 1-5 are pure damage signal scales (x1.1 ... x1.11),
  Recoil changes `recoilPitch/Yaw`, and the others clip size / reload speed. `shotgunSpreadConeDegrees` is in
  `Ark/WeaponMods/Config.xml`'s moddable list but no `ArkWeaponModifier` in the game data references it, so an
  upgraded shotgun has exactly the vanilla pattern. Stat modifiers are **additive** on the base value:
  `ArkStats::AddModifier` recomputes `final = base + sum(modifiers)` into the entry (+0x1C) and
  `GetStatFloat` returns that.
* `CArkWeapon::GetStatFloat` (0x1667570) is a 12-byte thunk (`add rcx, 0x1A8; jmp ArkStats::GetStatFloat`);
  `GetStatInt` (0x16675C0) and `GetStatFloatPlayer` (0x1667580, uses the player's stats at ArkPlayer+0x7C0)
  reach the same implementation *without* going through it, so hooking the thunk only sees float reads of the
  weapon's own stats.
* Stat modifiers are appended to a per-stat list and `final = base + sum(list)`; the running counter of
  modifiers ever applied to a weapon is `ArkStats::m_nextModifierId` at **weapon+0x1AC** (the stats object is
  at +0x1A8: `{uint ownerId; uint nextModifierId; map}`). Re-applying a weapon mod's modifiers therefore
  *stacks* them - useful sanity check when a modded stat looks far too large.
* `vm_spread_debug 1` logs one line per shot with all of the above, including the angle between the camera
  axis and the aim point (~0 = the shot was straight, so the pattern is the cone alone).

## Reticle and the in-world screen cursor

* `hud_reticleSetting` (SCVars+0x930; 0 off, 1 default, 2 dot) is exactly what the options menu writes
  (`gameOptions.xml`, Action="hud_reticleSetting"). Its only readers are in `CArkUIHUD` (4 sites); at 0 the
  HUD is sent `reticleDisplay("none")` *and* `interactIconDisplay("none")`.
* On in-world screens the cursor is the HUD reticle: `ArkExaminationMode::UpdateReticlePos` (`+0x157EBA0`)
  moves `m_reticlePos` (+0x60) and sends `reticlePosition` to the HUD, so a hidden reticle means no cursor.
  The mod therefore un-hides the reticle while `ArkPlayer::m_examinationMode.m_examinationState != inactive`
  with `m_examinationType == worldUI`, and only ever writes the cvar when its own target value changes.
  Note that with a mouse `m_reticlePos` stays at the centre and the *camera* turns instead (see the
  interaction section): the reticle is a fixed centre marker there, not a moving cursor.

## Interaction animation (support-hand reach)

Written as a reference rather than a diary: the facts about the rig first, then the rules that fell out of
getting it stable (each one cost a release), then what does *not* work, then how to read the diagnostics.
Version notes at the end.

### The rig

* The first-person arms use CryEngine's *animation-driven IK*. `CProceduralWeaponAnimationContext::Initialize`
  (`0x17D5B60`) resolves three joints by name: `r_hand_spine_target` (`ctx+0x18`, right IK target),
  `l_hand_spine_target` (`ctx+0x1C`, left IK target) and `r_hand_spine_blend` (`ctx+0x20`, the right arm's
  IK weight). Every `Update` pushes `PushPosition(weightJoint, eOp_OverrideRelative(1), Vec3(1,0,0))` - the
  right arm's IK weight forced to 1 - then the rotated `m_rightOffset` / `m_leftOffset` additively onto the
  two target joints; the limb IK (`LftArm01` / `RgtArm01`, `AnimationPoseModifier_LimbIk`) brings each hand
  to its target. An additive push on the *left* target joint alone therefore moves the support hand off the
  weapon with the arm solved by the rig. The left weight joint is `l_hand_spine_blend` (looked up by name,
  forced to 1 during a reach).
* The context keeps running with **no weapon out** (the hidden arms are still animated), and it keeps running
  on screens - *once it exists*. It is a mannequin procedural context ("ProceduralWeaponAnimationContext",
  `TProceduralContextualClip::GetContextName` at `0x17D36D0`), created by the first weapon procedural clip that
  runs, i.e. the first time a weapon is equipped: on a fresh game, before the wrench, there is no context and
  no `Update` to hook, and nothing carried our pushes (3.11.1). **No weapon animation context yet**: 3.11.2
  drives the hand with our own queue in that window (`PushWithOwnQueue`, from `UpdateBeforeSystem`, only while
  `ctxUpdatesLastFrame == 0`): `CryCreateClassInstance("AnimationPoseModifier_OperatorQueue", &sp)`
  (`0x2C3530`, `std::shared_ptr` {ptr, ctrl}), the pose-modifier interface via `QueryInterface` (slot 2, IID
  `7f44425e-7547-fe22-49f4-9ad34e27b6ba`), then every frame what the context does at `0x17D4E86`-`0x17D4F9F`:
  `ISkeletonAnim::PushPoseModifier(layer 6, shared_ptr&, "ProceduralWeapon")` (slot 36), the queue's
  `Clear()` (slot 14), then the pushes; the left IK joint by name (`l_hand_spine_target`). Also seen there:
  `CProceduralWeaponAnimationContext::Initialize` is really `0x17D5B50` (`m_instanceCount++`, then the
  once-only part at `0x17D5B60`: store the *IScope* at `+0x38`, `scope->GetCharInst()` (slot 3), create the
  queue, resolve the three joints), and `Update` calls `m_pScope->GetCharInst()` before anything else - a
  context cannot be initialised without a real scope, which is why the own queue and not a hand-made
  Initialize.
* `IAnimationOperatorQueue` vtable: `PushPosition` slot 8 (`+0x40`), `PushOrientation` slot 9 (`+0x48`);
  ops 0 = Override, 1 = OverrideRelative, 3 = Additive. Positions are model space.
* **A position pushed on a joint travels down to its children.** Pushing the same shift on the root moves the
  whole body once; pushing it on every joint moves the hand once per ancestor (the 3.9.3 "every joint
  flails" mode). Move a body with the root joint (index 0) only.
* **An additive push is applied exactly.** `final = animated + add` for the pushed joint, and the shift from
  the root arrives unchanged. This is what makes the bookkeeping below deterministic.
* `ISkeletonPose::GetAbsJointByID` (slot 24, `+0xC0`) returns a *reference into the final-pose buffer of the
  previous frame* (writable; the skinning reads it at render time - the aim lock's render-side edits live
  there). Anything read from it is one frame old and includes every modifier that ran, ours included.
* `IDefaultSkeleton`: `GetJointCount` slot 1, `GetJointParentIDByID` 2, `GetJointNameByID` 6, `GetJointIDByName`
  7, `GetDefaultAbsJointByID` 8, `GetDefaultRelJointByID` 9. The last two are not in the SDK header;
  `UpdateSkeletonCache` proves them on the live skeleton (`abs[j] == abs[parent] * rel[j]` for all 101 joints,
  reads under SEH via `SafeReadJoint`) before trusting them ("bind pose accessors verified" in the log). The
  left hand subtree is 21 joints.
* `ISkeletonAnim` = `ICharacterInstance` slot 5 (`+0x28`); `PushPoseModifier` slot 36 (`+0x120`), the game uses
  layer 6. The arms' camera bone: `ArkPlayer::GetBoneTransform(BONE_CAMERA)`, model space.
* The IK weight is the weight joint's *relative translation x* (that is what the game's `eOp_OverrideRelative
  (1,0,0)` sets). Two-handed weapons animate the left weight at 1; one-handed ones (wrench, grenades) at 0,
  with the support hand animated off screen. Forcing the weight to 1 there snaps the hand to the IK target in
  one frame (the 3.10.0 "hover hand pops with the wrench"). The mod captures the animated weight on the first
  frame it pushes (`animIkWeight`, re-captured on weapon change) and pushes `animW + (1 - animW) * blend`
  (`vm_interact_ik_weight_ramp`), so the hand comes in along with the blend.
* The limb IK runs *after* the queue and recomputes the hand from the arm: an absolute `eOp_Override`
  orientation on the hand joint itself is discarded. The wrist is steered through the IK target joint's
  orientation (`eOp_Override` on `l_hand_spine_target`) and/or a parent-relative override on the hand
  (`!forearmAbs_prev.q * desired`) - both routes exist as `vm_interact_wrist_mode`.

### Hooks

* Trigger: pre-hook of `ArkPlayerInteraction::Interact(EArkInteractionMode)` (`0x1566820`). It is
  self-contained - target from `m_usableEntityId` (`+0x46C`), type from `m_interactionInfo[mode]`
  (`+0x128 + mode*0x18`, `EArkInteractionType` at +0, hold duration at +0x14), runs the entity's Lua
  `OnUsed`/`OnHoldUsed`/... then `PerformInteraction(type, mode, pEntity, delay)`. So the call can be
  stored and made later: the hook returns `true` and the stored `(this, mode)` is invoked from
  `UpdateBeforeSystem` after `vm_interact_fire_delay` seconds (re-entrancy flag lets it through), after
  re-validating the target. `PerformInteraction`'s own `delay` only feeds the carry type.
  Modes: 0 use, 1 holdUse, 2 loot, 3 special, 4 remoteManipulation. Types: 1 scriptDefined, 3 codeDefined,
  4 pickup, 5 consume, 6 carry, 7 hack, 8 repair, 9 fortify, 10 examine, 11 equip, 12 hoover.
* Carry is never deferred through `Interact`; it is handled at `ArkPlayerInteraction::PerformInteraction`
  (`0x1566B10`, pre-hook): `PerformInteraction(carry, mode, entity, delay)` with `delay > 0` arms
  `m_carryDelay` (`+0x460`, plus the entity's `audioTrigger_HoldCarryStart`) and `ArkPlayerInteraction::Update`
  (`+0x161A`) calls `StartCarrying(pCurrentTarget, false, false)` when it runs out; with `delay <= 0` it picks
  the object up on the spot. Raising the delay to the grab's reach time puts the pickup at the apex with the
  game's own bookkeeping. `pCurrentTarget` is the target selector's *current* entity, null whenever the
  crosshair has left the object (menu, mimic form, dead), and `StartCarrying` (`0x122FC50`) dereferences it -
  a vanilla crash (`mov rax,[rsi]` at `+0x122FD2D`) that any delay makes real, hence the null-guard hook.
  `OnActionUse` (`0x1563F60`): press (activation 1) calls `Interact(use)` at once only when the target has no
  hold-use interaction; otherwise the short press fires `Interact(use)` on release and the hold (activation 4)
  fires `Interact(holdUse)`. A carry that needs Leverage only reaches `PerformInteraction(carry)` from the hold,
  which is why the animation hangs off that call and not off `Interact`.
* "Something usable in front of us" for the hover hand is simply `m_usableEntityId != 0` plus the four
  modes' types - the same data the game shows its prompt from. No extra ray until the point is needed.
* Auto zoom-in on screens: `ArkInteractiveScreen::OnInteraction` (`0x139B9F0`) asks
  `ArkWorldUIManager::ShouldAutoExamineType(type)` (`0x13AF310`), a table of the vanilla cvars
  `ui_examine_{fabricator,keycard,keypad,operatordispenser,securitystation,workstation}` (defaults 1,0,1,0,1,1;
  kiosks never). When it says no, the screen takes the "use" press itself at the crosshair (`0x139D4E0`);
  when yes, `ArkWorldUIOwner::OnInteraction` (`0x13B1CC0`) calls `ArkExaminationMode::SetExamining(true)`.
  `vm_interact_nozoom_*` writes those cvars whenever the live value differs (`ApplyExamineCVars`; the game
  re-registers them with their defaults on level load).
* Which animation an interaction gets is a rule list (`InteractRule`, `ResolveStyle`, `<Rules>` in the poses
  file; first match on type / mode / entity class substring / prompt text substring -> press, grab or none,
  and whether the hovering hand comes up). Prompt texts are localisation tokens (`@use_npc`, `@i_defaultBook`),
  not English words; entity class names are in `ArkEntityClassLibrary.h` (`ArkHuman`, `ArkContainer`,
  `ArkCargoContainer`, `ArkHarvestable`, `ArkBook`, `ArkOperator*`, `ArkWeapon*` ...). The last interaction's
  class and prompt text are shown in the tab, with a button that turns them into a rule.
* Full list of hooks the feature uses: `ArkPlayerInteraction::Interact` (pre, deferral), `::PerformInteraction`
  (pre, carry), `ArkPlayerCarry::StartCarrying` (null guard + "carry started"), `ArkWeaponUtils::
  DoWeaponImpulse` (only while our own melee hit is on the stack), the `CProceduralWeaponAnimationContext::
  Update` hook shared with the aim lock (pushes), `ArkPlayerCamera::UpdateView` (camera kick, read-back), the
  raw input listener (screen clicks, melee key).

### Target point and view space

* Everything is computed in view space (camera at the origin: X right, Y forward, Z up) with the exact
  model-space camera the aim lock already has (`camAbs`; on screens `PredictCamera` returns the real view
  camera, `R.camModel` from `gEnv->pSystem->GetViewCamera()`). World -> model = entity-inverse, model ->
  view = `camAbs^-1`.
* The point: the crosshair ray hit (`RayWorldIntersection`, 4 m, `rwi_stop_at_pierceable`, skipping the
  player) if it lies within 25 cm of the entity's world bounds, else the bounds centre (`FindInteractPoint`).
  On screens the same centre ray (3 m) - see "Examination mode" for why the centre and not a cursor.
* Reach math: `base = handView + (rest - handView) * restBlend` (the animated IK target, or the resting spot);
  `desired = base + (target - base) * curve * amount + arc + poseOffset * w + weaponCorr * curve + examCorr *
  curve * examBlend`; then the envelope on the *wrist* (never behind the camera; beyond the forward limit the
  whole vector is scaled so the hand still covers the target on screen; side / up / down clamps). Applying the
  envelope to the *target* instead (3.8.x) left the hand hovering mid-air at the same depth every click.

### Hand poses

* Fingers are plain joints under the hand joint (`RenderLockState::leftSubtree`, parents first);
  `PushOrientation(joint, eOp_OverrideRelative, bindRel * userAdjust)` makes a pose *absolute* - the same
  whatever weapon is held, the weapon's grip only supplies the start of the blend (subtree pose captured on
  the first frame we push; from then on the final pose is ours, so it cannot be re-captured).
* A pose also carries a wrist offset (view space) added to the target so the fingertip, not the wrist, lands
  on the object; or `absolute_pos` - the hand position itself, object ignored (not useful for pointing).
* Three pose slots in `Vee.ViewmodelTweaks.poses.xml` (`Style name="press|grab|rest"`): the reach's pose and
  the resting hand's. While resting, the joint target is `nlerp(restPose, reachPose, curve)` so a press
  blends out of the rest pose and back without a pop. Posing mode freezes the reach fully "in".
* Per-weapon correction (`WeaponSettings::interact`, `interact_*` in the weapons file, `_none` for unarmed):
  the grip changes where the hand starts and with it how the fingertip sits relative to the wrist joint at the
  end. Screens have their own on top (`examCorr`).

### The closed loop and the additive chain - rules

1. **Reconstruct, do not guess.** The animated (pre-modifier) IK target of last frame is `final - lastAdd`,
   where `lastAdd` is *everything* we pushed that reached the joint: the reach add *and* the body shift
   arriving from the root. Frames without a reach still record the shift (`noReachThisFrame`). The old
   "applied / skipped" two-hypothesis test (still used by the aim lock, where it is fine) could pick wrong
   and then never recover: a wrong baseline gives a wrong push whose read-back fits the wrong hypothesis
   again - the hand snapping between two states forever ("flailing", 3.9.3).
2. **Detect, then reset.** A skeleton that did not update returns an identical read-back: keep the previous
   reconstruction. A reconstructed animation jumping over 30 cm in one frame means the model is wrong
   (animation change, our push not taken): reset - push only what is known exactly (the shift and last
   frame's reach repeated, so nothing pops) and start clean next frame. Counter "chain resets" in the tab.
3. **Measure the reach from where the joint will be after the shift** (`handView = cam^-1 * (anim + shift)`).
   Measuring before it made the push overshoot by the whole shift (0.4-0.8 m on screens); the closed loop
   spent its entire 35 cm budget taking that back, so the hand was "on target" only where the shift was
   under 35 cm and "IK NOT REACHING" beyond (3.9.3).
4. **Compare in the frame the push was made in.** The hand read back is last frame's; compare it with last
   frame's `desired` in *last frame's* camera (`camReachPrev`), never the current one - otherwise every
   camera movement is integrated as a hand error and the hand chases the turn.
5. **What the skeleton side reads back must be what the skeleton side produced.** Render-side edits of the
   abs buffer (the 3.8.1 body shift) are not consistently visible in the read-back; the loop chased a hand it
   saw somewhere else. Body moves go through the queue (root joint additive).
6. The loop itself: `corr += (desiredPrev - handActual) * gain` (gain `vm_interact_correct`, clamped to
   35 cm, reset when the reach and rest end or the chain resets). It only has the rig's effector offset to
   absorb now; `corr SATURATED` in the overlay means something structural is wrong, not that the gain is off.
7. Gate every screen-only quantity on a *blend* (`examBlend`: in 0.1 s, out over `vm_interact_exam_leave_time`),
   not on the raw flag - body shift, screen corrections, forward limit, arm extension - or leaving a screen
   snaps the arm to the weapon (3.10.0).

### Examination mode (in-world screens and keypads)

* State: `ArkPlayer+0x9B0` = `ArkExaminationMode`; `m_examinationState` (`+0x98`) 1 = active,
  `m_examinationType` 1 = worldUI, `m_localRotation` `+0x44`, `m_reticlePos` `+0x60`, `m_targetEntity` `+0x9C`.
* `SetExamining_Internal` (`0x157E1E0`): on enter unequips the weapon (`ArkPlayerWeaponComponent` call) and
  clears the render flag (bit 1) of the player entity's slots 0 (arms) and 5; on exit sets `flags | 1` back.
  **Drawing a weapon does not set the flag again** - whoever clears it after exit hides the arms for good
  (the 3.9.4 "viewmodels stop rendering after a screen" bug).
* `ArkExaminationMode::UpdateView` (`0x157EC80`) runs *inside* `ArkPlayerCamera::UpdateView` (`+0x9C5`, which
  returns early when it handled the view): the camera lerps to the "optimal view" in front of the screen -
  0.39-0.84 m away from the head bone; keypads ~22-26 cm from the surface, monitors 0.6-0.8 m. With a mouse,
  `ArkPlayerInput::GetRotation` accumulates into `m_localRotation` (clamped to a FOV-based limit times
  `m_maxCameraRotation`): **the camera turns, the click is the centre of the view**. `m_reticlePos` is the
  gamepad cursor and sits at (0.5, 0.5) with a mouse; the hardware cursor is in desktop coordinates. Aiming
  the hand at either was wrong (3.8.1-3.8.3); the centre ray is right (`vm_interact_exam_cursor 0`).
* The arms stay with the body at the head, so a hand reaching in front of the *view* is beyond the arm and
  behind the camera. The body is brought along by a root-joint additive `realCam.t - camBone.t + bodyOffset`
  (`vm_interact_exam_shift_mode 1`, rules 1-5 above). Even then the shoulder sits ~0.45 m behind and 0.3 m
  below the view: `vm_interact_exam_auto_body` slides the body forward until shoulder-to-wrist equals the arm
  length (`vm_interact_exam_arm_length`), needed for monitors, idle on keypads. Below
  `vm_interact_exam_min_dist` there is no reach (a hand touching a keypad 22 cm away would sit on the lens;
  a finger filling the screen for the press is acceptable, the point is that it hits the right button).
* The examined screen renders in the *nearest* pass together with the arms (`m_nearFOVLockedCount != 0`
  there, so `EnforceWeaponFov` leaves the near FOV alone): changing the world FOV rescales the surroundings
  but not the screen; hand and screen share one projection, and the centre-ray hit on the screen's physics
  mesh is the right target under any FOV. The zoom is a zoom-manager entry `SetDesiredHFOV(worldUI+0x98, 0,
  false, priority normal(3))` pushed once the camera lerp is done; an entry at priority high overrides it
  (`vm_interact_exam_fov_mode`).
* Clicks on screens do not go through `Interact`; they are taken from the raw input listener
  (`vm_interact_exam_key*`, bindable) while `active && worldUI`.

### Quick melee (3.11.0)

* `ArkWrenchComponent::OnHit(dir, CArkWeapon&, damageScale, bCharged)` (`0x13926D0`) is the whole wrench hit:
  `GetHits` (`0x13905C0`) casts from the *player's* view with the wrench's range stats, then per hit the damage
  signal package (`m_packageId` / critical / charged variants, damage x `damageScale`), NPC reaction, physics
  impulse, and the player's fatigue (`damageScale * m_fatigueThisHit`). It reads the weapon only for stats and
  the owner entity, so it works on the wrench sitting in the inventory: `ArkPlayerWeaponComponent::FindWeapon
  (ArkWrenchComponent::GetWrenchArchetypeId())` (or the double wrench) -> `CArkWeapon::GetWeaponFromEntityId`
  (`0x1667720`) -> `static_cast<ArkWeaponWrench*>` -> `m_wrenchComponent` at `+0x4C8`. `dir` is the swipe angle
  in degrees (0 = straight); `m_bDodge` set makes the call a no-op once. `ArkWeaponWrench::OnHit` (`0x1685090`)
  is the same plus the charge scale and the hit animations - not used.
* No wrench in the inventory: the punch plays and lands nothing (counted in the tab).
* Sound: `ArkAudioTrigger::Load(name)` / `Execute(pEntity)` with a wwise event name; the wrench's own swing
  event is not in the DLL's strings (it lives in the banks / entity properties), `Play_Player_Throw` (the
  throw whoosh) is the default, `vm_melee_sound_name` takes another.
* Camera kick: `params.rotation *= kick` in the `ArkPlayerCamera::UpdateView` post-hook before the aim lock reads
  the camera, so the weapon follows; the arms are placed from the camera bone by the game and do not, which
  reads as the head moving against the hands.
* The punch is reach style 2 with a *windup* keyframe (`ReachStyle::windupTime`, `windX/Y/Z`): a phase before
  the reach where `wind` runs 0 -> 1 (reach easing) and the hand goes to `start + windOffset`; during the reach
  `wind = 1 - curve`. `ApexTime() = windup + reach` is what the deferred fire, the carry delay and the melee hit
  use. The hit lands on the transition into the hold phase (`meleePending`).

### Arms visibility

* States in which the game keeps the arms out of sight: unarmed and examining. While a reach or the resting
  hand is active there the mod sets `CEntity::SetSlotFlags(0, flags | 1)` and remembers what it found
  (`armsForcedWhile` = which state). It writes the remembered flags back **only if that same state is still
  on**; after a screen the game has already restored the flag and must be left alone (see above).
* Hiding the right arm by pushing all its joints behind the camera leaves a stump (skin weighted to joints
  above the upper arm): off by default.

### What does not work (do not retry without a new idea)

* `IRenderAuxGeom::RenderText` - goes through Chairloader's `CAuxGeomCB` into `gRenDev->FlushTextMessages`,
  which draws nothing in this build (in either of our mods). Mods' `Draw()` only runs while the F1 GUI is
  shown, but the ImGui frame is live every frame (`NewFrame` in `ChairImGui::UpdateBeforeSystem`, `Render`
  at `RenderEnd`), so `ImGui::GetForegroundDrawList()` from `MainUpdate` is the way to draw overlays
  (`DrawInteractMarkers`).
* Own `AnimationPoseModifier_OperatorQueue` - works since 3.11.3, see "No weapon animation context yet" in the
  rig section. Both earlier attempts (3.8.2 "read at -1", 3.11.2 "Pure function call" on the second push) had
  the same cause: `ISkeletonAnim::PushPoseModifier` takes the `shared_ptr` **by value and consumes the
  reference** (the context increments the control block's use count for the temporary it passes and never
  releases it; MSVC x64 hands the copy to the callee), so passing our single reference let the skeleton
  destroy the queue at the end of the frame, and the next frame's calls went through a destroyed object's
  vtable. One `lock inc [ctrl+8]` per push fixes it; the rest (vtable-in-module checks, SEH around the push,
  self-disable on a failed check) stays as a guard.
* Render-side body shift (writing the abs buffer after the camera is final, 3.8.1-3.9.2): inconsistent with
  the read-back, see rule 5.
* Every-joint body shift: multiplies down the hierarchy, see "The rig".
* Arm extension (additive on the upper arm on screens): the limb IK re-solves from the shoulder, no gain.
* A separate world FOV on screens to "make room": the screen is in the nearest pass, it does not rescale.
* A hardware / reticle cursor as the click point on screens: see "Examination mode".

### Reading the diagnostics

The overlay (`vm_interact_debug`, ImGui): red = target, green = where the wrist is asked, cyan = where the
wrist joint really is (last frame), magenta = shoulder, plus a line of numbers. Log lines in `Game.log` with
the overlay on: "screen click ...", "reach at target ...", "quick melee hit ...", "quick melee impulse ..."
(raw impulse direction against the view forward), "screen ray hit ...". Readouts in the Interact tab:
pushes / chain resets / NaN recoveries (Advanced), carries called off, hovering state with the usable
thing's type and distance, the animated hand's view-space position and the IK weight ("Where it comes up
from"), whether the hand comes from the spot or the animated hand, whether the game's context or our own
pose modifier carried the pushes (Advanced state line), punches / hits / without a wrench.

Fresh-game checklist (a wrong answer here is where the last three releases' bugs lived): before the wrench,
the Advanced state line must say "-> our own pose modifier" and `Game.log` must have "created our own
AnimationPoseModifier_OperatorQueue" without a following "fallback disabled"; with the wrench out, "Where
it comes up from" must say "coming up from the spot"; with the pistol out it must say "from the animated
hand".

| Symptom | Meaning |
| --- | --- |
| residual ~0, `corr SATURATED` | a structural offset the loop is hiding (double-counted shift, wrong frame): fix the model, not the gain |
| `IK NOT REACHING`, residual constant | wrist asked beyond the arm: shoulder too far back (auto body / body offset), or the envelope limit is above what the arm can do |
| hand snaps between two places every frame | chain bookkeeping lost (rule 1/2); should now show as chain resets instead |
| hand drifts while turning the camera, settles after | loop measured in the wrong camera (rule 4) |
| whole arm pops once when a screen closes | a screen-only quantity gated on the raw flag (rule 7) |
| arms invisible after a screen, weapon too | render flag written back after the game restored it |
| `TOO CLOSE` on a keypad | target closer than `vm_interact_exam_min_dist`: intended |
| chain resets climbing while just looking around | animation jumps > 30 cm/frame on the IK target: lower the threshold's assumptions or check the weapon anim |
| hand pops into view over two frames with the wrench / grenades / no weapon | the blend started from the off-screen animated hand: "Support hand for this weapon" is not "off the weapon" (check the weapons file entry) |
| punch hand vanishes for a frame at the apex | a push was dropped by a sanity limit (`add.t` over 3 m) or the arms were not shown for the phase |
| nothing at all on a fresh game before the wrench | the own operator queue did not engage: look for "fallback disabled" in the log and the reason on the same line |
| "Pure function call" fatal error | a virtual called on a destroyed object - for the own queue, a lost reference (see "What does not work") |
| a tap on a physics object plays a grab | `vm_interact_carry_hold` is 0, or the game's carry timer was not read (offset 0x460 static_assert) |

### Version notes

* 3.6.0 first version (deferral, tween styles); 3.6.1 carry crash (null `StartCarrying`).
* 3.7.x absolute bind-pose finger/wrist poses, wrist via IK target, closed loop, per-weapon correction.
* 3.8.x no weapon + screens: arms shown by slot flags, centre-ray click, envelope on the wrist, rest hand,
  ImGui overlay, screen FOV override; own-queue crash disabled.
* 3.9.3 body shift moved into the queue; 3.9.4 root-only shift, shift-aware reach base, exact chain
  reconstruction with reset, loop measured in its own camera - first stable screens.
* 3.11.7: the hand blinked for a moment on the way into and out of a screen (and around weapon holster /
  draw): the fallback queue engaged one frame after the context stopped running (a frame with no push - the
  hand at the animation) and, the frame the context ran again, both pushed (applied twice - the hand past the
  target). Now our own pose modifier carries the interaction pushes *every* frame
  (`vm_interact_own_queue_always`, default on) and `OnProceduralContextUpdated` skips its copy when
  `ownQueueFrame == m_frameIndex`; the aim lock still goes through the context. Cost: the pushes are computed
  in `UpdateBeforeSystem`, i.e. with the previous frame's camera rotation (one frame of lag on the hand while
  turning; the loop is measured in `camReachPrev` so it does not drift). Off = the old way (fallback only when
  the context did not run last frame), still with the double-push guard.
* 3.11.6: arms forced on whenever the game clears the flag while we want them, not only on the first time -
  hovering hand out, screen, exit, screen again left the hand invisible (the second entry cleared the flag
  while `armsForced` was still true from the first).
* 3.11.5: defaults = the values tuned in play through 3.11.4 (grab and punch styles incl. windup, envelope,
  rest / start spots, start arm pose, melee cooldown / kick / lowering, punch pose, door rule, wrench shoulder).
  The way to refresh them: dump `vm_interact_*` / `vm_melee_*` from `Chairloader_CVars.xml`, the `interact_*`
  attributes from the weapons file and the poses file, and copy into `ViewmodelSettings`, the `ReachStyle`
  factories, `SeedDefaultPoses`, `SeedDefaultRules` and `s_builtInInteract`.
* 3.11.4: hand off the weapon - orientation too: the wrist blends from the spot's orientation
  (`vm_interact_start_hand_*`, per weapon `interact_start_hand_*`) instead of from the off-screen animated hand,
  and an extra forearm rotation while the hand is up (`vm_interact_start_forearm_*`, per weapon
  `interact_start_forearm_*`) on top of the per-weapon twist. `hiddenStartUsed` is decided before the pose push.
* 3.11.3: the own operator queue crashed on its second frame ("Pure function call"): the reference handed to
  `PushPoseModifier` is consumed, see "What does not work". One extra reference per push.
* 3.11.2: the hand before the first weapon (own operator queue, above). The 1.5 m sanity limit on the additive
  dropped the push for a frame at the apex of a punch from an off-screen hand (hand popped to the animation and
  back): 3 m now. Arms are shown during the windup too (`reachActive` includes `wind`). Hand off the weapon: a
  shoulder / elbow offset (global + per weapon) applied while the hand is up, so the IK does not solve the arm
  from wherever the one-handed animation left the shoulder. Weapon lowering eased (`vm_melee_lower_ease`).
* 3.11.1: the one-handed flag was not reaching `SupportHandOffWeapon()`: the user's weapons file has an entry
  for every weapon, and those entries read the new attribute as "auto", so the built-in answer never applied.
  A missing attribute now takes the built-in value on load, and "auto" consults the built-in table before the
  heuristic. Windup gained shoulder / elbow / forearm offsets (additive on the upper-arm and forearm joints,
  forearm rotation as for the per-weapon twist; `windup_keep` carries them through the strike). The quick
  melee's physics impulse pulled objects in: `ArkWeaponUtils::DoWeaponImpulse` (`0x1680A10`) is hooked while
  our OnHit call is on the stack (`meleeHitInProgress`) and the direction flipped (`vm_melee_impulse_flip`) /
  scaled, with the raw direction logged against the view forward when the overlay is on - the wrench passes
  the player *entity's* forward rotated by the swipe angle and DoWeaponImpulse negates it before
  DoHitImpulse; why the sign comes out wrong for a straight punch and right for the wrench's swipes is not
  understood, the log line is there to find out. The equipped weapon is lowered during the punch through
  the hip offset (`vm_melee_lower_*`, blend `meleeLowerBlend` in from the windup and out from the return).
* 3.11.0: quick melee (above). One-handed weapons did not take the hidden start because their left IK weight
  is animated at 1 (only the target is off screen): `SupportHandOffWeapon()` now uses a per-weapon flag
  (`interact_hand_off`, built in for the wrench / grenades / Nullwave = off, two-handed = on, else auto from the
  weight and the animated hand's position, shown in the tab). Windup keyframe on every style; target offset
  sliders to +-80 cm.
* 3.10.4: carrying re-done around what testing showed. `OnActionUse` sends `Interact(holdUse)` on the first
  hold event, i.e. right after the press, and for a heavy object the Lua then calls `PerformInteraction(carry,
  ..., delay = the hold-to-lift time)`: the game's own "hold" IS `m_carryDelay`, and `StopHoldToUseInteract`
  (`0x1567640`, on release) ends with `m_carryDelay.Invalidate()` (`m_timeRemaining = -1`), which is why a
  released key never picked the object up once the delay had been stretched. So now: `OnPerformCarry` only
  schedules - the grab starts after `max(delay, vm_interact_carry_hold)` and the carry timer is set to that
  plus the reach - and `UpdateCarry` watches `m_carryDelay.m_timeRemaining`: below zero before
  `StartCarrying` (our hook sets `carryStarted`) means the key was released -> the pending grab is dropped or
  the playing one goes straight into its return. A tap shows nothing; a heavy object's grab starts when the
  hold completes; the pickup is at the apex either way; no entity pointer is kept (the id is looked up when
  the grab starts). The one-handed "pop" was the *start* of the blend, not the envelope: the support hand is
  animated off screen when it is not on the weapon (left IK weight 0) and with no weapon, and any blend from
  there crosses into view in a couple of frames. `vm_interact_hidden_start`: the hand comes up from a fixed
  spot below the view (global `vm_interact_start_*`, per-weapon `interact_start_*`) with the IK weight on from
  the first frame; the additive is still computed against the true animated joint. Styles carry
  `envelope` (limits x this, with the reach) and `along` (target moved along the camera -> object line) for
  grabs down to the floor. The style's hand rotation is applied inside `PushHandPose` too (a pose owning the
  wrist skipped the additive route, so it "did nothing" whenever the resting hand was up). Third reach style
  `punch` (`vm_interact_punch_*`, pose slot "punch", test button) for a later quick-melee key. Forearm and
  wrist corrections of the shotgun / GooGun / ToyGun baked into the built-ins; seeded rules use the prompt
  tokens (`@use_npc`) rather than English words.
* 3.10.3: `ui_examine_*` are re-registered by the game on level load (their defaults come back), so
  `ApplyExamineCVars` now writes whenever the live value differs. The reach envelope scales the whole wrist
  vector by one factor (direction to the target kept; a per-axis clamp left the hand on the box's floor at the
  object's distance - "stops short" of things on the ground, default below-eye limit raised to 0.8 m) and takes
  effect in proportion to the blend (at the start of a hover the hand is where the animation has it, and for
  one-handed weapons that is outside the box: clamping it there was the remaining "pops in" of the wrench /
  grenades). Per-weapon forearm rotation (`interact_forearm_*`, additive in model space from last frame's
  forearm frame; the hand's absolute wrist push keeps the hand where it is). Interaction *rules*
  (`<Rules>` in the poses file, `InteractRule`, `ResolveStyle`): first match on type / mode / entity class
  substring / prompt text substring gives press / grab / none and whether the hovering hand comes up; seeded
  with talk (ArkHuman + "talk") = none, ArkHuman = grab, *Container* = grab, ArkHarvestable = grab, hold-use
  on ArkWeapon* (take ammo) = grab; "add a rule from the last interaction" in the tab. Carrying moved to a
  pre-hook of `ArkPlayerInteraction::PerformInteraction` (0x1566B10): the grab plays only when the carry
  really begins (a short press on something that needs a hold never gets there), and with
  `vm_interact_carry_apex` the `_delay` argument is raised to the grab's reach time - PerformInteraction then
  arms `m_carryDelay` itself and `Update` calls StartCarrying when it runs out (with our null guard), so no
  state of ours can go stale. Grab-style interactions fire at the apex (`vm_interact_grab_apex`). Defaults
  (timings, envelope, rest spots, drift, hover, no-zoom switches, the point / point_rest poses, per-weapon
  corrections) updated to the values tuned in play; `_none` has a built-in entry now.
* 3.10.2 cleanup, no behaviour change: removed the dead ends as options (every-joint / render-side shift,
  own operator queue, right-arm hide, arm extension, cursor source / flip-y / follow test - the click is the
  centre ray, full stop) and their code; the Interact tab regrouped (Reach / Screens and keypads / Resting
  hand / Hand pose / Test / Advanced). Persisted cvars of the removed options are simply ignored.
* 3.10.1 IK weight ramp for one-handed weapons, resting spot per mode (outside / on screens,
  `vm_interact_rest_exam_*`) plus a per-weapon offset (`interact_rest_*` in the weapons file), hover off while
  aiming unless `vm_interact_hover_aiming`.
* 3.10.0 arms-invisible-after-screen fix, `examBlend` fade, chain reset repeats last reach, rest pose slot +
  drift + hover outside screens (`vm_interact_hover_*`), gentle press style on screens
  (`vm_interact_press_exam_*`), `ui_examine_*` exposed as `vm_interact_nozoom_*`.

Open: a Chairloader `ShutdownGame` crash was seen once when quitting the game while a screen was up (not
reproduced, not investigated).

## Robustness

* Everything read from the game (camera, entity rotation, attachment, bones, procedural offsets) is checked
  for finiteness and near-unit quaternions at the boundary; everything written back (offset, operator push,
  attachment, hand joints) is checked again. Accumulated state (low-passes, springs, blends, the additive chain,
  the kick reference) is reset when a bad number shows up, and persisted settings are sanitized on load.
  `Ang3(Quat)` in CryMath uses an unclamped `asin`, so all Euler conversions go through a clamped copy.
* The aim lock's additive hand push is only subtracted from the final pose when the skeleton actually applied
  it (the animation update can be skipped while the hook still runs); otherwise the chain would grow without
  bound. The interaction reach uses the stricter scheme described in its section (exact reconstruction, an
  identical read-back = no update, a reset on an implausible jump) after the two-hypothesis test proved able
  to lock into the wrong answer there.
* Mod cvars are unregistered in `ShutdownSystem`: the console keeps raw pointers to the names and storage,
  which vanish with the DLL, and touches them again at engine shutdown (crash on exit otherwise).

## Three ABI traps

1. Member functions returning a struct larger than 8 bytes (`QuatT`, `std::vector`, `std::pair`) take the
   hidden return pointer AFTER `this`. Never hook or call them through an SDK declaration that returns the
   struct directly; declare `R* (T* _this, R* _ret, args...)`.
2. `PreyFunction` instances are rebased when the DLL initialises; a function-local `static` one is
   constructed too late and calls the raw RVA.
3. **The SDK's `physinterface.h` is stock CryEngine; Prey's physics structs are not.** `pe_status_living`
   is 152 bytes in Prey (an extra `Vec3` after `velGround`; `groundSurfaceIdx` at +0x64, not +0x58), the
   header's is 136. `GetStatus(&living)` on a header-sized local wrote 16 bytes past it - exactly onto the
   caller's saved xmm6 in `UpdateFeel`'s frame, which `MainUpdate` was using for `dt`. The corrupted step
   made every exponential filter downstream (convergence, wall pull-back) either freeze (tiny/negative dt ->
   k = 0) or snap (large dt -> k = 1), and which one depended on the frame layout of each build. No crash,
   no NaN, just wrong smoothing - found by tracing filter value vs. target per update. Rule: every struct the
   *game* fills in (`pe_status_*`, `ray_hit`, ...) is declared with Prey's layout when known and always with
   a generous pad after it (`PreyStatusLiving`, `PaddedRayHit` in ModMain.cpp). Symptom to remember: a
   smoothed value that tracks its target instantly or not at all while the code is provably right means
   the *step* is garbage, and a garbage step in a value that was fine a call earlier means a callee
   trashed a callee-saved register - look for a stack struct the callee writes into.

