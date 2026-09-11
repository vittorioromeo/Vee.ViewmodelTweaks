// Vee.ViewmodelTweaks - Prey (2017) Chairloader mod
//
// Lets the player offset the first-person weapon viewmodel (position + rotation) with global standing /
// crouch offsets plus per-weapon hip offsets, a proper ironsight "aim lock" on a dedicated key (per-weapon
// poses), and a weapon FOV override - all live, from an ImGui window (F1 -> "Viewmodel Tweaks") or the
// console (vm_* cvars).
//
// How it works (see README.md for the full reverse-engineering notes):
//
//  * Prey still ships Crysis 3's "procedural weapon animation" system. Every frame
//    CProceduralWeaponAnimationContext::Update() calls CProceduralWeaponAnimation::Update(),
//    which blends the designer-authored pose offsets, look/strafe sway, recoil, bumps, etc.
//    into two view-space QuatTs: m_rightOffset (weapon hand) and m_leftOffset (support hand).
//    The context then rotates those into the aim frame and pushes them *additively* onto the
//    hand IK joints of the first-person arms through an IAnimationOperatorQueue pose modifier.
//      - Hip / crouch offsets: we post-hook CProceduralWeaponAnimation::Update() and fold the
//        user's offset into the QuatT(s) before the context consumes them.
//      - Aim lock, skeleton side: additive offsets inherit everything the body animation does with
//        camera pitch and look-sway, which ruins ironsights. So while aiming we post-hook the *context*
//        update and push an eOp_Override for the weapon-hand IK joint: camera(model space) * userPose *
//        (weaponBone relative to IK joint)^-1, so hands, weapon bone and effects end up near the pose.
//        The camera is not known yet at that point (the skeleton is evaluated BEFORE the camera is
//        built from it), so this uses last frame's exact camera + this frame's mouse delta.
//      - Aim lock, render side: ArkPlayerCamera::UpdateView builds the camera from the evaluated
//        skeleton (camera attachment position, view angles, smoothed lean roll) - after that the
//        camera is final and nothing moves the skeleton any more before rendering. We post-hook it and
//        write the weapon's attachment transform (CAttachmentBONE::m_AttModelRelative, what
//        DrawAttachment multiplies with the character matrix) to exactly camera * aimPose, and move the
//        hand joints (final absolute pose, what the skinning reads) by the same rigid delta. No
//        prediction, no feedback: the weapon is placed where it has to be, every frame, from exact data.
//
//  * The weapon is rendered in a separate "nearest" pass with its own FOV (r_DrawNearFoV).
//    ArkPlayerZoomManager hard-codes it to 55 degrees on Reset()/SetNearFOVUnlocked() and
//    rescales it proportionally to the HFOV while zooming. We re-apply the user's FOV every frame
//    relative to cl_hfov, so zooming (including our own aim camera zoom) keeps working.

#include "ModMain.h"
#include <Prey/CryRenderer/IRenderer.h>
#include <Prey/CryRenderer/IRenderAuxGeom.h>
#include <Prey/CrySystem/IConsole.h>
#include <Prey/CryInput/IHardwareMouse.h>
#include <Prey/CrySystem/HardwareMouse.h>
#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerCamera.h>
#include <Prey/GameDll/ark/player/ArkPlayerZoomManager.h>
#include <Prey/GameDll/ark/player/ArkPlayerInput.h>
#include <Prey/GameDll/ark/player/ArkPlayerWeaponComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerHealthComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerInteraction.h>
#include <Prey/GameDll/ark/ArkInteractionInfo.h>
#include <Prey/CryEntitySystem/Entity.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/ArkEnums.h>
#include <Prey/GameDll/ark/weapons/arkweapon.h>
#include <Prey/GameDll/ark/weapons/arkweaponshotgun.h>
#include <Prey/GameDll/ark/weapons/ArkWeaponWrench.h>
#include <Prey/GameDll/ark/weapons/ArkWrenchComponent.h>
#include <Prey/GameDll/ark/weapons/ArkWeaponUtils.h>
#include <Prey/Ark/ArkAudioUtil.h>
#include <Prey/GameDll/GameCVars.h>
#include <Prey/GameDll/weaponlookoffset.h>
#include <Prey/GameDll/weaponstrafeoffset.h>
#include <Prey/GameDll/weaponrecoiloffset.h>
#include <Prey/GameDll/weaponbumpoffset.h>
#include <Prey/CryAction/IViewSystem.h>
#include <Prey/CryPhysics/physinterface.h>
#include <pugixml.hpp>

ModMain* gMod = nullptr;

//---------------------------------------------------------------------------------
// Reverse-engineered game internals (PreyDll.dll, Chairloader-supported build)
//---------------------------------------------------------------------------------
namespace PreyInternals
{
    // CProceduralWeaponAnimation (GameDll/ProceduralWeaponAnimation.h in Crysis 3 GameSDK).
    // Not part of the Chairloader SDK headers, so we only describe the members we touch.
    namespace CProceduralWeaponAnimation
    {
        constexpr size_t OFFSET_RIGHT_OFFSET = 0x2D8;       // QuatT m_rightOffset (view space)
        constexpr size_t OFFSET_LEFT_OFFSET = 0x2F4;        // QuatT m_leftOffset  (view space)
        constexpr size_t OFFSET_DEBUG_INPUT_ENABLED = 0x320;// bool  m_debugInputEnabled (g_debugWeaponOffset == 2)

        // void CProceduralWeaponAnimation::Update(float deltaTime)
        inline auto FUpdate = PreyFunction<void(void* _this, float deltaTime)>(0x17D31E0);
    }

    // CProceduralWeaponAnimationContext (mannequin procedural context owning the object above at +0x40).
    namespace CProceduralWeaponAnimationContext
    {
        constexpr size_t OFFSET_RIGHT_IK_JOINT = 0x018;     // int   m_params.rightBlendIkIdx (weapon hand IK target joint)
        constexpr size_t OFFSET_LEFT_IK_JOINT = 0x01C;      // int   m_params.leftBlendIkIdx
        constexpr size_t OFFSET_WEAPON_TARGET_JOINT = 0x020;// int   m_params.weaponTargetIdx
        constexpr size_t OFFSET_POSE_MODIFIER = 0x028;      // IAnimationOperatorQueue* m_pPoseModifier
        constexpr size_t OFFSET_SCOPE = 0x038;              // IScope* m_pScope
        constexpr size_t OFFSET_WEAPON_SWAY = 0x040;        // CProceduralWeaponAnimation m_weaponSway
        constexpr size_t OFFSET_AIM_DIRECTION = 0x368;      // Vec3  m_aimDirection (model space)
        constexpr size_t OFFSET_INSTANCE_COUNT = 0x374;     // int   m_instanceCount (active procedural clips)

        // void CProceduralWeaponAnimationContext::Update(float timePassed)
        // Early-outs when m_instanceCount < 1, i.e. whenever the current mannequin fragment has no
        // weapon procedural clip (some jump/land fragments) -> no offsets at all get applied.
        inline auto FUpdate = PreyFunction<void(void* _this, float timePassed)>(0x17D4D50);
    }

    // Virtual table slots (verified against the game code, they differ from public CRYENGINE).
    constexpr size_t VT_IScope_GetCharInst = 0x18 / 8;
    constexpr size_t VT_ICharacterInstance_GetISkeletonPose = 0x38 / 8;
    constexpr size_t VT_ICharacterInstance_GetIDefaultSkeleton = 0x58 / 8; // [ArkPlayerCamera::GetJointId]
    constexpr size_t VT_ISkeletonPose_GetAbsJointByID = 0xC0 / 8;    // const QuatT& (int jointId)   [ArkPlayer::GetBoneTransform]
    constexpr size_t VT_IAnimationOperatorQueue_PushPosition = 0x40 / 8;    // (int joint, int op, const Vec3&)
    constexpr size_t VT_IAnimationOperatorQueue_PushOrientation = 0x48 / 8; // (int joint, int op, const Quat&)
    constexpr int OP_OVERRIDE = 0;      // IAnimationOperatorQueue::eOp_Override (model space)
    constexpr int OP_ADDITIVE = 3;      // eOp_Additive (what the game uses)

    // IDefaultSkeleton [ArkPlayerCamera::GetJointId, CAttachmentBONE::ProjectAttachment]
    constexpr size_t VT_IDefaultSkeleton_GetJointCount = 0x08 / 8;
    constexpr size_t VT_IDefaultSkeleton_GetJointParentIDByID = 0x10 / 8;
    constexpr size_t VT_IDefaultSkeleton_GetJointNameByID = 0x30 / 8;
    constexpr size_t VT_IDefaultSkeleton_GetJointIDByName = 0x38 / 8;

    // IAttachment (CAttachmentBONE vtable at .rdata 0x181d185c8). The weapon is bound to the
    // first-person arms as a skeleton attachment (CArkWeapon::AttachToHand -> AddBinding of a
    // CSKELAttachment), so it is rendered as part of the arms character at
    //     characterWorldMatrix * m_AttModelRelative(* m_addTransformation)
    // and NOT from its own entity transform.
    constexpr size_t VT_IAttachment_GetAttRelativeDefault = 0x60 / 8; // const QuatT& -> this+0xF8
    constexpr size_t VT_IAttachment_GetAttModelRelative = 0x68 / 8;   // const QuatT& -> this+0x130 [ArkPlayerCamera::GetDesiredPositionAndRotation]
    constexpr size_t VT_IAttachment_HideAttachment = 0x88 / 8;        // [CArkWeapon::AttachToHand]
    constexpr size_t OFFSET_CAttachmentBONE_JOINT_ID = 0x15C;         // int m_nJointID [CAttachmentBONE::UpdateAttModelRelative]

    // ArkPlayer::m_zoomManager::m_nearFOVLockedCount is read by the cl_hfov callback at
    // ArkPlayer + 0x18C0, which pins the zoom manager at ArkPlayer + 0x1898.
    constexpr size_t ARKPLAYER_ZOOMMANAGER_OFFSET = 0x1898;
    // ArkPlayerZoomManager::Update fetches the equipped weapon through ArkPlayer + 0x14B8.
    constexpr size_t ARKPLAYER_WEAPONCOMPONENT_OFFSET = 0x14B8;
    constexpr float STOCK_WEAPON_FOV = 55.0f;

    constexpr int BONE_WEAPON = (int)ArkPlayerCamera::EArkBoneList::BONE_WEAPON;
    constexpr int BONE_CAMERA = (int)ArkPlayerCamera::EArkBoneList::BONE_CAMERA;
}

static_assert(sizeof(QuatT) == 28, "QuatT layout mismatch");
static_assert(offsetof(ArkPlayerZoomManager, m_nearFOVLockedCount) == 0x28, "ArkPlayerZoomManager layout mismatch");
static_assert(offsetof(ArkPlayer, m_zoomManager) == PreyInternals::ARKPLAYER_ZOOMMANAGER_OFFSET, "ArkPlayer layout mismatch");
static_assert(offsetof(ArkPlayer, m_weaponComponent) == PreyInternals::ARKPLAYER_WEAPONCOMPONENT_OFFSET, "ArkPlayer layout mismatch");
static_assert(offsetof(ArkPlayer, m_camera) == 0x12A0, "ArkPlayer layout mismatch");
static_assert(offsetof(CArkWeapon, m_pAttachment) == 0x2B0, "CArkWeapon layout mismatch");
static_assert(offsetof(ArkPlayerCamera, m_rotation) == 0x150, "ArkPlayerCamera layout mismatch");
static_assert(offsetof(ArkPlayerInput, m_sprintCameraRotationRateScale) == 0x98, "ArkPlayerInput layout mismatch");
static_assert(offsetof(SViewParams, rotation) == 0x0C, "SViewParams layout mismatch");

// --- Structures the GAME fills in for us ---------------------------------------------------------
// Prey's physics is not stock CryEngine, and the SDK's physinterface.h is. pe_status_living in Prey is
// 152 bytes (an extra Vec3 after velGround: groundSurfaceIdx sits at +0x64 where the header says +0x58);
// the header's 136-byte version therefore made CLivingEntity::GetStatus write 16 bytes past our local -
// straight over the saved xmm6 in the caller's frame, i.e. over MainUpdate's `dt`. That garbage step was
// what made every smoothed value (convergence, wall pull-back) either freeze or snap. So: our own layout
// for the fields we read, and generous padding after everything the game writes into.
struct PreyStatusLiving : pe_status
{
    enum entype { type_id = ePE_status_living };
    PreyStatusLiving() { type = type_id; }
    int              bFlying;            // +0x04
    float            timeFlying;         // +0x08
    Vec3             camOffset;          // +0x0C
    Vec3             vel;                // +0x18 (confirmed against the game's readers)
    Vec3             velUnconstrained;   // +0x24
    Vec3             velRequested;       // +0x30
    Vec3             velGround;          // +0x3C
    Vec3             preyExtra;          // +0x48 Prey-only field
    float            groundHeight;       // +0x54
    Vec3             groundSlope;        // +0x58
    int              groundSurfaceIdx;   // +0x64 (confirmed)
    int              groundSurfaceIdxAux;// +0x68
    IPhysicalEntity* pGroundCollider;    // +0x70
    int              iGroundColliderPart;// +0x78
    float            timeSinceStanceChange; // +0x7C
    int              bStuck;             // +0x80
    volatile int*    pLockStep;          // +0x88
    int              iCurTime;           // +0x90
    int              bSquashed;          // +0x94
    char             pad[128];           // the game may write further still; nothing of ours lives here
};
static_assert(offsetof(PreyStatusLiving, vel) == 0x18 && offsetof(PreyStatusLiving, groundSurfaceIdx) == 0x64, "PreyStatusLiving layout");
struct PaddedRayHit : ray_hit
{
    char pad[128];
};

template <typename R, typename... Args>
static inline R VCall(void* pObj, size_t slot, Args... args)
{
    using Fn = R(*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(pObj))[slot])(pObj, args...);
}

template <typename T>
static inline T& Member(void* pObj, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(pObj) + offset);
}

//! Calls IDefaultSkeleton vtable slot `slot` as `const QuatT& (unsigned jointId)` and copies the result, under a
//! structured exception handler: the slot is being *verified* (see UpdateSkeletonCache), so a wrong guess must
//! not take the game down. No C++ objects with destructors in here (SEH rule).
static bool SafeReadJoint(void* pSkel, int slot, int joint, QuatT& out)
{
    using Fn = const QuatT* (*)(void*, unsigned);
    Fn fn = reinterpret_cast<Fn>((*reinterpret_cast<void***>(pSkel))[slot]);
    __try
    {
        const QuatT* p = fn(pSkel, (unsigned)joint);
        if (!p)
            return false;
        out = *p;
        return true;
    }
    __except (1)
    {
        return false;
    }
}

//---------------------------------------------------------------------------------
// Hooks
//---------------------------------------------------------------------------------
static auto s_hookPwaUpdate = PreyInternals::CProceduralWeaponAnimation::FUpdate.MakeHook();
static auto s_hookPwaContextUpdate = PreyInternals::CProceduralWeaponAnimationContext::FUpdate.MakeHook();

static void CProceduralWeaponAnimationContext_Update_Hook(void* _this, float timePassed)
{
    using namespace PreyInternals::CProceduralWeaponAnimationContext;
    int& instanceCount = Member<int>(_this, OFFSET_INSTANCE_COUNT);
    const int rawCount = instanceCount;
    const bool modifierNull = Member<void*>(_this, OFFSET_POSE_MODIFIER) == nullptr;

    // Keep the weapon offset pipeline running while no procedural clip is active, so the
    // user offset does not pop off during jump/land animations. The pose stacks are simply
    // empty in that case, so the game contributes nothing extra.
    bool forced = false;
    if (gMod && gMod->Active() && instanceCount < 1 && !modifierNull)
    {
        instanceCount = 1;
        forced = true;
    }

    if (gMod)
        gMod->OnProceduralContextSeen(_this, rawCount, forced, modifierNull);

    const bool ran = instanceCount >= 1;
    s_hookPwaContextUpdate.InvokeOrig(_this, timePassed);

    if (forced && instanceCount == 1)
        instanceCount = 0;

    if (gMod && ran)
        gMod->OnProceduralContextUpdated(_this);
}

static void CProceduralWeaponAnimation_Update_Hook(void* _this, float deltaTime)
{
    s_hookPwaUpdate.InvokeOrig(_this, deltaTime);

    if (!gMod)
        return;

    using namespace PreyInternals::CProceduralWeaponAnimation;
    gMod->GetDiag().pwaUpdatesThisFrame++;

    const ViewmodelSettings& s = gMod->GetSettings();
    if (!gMod->Active())
        return;

    // Leave the designer debug tool (g_debugWeaponOffset 2) alone.
    if (Member<bool>(_this, OFFSET_DEBUG_INPUT_ENABLED))
        return;

    // Only the weapon hand: the support hand follows the weapon through the rig already.
    QuatT& rightOffset = Member<QuatT>(_this, OFFSET_RIGHT_OFFSET);
    gMod->ApplyOffset(rightOffset);
}

// The game's procedural weapon offsets (view space), captured so the aim lock can keep recoil/bumps.
// NOTE: these are MEMBER functions returning a 28-byte QuatT, so the MSVC x64 convention is
// (this, hidden return pointer, args...). The SDK headers declare them as free functions returning
// QuatT, which would put the return pointer first - do not use those declarations for hooking.
#define VM_OFFSET_COMPUTE_HOOK(NAME, CLASS, ADDR, SLOT)                                              \
    static auto s_fn##NAME = PreyFunction<QuatT*(CLASS* _this, QuatT* _ret, float frameTime)>(ADDR);   \
    static auto s_hook##NAME = s_fn##NAME.MakeHook();                                                 \
    static QuatT* NAME##_Hook(CLASS* _this, QuatT* _ret, float frameTime)                             \
    {                                                                                                 \
        QuatT* r = s_hook##NAME.InvokeOrig(_this, _ret, frameTime);                                   \
        if (gMod && r)                                                                                \
            gMod->SetGameOffset(SLOT, *r);                                                            \
        return r;                                                                                     \
    }
VM_OFFSET_COMPUTE_HOOK(LookCompute, CLookOffset, 0x1801770, 0)
VM_OFFSET_COMPUTE_HOOK(StrafeCompute, CStrafeOffset, 0x18039C0, 1)
VM_OFFSET_COMPUTE_HOOK(RecoilCompute, CRecoilOffset, 0x1803390, 2)
VM_OFFSET_COMPUTE_HOOK(BumpCompute, CBumpOffset, 0x1801250, 3)

// The player camera. Called by the view system once per frame after the first-person skeleton has been
// evaluated (the camera position IS the evaluated camera attachment) and before anything is rendered.
static auto s_hookUpdateView = ArkPlayerCamera::FUpdateView.MakeHook();
static void ArkPlayerCamera_UpdateView_Hook(ArkPlayerCamera* const _this, SViewParams& _params)
{
    s_hookUpdateView.InvokeOrig(_this, _params);
    if (gMod)
        gMod->OnCameraUpdated(_this, _params);
}

// The game interpolates a weapon's camera-speed (and walk-speed) multiplier towards its "zoomed"
// value based on the zoom manager's HFOV progress. Our aim camera zoom triggers that too, which is
// where the aggressive ADS sensitivity drop comes from. Substitute the zoomed camera multiplier.
static auto s_hookHfovMultiplier = ArkPlayerZoomManager::FGetHFOVDependentMultiplier.MakeHook();
static float ArkPlayerZoomManager_GetHFOVDependentMultiplier_Hook(const ArkPlayerZoomManager* const _this, float currentMultiplier, float zoomedMultiplier)
{
    if (gMod)
    {
        float replacement = gMod->GetAimSensitivityMultiplier(currentMultiplier, zoomedMultiplier);
        if (replacement > 0.0f)
            zoomedMultiplier = replacement;
    }
    return s_hookHfovMultiplier.InvokeOrig(_this, currentMultiplier, zoomedMultiplier);
}

// Shotgun spread: the game interpolates the current dispersion between a minimum and a maximum that come
// from stats (chipsets scale them the same way). Scaling both ends scales the whole cone.
//
// IMPORTANT: both getters *cache* their result in the weapon (m_minDispersion / m_maxDispersion) before
// returning, and CArkWeaponShotgun::UpdateDispersion - called every frame from the player's movement update -
// compares that cache against a fresh call to decide "did the range change?", then remaps the current
// dispersion from the old range into the new one. If we scale only the returned value the cache never matches
// what we return, that test is true every frame, and the remap multiplies the current dispersion by our factor
// on every single frame (pinning it to the minimum, or to the maximum for factors above 1). So write our value
// back into the cache and leave the game's own change detection intact.
static float s_dbgDispMinOrig = 0.0f, s_dbgDispMinOut = 0.0f;
static float s_dbgDispMaxOrig = 0.0f, s_dbgDispMaxOut = 0.0f;
static float s_dbgConeOrig = 0.0f, s_dbgConeOut = 0.0f;

static inline bool DbgFinite(float v) { return v == v && fabsf(v) < 1e30f; }

static auto s_hookDispMin = CArkWeaponShotgun::FGetDispersionMinimum.MakeHook();
static auto s_hookDispMax = CArkWeaponShotgun::FGetDispersionMaximum.MakeHook();
static float CArkWeaponShotgun_GetDispersionMinimum_Hook(CArkWeaponShotgun const* const _this)
{
    float v = s_hookDispMin.InvokeOrig(_this);
    s_dbgDispMinOrig = v;
    if (gMod)
    {
        v *= gMod->GetSpreadMultiplier(_this);
        if (DbgFinite(v) && _this)
            const_cast<CArkWeaponShotgun*>(_this)->m_minDispersion = v; // keep the game's cache consistent
    }
    s_dbgDispMinOut = v;
    return v;
}
static float CArkWeaponShotgun_GetDispersionMaximum_Hook(CArkWeaponShotgun const* const _this)
{
    float v = s_hookDispMax.InvokeOrig(_this);
    s_dbgDispMaxOrig = v;
    if (gMod)
    {
        v *= gMod->GetSpreadMultiplier(_this);
        if (DbgFinite(v) && _this)
            const_cast<CArkWeaponShotgun*>(_this)->m_maxDispersion = v;
    }
    s_dbgDispMaxOut = v;
    return v;
}
// The shotgun's pellets are laid out in a cone whose angle is the weapon stat "ShotgunSpreadConeDegrees",
// read through CArkWeapon::GetStatFloat in SpawnPellets (the dispersion above only wobbles the cone's
// centre). Scale that stat too.
static auto s_hookGetStatFloat = CArkWeapon::FGetStatFloat.MakeHook();
static float CArkWeapon_GetStatFloat_Hook(const CArkWeapon* const _this, const CCryName& _statName)
{
    float v = s_hookGetStatFloat.InvokeOrig(_this, _statName);
    if (gMod && _statName.c_str() && strcmp(_statName.c_str(), "ShotgunSpreadConeDegrees") == 0)
    {
        s_dbgConeOrig = v;
        v *= gMod->GetSpreadMultiplier(_this);
        s_dbgConeOut = v;
    }
    return v;
}

// Diagnostics only: rows and columns are read back to back at the top of SpawnPellets.
static int s_dbgStatInt[2] = { 0, 0 };
static auto s_hookGetStatInt = CArkWeapon::FGetStatInt.MakeHook();
static int CArkWeapon_GetStatInt_Hook(const CArkWeapon* const _this, const CCryName& _statName)
{
    const int v = s_hookGetStatInt.InvokeOrig(_this, _statName);
    s_dbgStatInt[0] = s_dbgStatInt[1];
    s_dbgStatInt[1] = v;
    return v;
}

// Diagnostics only: the pellet cone is built around this aim point, so its angle off the camera axis tells us
// whether the game applied its dispersion to this shot at all (see OnSpawnPellets).
static auto s_hookSpawnPellets = CArkWeaponShotgun::FSpawnPellets.MakeHook();
static void CArkWeaponShotgun_SpawnPellets_Hook(CArkWeaponShotgun* const _this, Vec3 const& _position, Quat const& _rotation, Vec3 const& _aimPoint, const bool _bIsCritical, const bool _bShootStraight, const unsigned _groupId)
{
    s_hookSpawnPellets.InvokeOrig(_this, _position, _rotation, _aimPoint, _bIsCritical, _bShootStraight, _groupId);
    if (gMod) // after the original: the stat reads it makes are this shot's values
        gMod->OnSpawnPellets(_this, _position, _aimPoint, _bShootStraight);
}

// Every shot of every weapon goes through CArkWeapon::FireWeapon - the reliable shot event (the pistol's
// procedural recoil is too small for the offset-jump heuristic below).
static auto s_hookFireWeapon = CArkWeapon::FFireWeapon.MakeHook();
static void CArkWeapon_FireWeapon_Hook(CArkWeapon* const _this)
{
    s_hookFireWeapon.InvokeOrig(_this);
    if (gMod)
        gMod->OnWeaponFired(_this);
}

// ArkPlayerInteraction::Interact(mode) is what the use / hold-use / loot / special inputs end in: it takes the
// target from m_usableEntityId, the interaction type from m_interactionInfo[mode], runs the entity's Lua
// OnUsed/... and calls PerformInteraction(). Everything it needs lives in the object, so the call can be
// stored and made a few frames later (the support hand gets there first), see ModMain::OnInteract.
static_assert(offsetof(ArkPlayerInteraction, m_interactionInfo) == 0x128, "ArkPlayerInteraction layout mismatch");
static_assert(offsetof(ArkPlayerInteraction, m_usableEntityId) == 0x46C, "ArkPlayerInteraction layout mismatch");
static_assert(offsetof(ArkPlayerInteraction, m_carryDelay) == 0x460, "ArkPlayerInteraction layout mismatch");
static_assert(sizeof(ArkInteractionInfo) == 24, "ArkInteractionInfo layout mismatch");
static auto s_hookInteract = ArkPlayerInteraction::FInteract.MakeHook();
static bool ArkPlayerInteraction_Interact_Hook(ArkPlayerInteraction* const _this, EArkInteractionMode _interactMode)
{
    if (gMod && gMod->OnInteract(_this, (int)_interactMode))
        return true; // deferred: report success now, the real call follows shortly
    return s_hookInteract.InvokeOrig(_this, _interactMode);
}

// Carrying is a two-step affair in the game: PerformInteraction(carry) only arms ArkPlayerInteraction::m_carryDelay
// (and plays a sound); when the timer runs out, ArkPlayerInteraction::Update calls
// m_playerCarry.StartCarrying(pCurrentTarget) with whatever the target selector holds at THAT moment - and
// StartCarrying dereferences it without a check. Look away (or lose the target for any other reason) during
// the delay and the game reads address 0. Anything that lengthens the window makes it likelier, so: no entity,
// no carry.
static auto s_hookStartCarrying = ArkPlayerCarry::FStartCarrying.MakeHook();
static bool ArkPlayerCarry_StartCarrying_Hook(ArkPlayerCarry* const _this, IEntity* _pEntity, const bool _bRemote, const bool _bFromSerialize)
{
    if (!_pEntity)
    {
        CryLog("ViewmodelTweaks: ArkPlayerCarry::StartCarrying called with no entity (target lost during the carry delay) - ignored instead of crashing");
        return false;
    }
    if (gMod)
        gMod->OnCarryStarted();
    return s_hookStartCarrying.InvokeOrig(_this, _pEntity, _bRemote, _bFromSerialize);
}

//---------------------------------------------------------------------------------
// Helpers
//---------------------------------------------------------------------------------
static float EaseCurve(int mode, float u); // below
static inline float SmoothStep01(float t)
{
    t = clamp_tpl(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// --- Numeric sanity -------------------------------------------------------------------------------
// Cheap checks used at the boundaries of the pipeline (what we read from the game, what we write back)
// and on every accumulated state. A NaN compares false with everything, so "!(x < limit)" catches it.
static inline bool Finite(float v) { return fabsf(v) < 1e30f; } // false for NaN and inf
static inline bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
static inline bool SaneQuat(const Quat& q)
{
    const float n2 = q.w * q.w + q.v.x * q.v.x + q.v.y * q.v.y + q.v.z * q.v.z;
    return n2 > 0.25f && n2 < 4.0f; // false for NaN
}
static inline bool SaneQuatT(const QuatT& t, float maxPos = 100.0f)
{
    return SaneQuat(t.q) && Finite(t.t) && t.t.GetLengthSquared() < maxPos * maxPos;
}
static inline Quat SafeNormalized(const Quat& q)
{
    return SaneQuat(q) ? q.GetNormalized() : Quat(IDENTITY);
}
//! Euler angles of a (possibly slightly denormalized) quaternion without the asin() domain hazard.
static Ang3 SafeAng3(const Quat& qIn)
{
    const Quat q = SafeNormalized(qIn);
    Ang3 a;
    const float sy = clamp_tpl(-(q.v.x * q.v.z - q.w * q.v.y) * 2.0f, -1.0f, 1.0f);
    a.y = asinf(sy);
    if (fabsf(fabsf(a.y) - gf_PI * 0.5f) < 0.01f)
    {
        a.x = 0.0f;
        a.z = atan2f(-2.0f * (q.v.x * q.v.y - q.w * q.v.z), 1.0f - (q.v.x * q.v.x + q.v.z * q.v.z) * 2.0f);
    }
    else
    {
        a.x = atan2f((q.v.y * q.v.z + q.w * q.v.x) * 2.0f, 1.0f - (q.v.x * q.v.x + q.v.y * q.v.y) * 2.0f);
        a.z = atan2f((q.v.x * q.v.y + q.w * q.v.z) * 2.0f, 1.0f - (q.v.z * q.v.z + q.v.y * q.v.y) * 2.0f);
    }
    return a;
}
static inline bool SanePose(const PoseOffset& p)
{
    return Finite(p.posX) && Finite(p.posY) && Finite(p.posZ) && Finite(p.pitch) && Finite(p.yaw) && Finite(p.roll)
        && fabsf(p.posX) < 10.0f && fabsf(p.posY) < 10.0f && fabsf(p.posZ) < 10.0f;
}

//! Moves `value` towards `target` so that a full 0->1 trip takes `time` seconds.
static inline void MoveTowards(float& value, float target, float time, float dt)
{
    if (time <= 0.0001f)
    {
        value = target;
        return;
    }
    float step = dt / time;
    if (value < target)
        value = min(value + step, target);
    else if (value > target)
        value = max(value - step, target);
}

static bool IsHardwareCursorVisible()
{
    if (!gEnv || !gEnv->pHardwareMouse)
        return false;
    return static_cast<CHardwareMouse*>(gEnv->pHardwareMouse)->m_iReferenceCounter > 0;
}

static const char* GetWeaponClassName(ArkPlayer* pPlayer)
{
    if (!pPlayer)
        return "";
    CArkWeapon* pWeapon = pPlayer->m_weaponComponent.GetEquippedWeapon();
    if (!pWeapon)
        return "";
    IEntity* pEntity = pWeapon->GetEntity();
    if (!pEntity || !pEntity->GetClass())
        return "";
    const char* name = pEntity->GetClass()->GetName();
    return name ? name : "";
}

static QuatT BlendQuatT(const QuatT& a, const QuatT& b, float t)
{
    t = clamp_tpl(t, 0.0f, 1.0f);
    QuatT r;
    r.t = LERP(a.t, b.t, t);
    r.q = SafeNormalized(Quat::CreateNlerp(a.q, b.q, t));
    return r;
}

float ModMain::Now() const
{
    return (gEnv && gEnv->pTimer) ? gEnv->pTimer->GetCurrTime() : 0.0f;
}

//---------------------------------------------------------------------------------
// Additive offsets (global standing / crouch + per-weapon hip + legacy aim)
//---------------------------------------------------------------------------------
void ModMain::SetGameOffset(int which, const QuatT& q)
{
    if (which < 0 || which > 3)
        return;
    if (SaneQuatT(q, 5.0f))
        m_gameOffsets[which] = q;
    else
    {
        m_gameOffsets[which] = QuatT(IDENTITY);
        m_nanRecoveries++;
    }
}

void ModMain::ApplyOffset(QuatT& offset) const
{
    const ViewmodelSettings& s = m_settings;
    if (!SaneQuatT(offset, 10.0f))
        return; // the game's own offset is bad this frame: leave it alone
    const QuatT original = offset;

    const float ab = s.aimEnabled ? SmoothStep01(m_aimBlend) : 0.0f;
    float cb = s.crouchEnabled ? SmoothStep01(m_crouchBlend) : 0.0f;
    if (s.aimIgnoresCrouch)
        cb *= (1.0f - ab); // the aim pose must line the sights up regardless of stance

    PoseOffset total = s.base;
    if (cb > 0.0f)
        total.AddScaled(s.crouch, cb);
    const WeaponSettings* pWeapon = FindCurrentWeapon();
    if (pWeapon)
        total.AddScaled(pWeapon->hip, 1.0f);
    // Near-wall pose: the weapon's own "against a wall" attitude (e.g. shotgun muzzle up), blended in as the
    // wall pull-back builds up (m_wallBlend, see UpdateConvergence). Hip only: aiming is blocked near walls.
    const float wb = (s.wallPoseEnabled && pWeapon && SanePose(pWeapon->wall)) ? clamp_tpl(m_wallBlend, 0.0f, 1.0f) * (1.0f - ab) : 0.0f;
    if (wb > 0.0f)
        total.AddScaled(pWeapon->wall, wb);
    // Feel layer (hip side): sprint pose + sprint sway, view drag. Both fade out as the sights come up;
    // the aim side has its own versions.
    if (SanePose(m_feel.sprintOut))
        total.AddScaled(m_feel.sprintOut, 1.0f - ab);   // already faded to zero when the feature is off
    if (SanePose(m_feel.dragHipOut))
        total.AddScaled(m_feel.dragHipOut, 1.0f - ab);
    // Quick melee: the weapon drops out of the way while the punch plays.
    if (m_interact.meleeLowerBlend > 0.0f && SanePose(s.meleeLower))
        total.AddScaled(s.meleeLower, EaseCurve(s.meleeLowerEase, clamp_tpl(m_interact.meleeLowerBlend, 0.0f, 1.0f)));

    // Reloads: the support hand is animated in place (shells, magazines) against where the weapon is in
    // the stock pose, so everything that moves the weapon fades out for the duration and comes back after.
    const float keep = 1.0f - SmoothStep01(m_reloadFade);
    if (keep < 1.0f)
    {
        PoseOffset scaled;
        scaled.AddScaled(total, keep);
        total = scaled;
    }

    const Quat userRot = total.Rot();
    const Vec3 userPos = total.Pos();

    // Rotate the weapon in place (around its hand pivot) and translate in view space.
    offset.t += userPos;
    offset.q = userRot * offset.q;

    // Wall pull-back: slide the weapon towards the camera along the view axis. While aiming the lock
    // takes over (see ComputeAimExtra), so fade this copy out with the aim blend.
    if (s.wallPushEnabled && m_wallPush != 0.0f)
        offset.t.y -= m_wallPush * (1.0f - ab) * keep;

    // Hip-fire convergence: rotate in place so the barrel points at the crosshair's impact point.
    // Fades out with the aim blend (the ironsight lock is on the camera ray by definition).
    if (s.convergeEnabled && (m_convergeYaw != 0.0f || m_convergePitch != 0.0f) && keep > 0.0f)
    {
        // The wall pose deliberately points the barrel away from the impact point, so convergence yields to it.
        const float k = (1.0f - ab) * keep * (1.0f - wb * clamp_tpl(s.wallPoseConvergeFade, 0.0f, 1.0f));
        const Quat conv = Quat::CreateRotationXYZ(Ang3(DEG2RAD(m_convergePitch * k), 0.0f, DEG2RAD(m_convergeYaw * k)));
        offset.q = conv * offset.q;
    }
    offset.q = SafeNormalized(offset.q);
    if (!SaneQuatT(offset, 10.0f))
    {
        offset = original; // never hand the game a bad transform
        m_nanRecoveries++;
    }
}

//---------------------------------------------------------------------------------
// Diagnostics
//---------------------------------------------------------------------------------
void ModMain::OnProceduralContextSeen(void* pContext, int instanceCount, bool forced, bool modifierNull)
{
    using namespace PreyInternals::CProceduralWeaponAnimationContext;
    PipelineDiag& d = m_diag;
    d.ctxUpdatesThisFrame++;

    const float t = Now();
    if (pContext != d.lastContext)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "context changed %p -> %p", d.lastContext, pContext);
        d.Add(t, buf);
        d.lastContext = pContext;
    }
    if ((instanceCount < 1) != (d.lastInstanceCount < 1))
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "instanceCount %d -> %d (%s)", d.lastInstanceCount, instanceCount, forced ? "forced alive" : "not forced");
        d.Add(t, buf);
    }
    d.lastInstanceCount = instanceCount;
    d.lastForced = forced;
    if (modifierNull != d.lastModifierNull)
    {
        d.Add(t, modifierNull ? "pose modifier is NULL" : "pose modifier present");
        d.lastModifierNull = modifierNull;
    }
    const int ik = Member<int>(pContext, OFFSET_RIGHT_IK_JOINT);
    if (ik != d.lastRightIk)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "right IK joint %d -> %d", d.lastRightIk, ik);
        d.Add(t, buf);
        d.lastRightIk = ik;
    }
}

//---------------------------------------------------------------------------------
// Ironsight lock
//---------------------------------------------------------------------------------
static QuatT ScaleQuatT(const QuatT& q, float k)
{
    if (!(k > 0.0f) || !SaneQuatT(q)) return QuatT(IDENTITY);
    if (fabsf(k - 1.0f) < 1e-4f) return q;
    QuatT r;
    const Ang3 a = SafeAng3(q.q);
    r.q = Quat::CreateRotationXYZ(Ang3(a.x * k, a.y * k, a.z * k));
    r.t = q.t * k;
    return r;
}

float ModMain::GetLockBlend() const
{
    const ViewmodelSettings& s = m_settings;
    if (!Active() || !s.aimEnabled)
        return 0.0f;
    if (m_playerDead || m_reviveGuard > 0.0f)
        return 0.0f; // death model / model reload: attachments and joints are being recreated
    return SmoothStep01(m_aimBlend);
}

QuatT ModMain::ComputeAimExtra() const
{
    const ViewmodelSettings& s = m_settings;
    // Keep (part of) the game's procedural offsets so the locked weapon still kicks and bumps
    // (the camera itself already carries the game's camera recoil), and optionally couple the
    // weapon to the head bob: the camera bobs with the animated head, so a weapon that is glued to
    // the camera looks perfectly still on screen; subtracting a fraction of the camera's own bob makes
    // it lag behind the head like a real ADS viewmodel. All terms are known exactly this frame.
    QuatT extra(IDENTITY);
    const WeaponSettings* pW = FindCurrentWeapon();
    const float recoilScale = pW ? pW->aimRecoilScale : 1.0f;
    if (s.aimAnimSway > 0.0f)
        extra = extra * ScaleQuatT(m_gameOffsets[0], s.aimAnimSway) * ScaleQuatT(m_gameOffsets[1], s.aimAnimSway);
    if (s.aimAnimRecoil * recoilScale > 0.0f)
        extra = extra * ScaleQuatT(m_gameOffsets[2], s.aimAnimRecoil * recoilScale) * ScaleQuatT(m_gameOffsets[3], s.aimAnimRecoil * recoilScale);
    // Head-bob coupling, boosted for a moment after each shot so the fire kick (which reaches the camera
    // through the body animation) shows up as weapon motion relative to the sights.
    float coupling = clamp_tpl(s.aimBobAmount, -1.0f, 1.0f);
    if (pW && m_fireTimer > 0.0f && pW->fireCouplingTime > 0.001f)
        coupling = max(coupling, pW->fireCoupling * clamp_tpl(m_fireTimer / pW->fireCouplingTime, 0.0f, 1.0f));
    if (coupling != 0.0f && m_render.bobValid)
        extra.t -= m_render.bobCam * coupling;
    // Wall pull-back along the view axis keeps the sights on the crosshair, so it can stay on while aiming.
    if (s.wallPushEnabled && s.wallPushWhileAiming && m_wallPush != 0.0f)
        extra.t.y -= m_wallPush;
    return extra;
}

//---------------------------------------------------------------------------------
// Skeleton side (before the animation is evaluated)
//---------------------------------------------------------------------------------
void ModMain::OnProceduralContextUpdated(void* pContext)
{
    using namespace PreyInternals;
    using namespace PreyInternals::CProceduralWeaponAnimationContext;

    AimLockState& L = m_lock;
    const RenderLockState& R = m_render;

    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    void* pModifier = Member<void*>(pContext, OFFSET_POSE_MODIFIER);
    void* pScope = Member<void*>(pContext, OFFSET_SCOPE);
    if (!pPlayer || !pModifier || !pScope || !pPlayer->GetEntity())
    {
        L.active = false;
        L.captured = false;
        L.addValid = false;
        L.kick = QuatT(IDENTITY);
        return;
    }

    L.rightIkJoint = Member<int>(pContext, OFFSET_RIGHT_IK_JOINT);
    L.leftIkJoint = Member<int>(pContext, OFFSET_LEFT_IK_JOINT);
    L.weaponJoint = Member<int>(pContext, OFFSET_WEAPON_TARGET_JOINT);

    void* pCharInst = VCall<void*>(pScope, VT_IScope_GetCharInst);
    ICharacterInstance* pPlayerChar = pPlayer->GetEntity()->GetCharacter(0);
    L.charMatches = pCharInst && pCharInst == static_cast<void*>(pPlayerChar);
    if (!pCharInst || L.rightIkJoint < 0)
    {
        L.active = false;
        L.captured = false;
        L.addValid = false;
        L.kick = QuatT(IDENTITY);
        return;
    }

    // Skeleton readings are the *previous* frame's final pose (model space).
    void* pSkelPose = VCall<void*>(pCharInst, VT_ICharacterInstance_GetISkeletonPose);
    if (!pSkelPose)
        return;
    const QuatT* pIkAbs = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, L.rightIkJoint);
    if (!pIkAbs || !SaneQuatT(*pIkAbs))
    {
        // Skeleton not evaluated yet (first frames after a load) or garbage: do nothing this frame.
        L.active = false;
        L.captured = false;
        L.addValid = false;
        L.kick = QuatT(IDENTITY);
        m_nanRecoveries += pIkAbs ? 1 : 0;
        return;
    }
    L.ikAbs = *pIkAbs;
    // "Weapon" here means the weapon attachment (what gets rendered and what the render side places),
    // read from the game's values of the previous frame (before the render side moved the hands).
    if (R.attOffsetValid && SaneQuatT(R.weaponBoneGame) && SaneQuatT(R.attOffset))
        L.weaponAbs = R.weaponBoneGame * R.attOffset;
    else
        L.weaponAbs = pPlayer->GetBoneTransform(BONE_WEAPON);
    if (!SaneQuatT(L.weaponAbs))
    {
        L.active = false;
        L.captured = false;
        L.addValid = false;
        L.kick = QuatT(IDENTITY);
        m_nanRecoveries++;
        return;
    }
    L.ikErr = (L.ikAbs.t - L.lastTarget.t).GetLength();

    const float ab = L.charMatches ? GetLockBlend() : 0.0f;

    // --- Fire animation pass-through -------------------------------------------------------------
    // The animated (pre-modifier) hand of last frame = last frame's final hand minus what we added to it.
    {
        const QuatT finalIk = (R.ikGameValid && SaneQuatT(R.ikGame)) ? R.ikGame : L.ikAbs;
        QuatT anim = finalIk;
        if (L.addValid)
        {
            // Only subtract our add if the skeleton actually applied it: the animation update can be skipped
            // (pause menu, character not updated) while this hook still runs, and subtracting a push that never
            // happened would make the next push grow without bound. Test: the final hand must be near
            // "last animated hand + our add".
            // Two hypotheses for last frame's final hand: "animated + our add" (applied) or "animated only"
            // (the queue did not run). Pick the closer one; the error is then at most the smaller of the two.
            const Vec3 expected = L.animIk.t + L.lastAdd.t;
            const float dApplied = (finalIk.t - expected).GetLengthSquared();
            const float dSkipped = (finalIk.t - L.animIk.t).GetLengthSquared();
            if (dApplied <= dSkipped && dApplied < 0.15f * 0.15f)
            {
                anim.t -= L.lastAdd.t;
                anim.q = SafeNormalized((!L.lastAdd.q) * anim.q);
            }
            else
                L.pushesNotApplied++;
        }
        L.animIk = anim;
        // Relative to the camera of that frame, so head motion does not count as hand motion.
        const QuatT animRelCam = SaneQuatT(L.camAbs) ? (L.camAbs.GetInverted() * anim) : QuatT(IDENTITY);
        L.animRelCam = animRelCam;
        L.animRelCamValid = R.camValid && L.addValid && SaneQuatT(L.camAbs) && SaneQuatT(animRelCam, 5.0f);
        const float dt = clamp_tpl((gEnv && gEnv->pTimer) ? gEnv->pTimer->GetFrameTime() : 0.016f, 0.0f, 0.1f);
        const bool restBad = !L.animRestValid || !SaneQuatT(L.animRestRelCam, 5.0f) || !SaneQuatT(animRelCam, 5.0f)
            || !((animRelCam.t - L.animRestRelCam.t).GetLengthSquared() < 0.5f * 0.5f) || !R.camValid;
        if (restBad)
        {
            L.animRestRelCam = animRelCam;
            L.animRestValid = R.camValid && SaneQuatT(animRelCam, 5.0f);
        }
        else if (m_fireTimer <= 0.0f)
        {
            // Slow reference; frozen while a shot plays out so the kick is measured against the pose before it.
            const float k = 1.0f - expf(-dt / 0.35f);
            L.animRestRelCam = BlendQuatT(L.animRestRelCam, animRelCam, k);
        }
        QuatT dev = L.animRestValid ? (L.animRestRelCam.GetInverted() * animRelCam) : QuatT(IDENTITY); // hand-local deviation
        dev.q = SafeNormalized(dev.q);
        L.kickPos = Finite(dev.t) ? dev.t.GetLength() : 1e9f;
        L.kickRot = 2.0f * acosf(clamp_tpl(fabsf(dev.q.w), 0.0f, 1.0f));
        const WeaponSettings* pW = FindCurrentWeapon();
        const float T = (pW && pW->fireCouplingTime > 0.05f) ? pW->fireCouplingTime : 0.3f;
        // Gate: only around shots (idle breathing / walk bob must not leak into the sights).
        const float gate = (m_fireTimer > 0.0f) ? SmoothStep01(m_fireTimer / (0.3f * T)) : 0.0f;
        const float scale = (pW ? pW->aimKickScale : 1.0f) * gate;
        if (scale > 0.0f && ab > 0.0f && L.animRestValid && L.captured && L.kickPos < 0.25f && L.kickRot < DEG2RAD(45.0f))
        {
            // dev is the hand's motion in the hand's own axes. The weapon rides rigidly on the hand
            // (weapon = hand * weaponRel), so the same motion in the weapon's axes is weaponRel^-1 * dev * weaponRel.
            QuatT kickW = L.weaponRel.GetInverted() * dev * L.weaponRel;
            kickW.q = SafeNormalized(kickW.q);
            L.kick = SaneQuatT(kickW, 1.0f) ? ScaleQuatT(kickW, scale) : QuatT(IDENTITY);
        }
        else
            L.kick = QuatT(IDENTITY);
    }

    QuatT camAbs(IDENTITY);
    PredictCamera(pPlayer, camAbs);

    PushAimLock(pModifier, camAbs, L.ikAbs, L.weaponAbs, ab);
    PushInteractReach(pModifier, pSkelPose, camAbs);
}

bool ModMain::PredictCamera(ArkPlayer* pPlayer, QuatT& camAbs) const
{
    using namespace PreyInternals;
    const RenderLockState& R = m_render;
    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
    if (!pEnt)
        return false;
    const Quat entRot = SafeNormalized(pEnt->GetWorldRotation());
    const Quat freshRot = SafeNormalized(pPlayer->m_camera.m_rotation); // this frame's view rotation (updated in PrePhysicsUpdate)

    // Examination mode: the view is ArkExaminationMode's, not the player's look rotation, and it lerps slowly -
    // last frame's real camera is the best there is.
    if (m_interact.examining && R.camValid && SaneQuatT(R.camModel, 50.0f))
    {
        camAbs = R.camModel;
        return true;
    }

    // Best available camera for this frame: last frame's exact camera (position in model space is
    // frame-independent; a frame of head motion is a millimetre) with this frame's mouse look applied.
    // Nothing here is filtered or integrated; the render-side step fixes whatever is left.
    if (R.camValid && SaneQuat(R.freshRotAtCam) && SaneQuat(R.camWorldRot) && Finite(R.camModelPos))
    {
        const Quat delta = SafeNormalized(freshRot * (!R.freshRotAtCam));
        const Quat camRotWorld = SafeNormalized(delta * R.camWorldRot);
        camAbs.q = SafeNormalized((!entRot) * camRotWorld);
        camAbs.t = R.camModelPos;
    }
    else
    {
        const QuatT camBone = pPlayer->GetBoneTransform(BONE_CAMERA);
        camAbs.q = SafeNormalized((!entRot) * freshRot);
        camAbs.t = Finite(camBone.t) ? camBone.t : Vec3(0.0f, 0.0f, 1.6f);
    }
    return SaneQuatT(camAbs, 50.0f);
}

void ModMain::PushAimLock(void* pModifier, const QuatT& camAbs, const QuatT& ikAbs, const QuatT& weaponBone, float ab)
{
    using namespace PreyInternals;
    AimLockState& L = m_lock;

    L.camAbs = camAbs;

    if (ab <= 0.0f || !pModifier)
    {
        L.active = false;
        L.captured = false;
        L.addValid = false;
        L.kick = QuatT(IDENTITY);
        return;
    }

    if (!L.captured || L.weaponClass != m_currentWeaponClass)
    {
        const QuatT hipRel = camAbs.GetInverted() * ikAbs;       // where the hand is now, relative to the camera
        const QuatT weaponRel = ikAbs.GetInverted() * weaponBone; // hand -> weapon bone, assumed rigid
        if (!SaneQuatT(hipRel, 5.0f) || !SaneQuatT(weaponRel, 5.0f))
        {
            // Not a usable pose (skeleton still settling): try again next frame, push nothing.
            L.active = false;
            L.addValid = false;
            L.kick = QuatT(IDENTITY);
            m_nanRecoveries++;
            return;
        }
        if (!L.active)
            L.hipRel = hipRel;
        L.weaponRel = weaponRel;
        L.weaponClass = m_currentWeaponClass;
        L.captured = true;
        L.animRestValid = false;
    }
    L.active = true;

    const WeaponSettings& w = GetCurrentWeapon();
    const QuatT desiredRelCam = ComputeAimExtra() * w.aim.AsQuatT() * ComputeAimLocal() * L.kick;

    const QuatT aimWeaponAbs = camAbs * desiredRelCam;                  // where the weapon bone should be
    const QuatT aimIkAbs = aimWeaponAbs * L.weaponRel.GetInverted();    // -> where the hand IK target must be
    // Hip side of the blend: the LIVE animated hand (last frame's, following the camera), not the pose
    // captured when aiming started - so whatever the hip path does meanwhile (sprint pose, view drag,
    // crouch) is what the weapon returns to, without a pop when the lock releases.
    const QuatT hipAbs = camAbs * (L.animRelCamValid ? L.animRelCam : L.hipRel);
    QuatT target = BlendQuatT(hipAbs, aimIkAbs, ab);
    target.q.Normalize();

    // Additive, not override: the fire animation keeps moving the hand (the deviation is measured above
    // and fed back into the aim pose). The add is relative to last frame's animated hand; the frame of
    // animation velocity that leaves is fixed exactly on the render side.
    QuatT add;
    add.t = target.t - L.animIk.t;
    add.q = SafeNormalized(target.q * (!L.animIk.q));
    // Never push anything unreasonable into the skeleton: a hand more than ~a metre from where the
    // animation put it means our bookkeeping is off - drop this frame and start the additive chain afresh.
    if (!SaneQuatT(target, 50.0f) || !Finite(add.t) || add.t.GetLengthSquared() > 1.0f * 1.0f)
    {
        L.addValid = false;
        L.kick = QuatT(IDENTITY);
        m_nanRecoveries++;
        return;
    }
    VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, L.rightIkJoint, OP_ADDITIVE, &add.t);
    VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, L.rightIkJoint, OP_ADDITIVE, &add.q);
    L.lastAdd = add;
    L.addValid = true;
    L.lastTarget = target;
}

//---------------------------------------------------------------------------------
// Render side (after the camera is final)
//---------------------------------------------------------------------------------
void ModMain::UpdateSkeletonCache(void* pCharInst, void* pAttachment)
{
    using namespace PreyInternals;
    RenderLockState& R = m_render;

    void* pSkel = VCall<void*>(pCharInst, VT_ICharacterInstance_GetIDefaultSkeleton);
    if (!pSkel)
        return;
    const int count = (int)VCall<unsigned>(pSkel, VT_IDefaultSkeleton_GetJointCount);
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    // With no weapon there is no attachment: the weapon prop joint comes from the camera's bone list instead.
    int attachJoint = pAttachment ? Member<int>(pAttachment, OFFSET_CAttachmentBONE_JOINT_ID) : -1;
    if (attachJoint < 0 && pPlayer)
        attachJoint = pPlayer->m_camera.GetJointId((int)ArkPlayerCamera::EArkBoneList::BONE_WEAPON);
    if (pSkel == R.skeleton && count == R.jointCount && attachJoint == R.attachJoint)
        return;

    R.skeleton = pSkel;
    R.jointCount = count;
    R.attachJoint = attachJoint;
    R.rightSubtree.clear();
    R.leftSubtree.clear();
    R.rightHand = R.leftHand = -1;
    R.attachJointName.clear();
    R.rightHandName.clear();
    R.leftHandName.clear();
    if (count <= 0 || count > 4096)
        return;

    auto jointName = [&](int id) -> const char* {
        if (id < 0 || id >= count) return "";
        const char* n = VCall<const char*>(pSkel, VT_IDefaultSkeleton_GetJointNameByID, id);
        return n ? n : "";
    };
    auto parentOf = [&](int id) -> int {
        if (id < 0 || id >= count) return -1;
        return VCall<int>(pSkel, VT_IDefaultSkeleton_GetJointParentIDByID, id);
    };
    // The weapon hangs off a "prop" joint that is a child of the hand joint. Move the whole hand.
    auto handOf = [&](int propJoint) -> int {
        if (propJoint < 0 || propJoint >= count) return -1;
        const char* n = jointName(propJoint);
        if (strstr(n, "Prop") || strstr(n, "prop") || strstr(n, "weapon"))
        {
            const int p = parentOf(propJoint);
            return p >= 0 ? p : propJoint;
        }
        return propJoint;
    };

    R.attachJointName = jointName(attachJoint);
    R.rightHand = handOf(attachJoint);
    const int leftProp = pPlayer ? pPlayer->m_camera.GetJointId((int)ArkPlayerCamera::EArkBoneList::BONE_WEAPON2) : -1;
    R.leftHand = handOf(leftProp);
    R.rightHandName = jointName(R.rightHand);
    R.leftHandName = jointName(R.leftHand);

    // The left arm's animation-driven IK weight joint (the game forces the right one, "r_hand_spine_blend",
    // to 1 every frame; the left one is animated). Name inferred from the right one - may not exist.
    m_interact.weightJoint = -1;
    m_interact.weightJointName.clear();
    for (const char* candidate : { "l_hand_spine_blend", "l_hand_blend", "l_hand_spine_target_blend" })
    {
        const int id = VCall<int>(pSkel, VT_IDefaultSkeleton_GetJointIDByName, candidate);
        if (id >= 0 && id < count)
        {
            m_interact.weightJoint = id;
            m_interact.weightJointName = candidate;
            m_interact.weightJointParent = VCall<int>(pSkel, VT_IDefaultSkeleton_GetJointParentIDByID, id);
            if (m_interact.weightJointParent < 0 || m_interact.weightJointParent >= count) m_interact.weightJointParent = -1;
            break;
        }
    }

    // Subtrees (CryEngine guarantees parent index < child index).
    std::vector<char> inRight(count, 0), inLeft(count, 0);
    for (int i = 0; i < count; i++)
    {
        const int p = parentOf(i);
        inRight[i] = (i == R.rightHand) || (p >= 0 && inRight[p]);
        inLeft[i] = (i == R.leftHand) || (p >= 0 && inLeft[p]);
        if (inRight[i]) R.rightSubtree.push_back(i);
        if (inLeft[i] && !inRight[i]) R.leftSubtree.push_back(i);
    }
    R.leftUpperArm = -1;
    if (R.leftHand >= 0)
    {
        const int forearm = parentOf(R.leftHand);
        R.leftUpperArm = forearm >= 0 ? parentOf(forearm) : -1;
    }
    R.leftSubtreeNames.clear();
    R.leftSubtreeParent.clear();
    for (int id : R.leftSubtree)
    {
        R.leftSubtreeNames.emplace_back(jointName(id));
        R.leftSubtreeParent.push_back(parentOf(id));
    }
    m_interact.animValid = false; // captured against another skeleton

    // Bind pose. IDefaultSkeleton has GetDefaultAbsJointByID / GetDefaultRelJointByID right after
    // GetJointIDByName in every CryEngine of this era (slots 8 and 9 here); the SDK has no header for it, so
    // the two slots are verified on the live object before use: every joint must satisfy
    // abs[j] == abs[parent] * rel[j], which no other pair of getters does by accident.
    R.defaultPoseValid = false;
    R.defaultRelSlot = R.defaultAbsSlot = -1;
    R.leftSubtreeDefaultRel.assign(R.leftSubtree.size(), Quat(IDENTITY));
    {
        const int candidates[][2] = { { 8, 9 }, { 9, 8 }, { 9, 10 }, { 10, 11 } }; // {abs, rel}
        for (const auto& c : candidates)
        {
            int checked = 0, bad = 0;
            for (int j = 0; j < count && bad == 0; j++)
            {
                QuatT absJ, relJ, absP;
                if (!SafeReadJoint(pSkel, c[0], j, absJ) || !SafeReadJoint(pSkel, c[1], j, relJ)) { bad++; break; }
                if (!SaneQuatT(absJ, 20.0f) || !SaneQuatT(relJ, 20.0f)) { bad++; break; }
                const int p = parentOf(j);
                if (p < 0)
                    continue; // roots: rel == abs is not guaranteed for every rig, skip
                if (!SafeReadJoint(pSkel, c[0], p, absP) || !SaneQuatT(absP, 20.0f)) { bad++; break; }
                const QuatT recon = absP * relJ;
                if ((recon.t - absJ.t).GetLength() > 0.002f || fabsf(fabsf(recon.q | absJ.q) - 1.0f) > 0.002f)
                    bad++;
                checked++;
            }
            if (bad == 0 && checked > 8)
            {
                R.defaultPoseValid = true;
                R.defaultAbsSlot = c[0];
                R.defaultRelSlot = c[1];
                for (size_t i = 0; i < R.leftSubtree.size(); i++)
                {
                    QuatT rel;
                    if (SafeReadJoint(pSkel, c[1], R.leftSubtree[i], rel))
                        R.leftSubtreeDefaultRel[i] = SafeNormalized(rel.q);
                }
                break;
            }
        }
        CryLog("ViewmodelTweaks: arms skeleton {} joints, hand subtree {} joints, bind pose accessors {}", count, (int)R.leftSubtree.size(),
            R.defaultPoseValid ? "verified" : "NOT found (hand poses fall back to identity rotations)");
    }
}

void ModMain::OnWeaponFired(CArkWeapon* pWeapon)
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !pPlayer->GetEntity() || !pWeapon)
        return;
    if (pWeapon->GetOwnerId() != pPlayer->GetEntity()->GetId())
        return;
    m_shotPending = true;
}

void ModMain::OnCameraUpdated(ArkPlayerCamera* pCamera, SViewParams& params)
{
    using namespace PreyInternals;
    RenderLockState& R = m_render;
    const ViewmodelSettings& s = m_settings;

    if (R.frameOfLastCall != m_frameIndex)
    {
        R.callsLastFrame = R.callsThisFrame;
        R.callsThisFrame = 0;
        R.frameOfLastCall = m_frameIndex;
    }
    R.callsThisFrame++;
    R.ikGameValid = false; // re-captured below when everything checks out; stale values must never survive an early return

    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || pCamera != &pPlayer->m_camera || !pPlayer->GetEntity())
        return;
    IEntity* pEnt = pPlayer->GetEntity();

    // --- Exact camera of this frame, in model (entity) space -------------------------------------
    // With camera-space rendering (ENTITY_SLOT_RENDER_NEAREST on the arms) UpdateView returns the
    // position relative to the entity; CView adds the entity position afterwards. Detect the other case.
    // Anything that is not a number (first frames of a load, a camera that has not been set up yet) means
    // this frame is skipped and every accumulated state is invalidated rather than poisoned.
    if (!SaneQuat(params.rotation) || !Finite(params.position) || !SaneQuat(pEnt->GetWorldRotation()) || !Finite(pEnt->GetWorldPos()))
    {
        R.camValid = false;
        R.attachValid = false;
        R.bobValid = false;
        m_nanRecoveries++;
        return;
    }
    // Quick melee: a small camera kick (pitch down, a touch of yaw) that comes and goes with the punch.
    if (m_interact.meleeKickTime >= 0.0f && (s.meleeCamKick != 0.0f || s.meleeCamKickYaw != 0.0f))
    {
        const float u = clamp_tpl(m_interact.meleeKickTime / max(s.meleeCamKickTime, 0.01f), 0.0f, 1.0f);
        const float a = sinf(gf_PI * u);
        const Quat kick = Quat::CreateRotationXYZ(Ang3(DEG2RAD(-clamp_tpl(s.meleeCamKick, -10.0f, 10.0f) * a), 0.0f, DEG2RAD(-clamp_tpl(s.meleeCamKickYaw, -10.0f, 10.0f) * a)));
        const Quat q = SafeNormalized(params.rotation * kick);
        if (SaneQuat(q))
            params.rotation = q;
    }
    const Quat entRot = SafeNormalized(pEnt->GetWorldRotation());
    Vec3 camPosRel = params.position;
    R.positionWasWorld = camPosRel.GetLengthSquared() > 20.0f * 20.0f;
    if (R.positionWasWorld)
        camPosRel -= pEnt->GetWorldPos();
    const Quat camRotWorld = SafeNormalized(params.rotation);

    QuatT camModel;
    camModel.q = SafeNormalized((!entRot) * camRotWorld);
    camModel.t = (!entRot) * camPosRel;
    if (m_interact.examining && gEnv && gEnv->pSystem)
    {
        // What was actually rendered last frame (ArkExaminationMode overrides the view after this hook):
        // the view camera, taken into entity space.
        const CCamera& vc = gEnv->pSystem->GetViewCamera();
        const Matrix34 m = vc.GetMatrix();
        const Vec3 wp = m.GetTranslation();
        const Quat wq = Quat(Matrix33(m));
        if (Finite(wp) && SaneQuat(wq))
        {
            camModel.q = SafeNormalized((!entRot) * wq);
            camModel.t = (!entRot) * (wp - pEnt->GetWorldPos());
        }
    }
    if (!SaneQuatT(camModel, 50.0f))
    {
        R.camValid = false;
        R.attachValid = false;
        R.bobValid = false;
        m_nanRecoveries++;
        return;
    }
    R.camModel = camModel;

    R.camWorldRot = camRotWorld;
    R.camModelPos = camModel.t;
    R.freshRotAtCam = pPlayer->m_camera.m_rotation;
    R.camValid = true;

    const float dt = (gEnv && gEnv->pTimer) ? gEnv->pTimer->GetFrameTime() : 0.016f;

    // Head bob = camera motion minus its slow component (posture, crouch, lean, eye height).
    {
        if (!R.bobValid || !Finite(R.camLowPass) || !((camModel.t - R.camLowPass).GetLengthSquared() < 1.0f))
        {
            R.camLowPass = camModel.t;
            R.bobValid = true;
        }
        const float tau = max(s.aimBobTau, 0.02f);
        const float k = 1.0f - expf(-dt / tau);
        R.camLowPass += (camModel.t - R.camLowPass) * k;
        Vec3 bob = camModel.t - R.camLowPass;
        const float maxBob = 0.05f;
        if (bob.GetLength() > maxBob)
            bob = bob.GetNormalized() * maxBob;
        R.bobCam = (!camModel.q) * bob;
    }

    // Shot detection: the camera recoil timer is re-armed by every shot, and the procedural recoil
    // offset jumps. Either one starts the post-shot coupling boost.
    {
        const float camRecoil = pPlayer->m_camera.m_recoilTimeRemaining;
        const float recoilMag = m_gameOffsets[2].t.GetLength();
        const bool shot = m_shotPending || (camRecoil > m_prevCamRecoilTime + 1e-4f) || (recoilMag > m_prevRecoilMag + 0.002f && recoilMag > 0.003f);
        m_shotPending = false;
        m_prevCamRecoilTime = camRecoil;
        m_prevRecoilMag = recoilMag;
        if (shot)
        {
            m_shotsSeen++;
            const WeaponSettings* pW = FindCurrentWeapon();
            m_fireTimer = max(m_fireTimer, pW ? pW->fireCouplingTime : 0.3f);
        }
        else if (m_fireTimer > 0.0f)
            m_fireTimer = max(0.0f, m_fireTimer - dt);
    }

    // --- Weapon attachment ------------------------------------------------------------------------
    if (m_playerDead || m_reviveGuard > 0.0f)
    {
        R.attachValid = false;
        return;
    }
    CArkWeapon* pWeapon = pPlayer->m_weaponComponent.GetEquippedWeapon();
    // Only a weapon the player actually owns has a live attachment on the arms.
    if (pWeapon && pWeapon->GetOwnerId() != pEnt->GetId())
        pWeapon = nullptr;
    void* pAttachment = pWeapon ? static_cast<void*>(pWeapon->m_pAttachment) : nullptr;
    ICharacterInstance* pCharInst = pEnt->GetCharacter(0);
    if (pCharInst)
        UpdateSkeletonCache(pCharInst, pAttachment); // hand subtrees are needed with no weapon too (interaction reach)

    if (!pAttachment || !pCharInst)
    {
        R.attachValid = false;
        return;
    }

    QuatT* pModelRel = const_cast<QuatT*>(VCall<const QuatT*>(pAttachment, VT_IAttachment_GetAttModelRelative));
    if (!pModelRel)
    {
        R.attachValid = false;
        return;
    }
    const QuatT gameModel = *pModelRel;
    const QuatT weaponBone = pPlayer->GetBoneTransform(BONE_WEAPON);
    if (!SaneQuatT(gameModel, 50.0f) || !SaneQuatT(weaponBone, 50.0f))
    {
        // The game's own values are not usable this frame (attachment being recreated, model reload):
        // leave everything as the game has it and forget our references.
        R.attachValid = false;
        R.attOffsetValid = false;
        R.hipValid = false;
        R.ikGameValid = false;
        m_nanRecoveries++;
        return;
    }
    R.attachValid = true;
    R.weaponModelGame = gameModel;
    R.weaponRelCam = camModel.GetInverted() * gameModel;
    // Final (unmodified) weapon bone of this frame and its constant offset to the attachment, so the
    // skeleton-side push next frame targets the very same thing we place here.
    R.weaponBoneGame = weaponBone;
    R.attOffset = R.weaponBoneGame.GetInverted() * gameModel;
    R.attOffsetValid = true;
    R.ikGameValid = false;
    if (m_lock.rightIkJoint >= 0)
    {
        if (void* pSkelPose0 = VCall<void*>(pCharInst, VT_ICharacterInstance_GetISkeletonPose))
            if (const QuatT* pIk = VCall<const QuatT*>(pSkelPose0, VT_ISkeletonPose_GetAbsJointByID, m_lock.rightIkJoint))
            {
                R.ikGame = *pIk;
                R.ikGameValid = SaneQuatT(R.ikGame, 50.0f);
            }
    }

    const float ab = (m_lock.charMatches || !m_lock.active) ? GetLockBlend() : 0.0f;
    if (ab <= 0.0f)
    {
        // Not aiming: this is the exact hip pose of the weapon relative to the camera.
        R.hipRelCam = R.weaponRelCam;
        R.hipValid = true;
        R.residualPos = 0.0f;
        R.residualRot = 0.0f;
    }

    // --- Desired transform ------------------------------------------------------------------------
    QuatT desiredModel = gameModel;
    bool changed = false;
    if (ab > 0.0f && s.aimRenderLock)
    {
        const WeaponSettings& w = GetCurrentWeapon();
        const QuatT aimRelCam = ComputeAimExtra() * w.aim.AsQuatT() * ComputeAimLocal() * m_lock.kick;
        QuatT desiredRelCam = BlendQuatT(R.weaponRelCam, aimRelCam, ab);
        desiredRelCam.q.Normalize();
        desiredModel = camModel * desiredRelCam;
        changed = true;
    }
    if (Active() && s.testOffsetUp != 0.0f)
    {
        desiredModel.t += (camModel.q * Vec3(0.0f, 0.0f, s.testOffsetUp));
        changed = true;
    }
    if (!changed)
        return;
    desiredModel.q = SafeNormalized(desiredModel.q);
    if (!SaneQuatT(desiredModel, 50.0f) || !((desiredModel.t - gameModel.t).GetLengthSquared() < 2.0f * 2.0f))
    {
        // Our own numbers went bad (or absurd): leave the game's pose for this frame and restart the
        // lock's accumulated state so the next frame begins from clean values.
        m_lock.addValid = false;
        m_lock.animRestValid = false;
        m_lock.kick = QuatT(IDENTITY);
        R.bobValid = false;
        m_nanRecoveries++;
        return;
    }
    R.desiredModel = desiredModel;

    // Rigid delta that takes the game's weapon transform to ours (model space).
    QuatT D = desiredModel * gameModel.GetInverted();
    D.q = SafeNormalized(D.q);
    R.residualPos = D.t.GetLength();
    {
        Quat dq = desiredModel.q * (!gameModel.q);
        dq.Normalize();
        R.residualRot = 2.0f * acosf(clamp_tpl(fabsf(dq.w), 0.0f, 1.0f));
    }
    if (ab > 0.0f && s.aimRenderLock)
    {
        R.residualPosAcc = max(R.residualPosAcc, R.residualPos);
        R.residualTimer += dt;
        if (R.residualTimer > 1.0f)
        {
            R.residualPosMax1s = R.residualPosAcc;
            R.residualPosAcc = 0.0f;
            R.residualTimer = 0.0f;
        }
    }

    *pModelRel = desiredModel;

    // --- Hands: move the final hand poses by the same rigid delta so they stay on the grip. ----------
    const bool wantHands = (ab > 0.0f && s.aimRenderLock) ? (s.aimHandsFollow != 0) : (s.testHands != 0);
    if (!wantHands)
        return;
    void* pSkelPose = VCall<void*>(pCharInst, VT_ICharacterInstance_GetISkeletonPose);
    if (!pSkelPose)
        return;
    auto jointRef = [&](int id) -> QuatT* {
        return const_cast<QuatT*>(VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, id));
    };
    for (int j : R.rightSubtree)
        if (QuatT* p = jointRef(j))
            if (SaneQuatT(*p, 50.0f))
                *p = D * (*p);

    R.leftOnWeapon = false;
    if (R.leftHand >= 0 && (s.aimLeftHandFollow || ab <= 0.0f))
    {
        if (QuatT* pl = jointRef(R.leftHand))
        {
            R.leftHandDist = Finite(pl->t) ? (pl->t - gameModel.t).GetLength() : 1e9f;
            R.leftOnWeapon = R.leftHandDist < 0.35f;
        }
        if (R.leftOnWeapon)
            for (int j : R.leftSubtree)
                if (QuatT* p = jointRef(j))
                    if (SaneQuatT(*p, 50.0f))
                        *p = D * (*p);
    }
}

//---------------------------------------------------------------------------------
// Interaction reach (support hand)
//---------------------------------------------------------------------------------
// The first-person arms are driven by CryEngine's animation-driven IK: the procedural weapon context pushes
// its offsets onto two target joints ("r_hand_spine_target" / "l_hand_spine_target", resolved by name in
// CProceduralWeaponAnimationContext::Initialize, 0x17D5B60) and the limb IK brings each hand to its target.
// So an additive push on the LEFT target joint - the same eOp_Additive the game uses - moves the support hand
// alone, with the arm solved by the rig. The reach is a pure tween on top of the live animated hand:
//     hand = animated + (target - animated) * curve(t) + arc(t)
// so it inherits the sway / bob and returns exactly where the animation is when it ends.
static float EaseCurve(int kind, float u)
{
    u = clamp_tpl(u, 0.0f, 1.0f);
    switch (kind)
    {
    case 0: return u;                                   // linear
    case 1: return SmoothStep01(u);                     // smooth
    case 2: return 1.0f - (1.0f - u) * (1.0f - u);      // ease out (fast start)
    case 3: return u * u;                               // ease in (slow start)
    default:                                            // ease in-out (cubic)
        return u < 0.5f ? 4.0f * u * u * u : 1.0f - powf(-2.0f * u + 2.0f, 3.0f) * 0.5f;
    }
}

static int StyleForInteraction(int type, int mode)
{
    if (mode == (int)EArkInteractionMode::loot)
        return 1;
    switch (type)
    {
    case (int)EArkInteractionType::pickup:
    case (int)EArkInteractionType::consume:
    case (int)EArkInteractionType::carry:
    case (int)EArkInteractionType::equip:
    case (int)EArkInteractionType::examine:
        return 1;   // grab
    default:
        return 0;   // press
    }
}

static const char* InteractionTypeName(int type)
{
    static const char* names[] = { "none", "scriptDefined", "unavailable", "codeDefined", "pickup", "consume", "carry",
        "hack", "repair", "fortify", "examine", "equip", "hoover" };
    return (type >= 0 && type < 13) ? names[type] : "?";
}

static const char* InteractionModeName(int mode)
{
    static const char* names[] = { "use", "holdUse", "loot", "special", "remote" };
    return (mode >= 0 && mode < 5) ? names[mode] : "?";
}

static bool ContainsNoCase(const char* hay, const std::string& needle)
{
    if (needle.empty()) return true;
    if (!hay) return false;
    std::string h(hay), n(needle);
    for (char& c : h) c = (char)tolower((unsigned char)c);
    for (char& c : n) c = (char)tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

int ModMain::ResolveStyle(int type, int mode, const char* className, const char* text, bool* pHover) const
{
    if (pHover) *pHover = true;
    for (const InteractRule& r : m_rules)
    {
        if (r.type >= 0 && r.type != type) continue;
        if (r.mode >= 0 && r.mode != mode) continue;
        if (!ContainsNoCase(className, r.classContains)) continue;
        if (!ContainsNoCase(text, r.textContains)) continue;
        if (pHover) *pHover = r.style != 2 && r.hover != 0;
        return clamp_tpl(r.style, 0, 2);
    }
    return StyleForInteraction(type, mode);
}

void ModMain::SeedDefaultRules()
{
    // What testing found the built-in choice gets wrong. Entity class names from ArkEntityClassLibrary.
    auto add = [&](int type, int mode, const char* cls, const char* text, int style, int hover, const char* note) {
        InteractRule r; r.type = type; r.mode = mode; r.classContains = cls; r.textContains = text; r.style = style; r.hover = hover; r.note = note;
        m_rules.push_back(r);
    };
    add(-1, (int)EArkInteractionMode::use, "ArkHuman", "@use_npc", 2, 0, "talking to someone: no animation, no hovering hand");
    add(-1, (int)EArkInteractionMode::use, "ArkOperator", "@use_npc", 2, 0, "talking to an operator");
    add(-1, -1, "ArkHuman", "", 1, 1, "searching a body");
    add(-1, (int)EArkInteractionMode::use, "ArkBook", "", 1, 1, "reading a book");
    add(-1, -1, "Container", "", 1, 1, "containers (ArkContainer, ArkCargoContainer, ...)");
    add(-1, -1, "ArkHarvestable", "", 1, 1, "harvesting");
    add(-1, (int)EArkInteractionMode::holdUse, "ArkWeapon", "", 1, 1, "taking the ammo out of a dropped weapon (hold)");
}

// PerformInteraction(carry, mode, entity, delay) is the point where a carry really begins. With delay > 0 the
// game arms m_carryDelay and StartCarrying follows from ArkPlayerInteraction::Update when it runs out (with the
// target selector's entity of that moment - null-guarded in our StartCarrying hook); with delay <= 0 it picks
// the object up right here. Stretching the delay to the reach puts the pickup at the apex of the grab, with no
// state of ours to go stale.
static auto s_hookPerformInteraction = ArkPlayerInteraction::FPerformInteraction.MakeHook();
static void ArkPlayerInteraction_PerformInteraction_Hook(ArkPlayerInteraction* const _this, EArkInteractionType _interaction, EArkInteractionMode _mode, IEntity* const _pEntity, float _delay)
{
    float delay = _delay;
    if (gMod && _interaction == EArkInteractionType::carry)
        delay = gMod->OnPerformCarry(_this, (int)_mode, _pEntity, _delay);
    s_hookPerformInteraction.InvokeOrig(_this, _interaction, _mode, _pEntity, delay);
}

float ModMain::OnPerformCarry(void* pInteractionRaw, int mode, IEntity* pEntity, float delay)
{
    InteractState& I = m_interact;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || pInteractionRaw != &pPlayer->m_interaction || !pEntity || mode == (int)EArkInteractionMode::remoteManipulation)
        return delay;
    const int type = (int)EArkInteractionType::carry;
    I.lastType = type;
    I.lastMode = mode;
    I.lastClass = (pEntity->GetClass() && pEntity->GetClass()->GetName()) ? pEntity->GetClass()->GetName() : "?";
    I.lastText.clear();
    I.lastEntity = I.lastClass + (pEntity->GetName() ? std::string(" '") + pEntity->GetName() + "'" : std::string()) + " (carry)";
    if (!InteractAnimAllowed(type, mode))
    {
        I.skipped++;
        return delay;
    }
    const int style = ResolveStyle(type, mode, I.lastClass.c_str(), nullptr);
    if (style == 2)
    {
        I.skipReason = "rule: no animation for this";
        I.skipped++;
        return delay;
    }
    if (!m_settings.interactCarryAtApex)
    {
        Vec3 world;
        const bool haveTarget = m_settings.interactTargetMode != 2 && FindInteractPoint(pEntity, world);
        StartReach(style, haveTarget ? &world : nullptr);
        return delay;
    }
    // The grab starts once the key has been held for the game's own delay (heavy objects: the hold-to-lift
    // time; light ones: 0) or at least interactCarryHoldTime - so a tap shows nothing - and the game picks the
    // object up when the grab is at its apex: its carry timer gets hold + reach. Releasing the key invalidates
    // that timer (StopHoldToUseInteract), which UpdateInteract watches to call the grab off.
    const float hold = max(max(delay, 0.0f), clamp_tpl(m_settings.interactCarryHoldTime, 0.0f, 2.0f));
    const ReachStyle& st = style == 1 ? m_settings.grab : (style == 2 ? m_settings.punch : m_settings.press);
    const float apex = clamp_tpl(st.ApexTime(), 0.0f, 1.5f);
    I.carryStyle = style;
    I.carryPending = true;
    I.carryStartIn = hold;
    I.carryEntityId = pEntity->GetId();
    I.carryAnimating = false;
    I.carryStarted = false;
    I.deferred++;
    return hold + apex;
}

void ModMain::OnCarryStarted()
{
    m_interact.carryStarted = true;
    m_interact.carryPending = false;
}

void ModMain::UpdateCarry(float dt)
{
    InteractState& I = m_interact;
    if (!I.carryPending && !I.carryAnimating)
        return;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !Active() || !m_settings.interactEnabled)
    {
        I.carryPending = I.carryAnimating = false;
        return;
    }
    // The game's carry timer: invalid (< 0) once the key was released (or the carry went through: the flag says which).
    const float remaining = pPlayer->m_interaction.m_carryDelay.m_timeRemaining;
    const bool timerLive = Finite(remaining) && remaining >= 0.0f;
    if (I.carryPending)
    {
        if (!timerLive && !I.carryStarted)
        {
            I.carryPending = false; // a tap: nothing to see
            I.carryCancelled++;
            return;
        }
        I.carryStartIn -= dt;
        if (I.carryStartIn > 0.0f)
            return;
        I.carryPending = false;
        IEntity* pEnt = (gEnv && gEnv->pEntitySystem && I.carryEntityId) ? gEnv->pEntitySystem->GetEntity(I.carryEntityId) : nullptr;
        if (!pEnt)
            return; // gone meanwhile
        Vec3 world;
        const bool haveTarget = m_settings.interactTargetMode != 2 && FindInteractPoint(pEnt, world);
        StartReach(I.carryStyle, haveTarget ? &world : nullptr);
        I.carryAnimating = true;
        return;
    }
    // Animating: the key let go before the object was picked up -> straight into the return.
    if (!timerLive && !I.carryStarted && I.phase != InteractState::Idle && !I.returning)
    {
        const ReachStyle& st = CurStyle();
        I.time = st.ApexTime() + max(st.holdTime, 0.0f) + max(st.returnTime, 0.0f) * (1.0f - clamp_tpl(I.curve, 0.0f, 1.0f));
        I.wind = 0.0f;
        I.carryCancelled++;
        I.carryAnimating = false;
        return;
    }
    if (I.phase == InteractState::Idle || I.carryStarted)
        I.carryAnimating = false;
}

// The wrench hit's physics impulse (ArkWeaponUtils::DoWeaponImpulse: direction from the swipe, force from the
// weapon's stat, per-entity scale). Only touched while our own OnHit call is on the stack: with the punch, objects
// were pulled toward the player instead of away, so the direction can be flipped and the force scaled.
static auto s_hookWeaponImpulse = ArkWeaponUtils::FDoWeaponImpulseOv0.MakeHook();
static void ArkWeaponUtils_DoWeaponImpulse_Hook(IEntity* const _pHitEntity, IPhysicalEntity* const _pHitPhysics, Vec3 _hitDirection, const int _partid, CArkWeapon const* const _pWeapon, const float _impulseScale, const float _minMassScale, const float _maxMassScale)
{
    Vec3 dir = _hitDirection;
    float scale = _impulseScale;
    if (gMod && gMod->MeleeHitInProgress())
    {
        const ViewmodelSettings& s = gMod->GetSettings();
        if (s.interactDebugMarker && gEnv && gEnv->pSystem)
        {
            const Vec3 fwd = gEnv->pSystem->GetViewCamera().GetMatrix().GetColumn1();
            CryLog("ViewmodelTweaks: quick melee impulse - direction ({:.2f} {:.2f} {:.2f}) vs view forward ({:.2f} {:.2f} {:.2f}), scale {:.2f}, mass scale {:.2f}..{:.2f}, entity {}",
                dir.x, dir.y, dir.z, fwd.x, fwd.y, fwd.z, scale, _minMassScale, _maxMassScale, (_pHitEntity && _pHitEntity->GetName()) ? _pHitEntity->GetName() : "?");
        }
        if (s.meleeImpulseFlip)
            dir = -dir;
        scale *= clamp_tpl(s.meleeImpulseScale, 0.0f, 10.0f);
    }
    s_hookWeaponImpulse.InvokeOrig(_pHitEntity, _pHitPhysics, dir, _partid, _pWeapon, scale, _minMassScale, _maxMassScale);
}

bool ModMain::SupportHandOffWeapon() const
{
    const InteractState& I = m_interact;
    if (I.unarmed)
        return true;
    const WeaponSettings* pW = InteractWeaponEntry();
    int mode = pW ? pW->interactHandOff : 2;
    if (mode == 2)
    {
        // auto: what is known about the weapon first (the built-in table says which are one-handed) ...
        if (const WeaponSettings* pB = WeaponSettings::BuiltIn(m_currentWeaponClass.c_str()))
            mode = pB->interactHandOff;
    }
    if (mode == 0) return false;
    if (mode == 1) return true;
    // ... else: the left IK weight is animated at 0, or the animated hand is nowhere near the view
    if (I.animIkWeightValid && I.animIkWeight < 0.5f)
        return true;
    const Vec3& h = I.handView;
    return Finite(h) && (h.y < 0.05f || h.z < -0.45f || fabsf(h.x) > 0.5f);
}

void ModMain::StartMelee()
{
    InteractState& I = m_interact;
    const ViewmodelSettings& s = m_settings;
    if (!Active() || !s.interactEnabled || !s.meleeEnabled || m_playerDead || !m_offsetHookActive)
        return;
    if (I.meleeCooldownLeft > 0.0f || I.meleePending || I.holdMode != 0)
        return;
    if (m_currentWeaponClass.empty() && !s.interactUnarmed)
        return;
    if (!s.meleeWhileAiming && s.aimEnabled && m_aimBlend > 0.3f)
        return;
    if (I.examining || m_wsReloading || m_wsSwitching || m_wsDrawing || m_wsUnequipping)
        return;
    StartReach(2, nullptr);
    I.meleePending = true;
    I.meleeCooldownLeft = max(s.meleeCooldown, 0.0f);
    I.meleeKickTime = 0.0f;
    I.meleePunches++;
    if (s.meleeSound && gEnv && gEnv->pConsole)
    {
        if (ICVar* pName = gEnv->pConsole->GetCVar("vm_melee_sound_name"))
        {
            const char* name = pName->GetString();
            ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
            if (name && *name && pPlayer && pPlayer->GetEntity())
            {
                ArkAudioTrigger trig;
                if (trig.Load(name))
                    trig.Execute(pPlayer->GetEntity());
                else if (!I.meleeSoundWarned)
                {
                    CryLog("ViewmodelTweaks: quick melee - audio trigger '{}' not found", name);
                    I.meleeSoundWarned = true;
                }
            }
        }
    }
}

void ModMain::DoMeleeHit()
{
    // The wrench's own hit (ArkWrenchComponent::OnHit: the player's view ray, the wrench's range and damage
    // stats, the damage signal, NPC reactions, fatigue), scaled - from the wrench in the inventory, equipped or
    // not (the hit is computed from the player, not from the weapon's position).
    InteractState& I = m_interact;
    I.meleePending = false;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
        return;
    unsigned id = pPlayer->m_weaponComponent.FindWeapon(ArkWrenchComponent::GetWrenchArchetypeId());
    if (!id)
        id = pPlayer->m_weaponComponent.FindWeapon(ArkWrenchComponent::GetDoubleWrenchArchetypeId());
    CArkWeapon* pW = id ? CArkWeapon::GetWeaponFromEntityId(id) : nullptr;
    IEntity* pEnt = pW ? pW->GetEntity() : nullptr;
    const char* cls = (pEnt && pEnt->GetClass()) ? pEnt->GetClass()->GetName() : nullptr;
    if (!pW || !cls || strncmp(cls, "ArkWeaponWrench", 15) != 0 && strncmp(cls, "ArkWeaponDoubleWrench", 21) != 0)
    {
        I.meleeNoWrench++;
        if (m_settings.interactDebugMarker)
            CryLog("ViewmodelTweaks: quick melee - no wrench in the inventory, the punch lands nothing");
        return;
    }
    ArkWeaponWrench* pWrench = static_cast<ArkWeaponWrench*>(pW);
    const float scale = clamp_tpl(m_settings.meleeDamage, 0.0f, 10.0f);
    I.meleeHitInProgress = true;
    const ArkWrenchComponent::hitResult r = pWrench->m_wrenchComponent.OnHit(0.0f, *pW, scale, false);
    I.meleeHitInProgress = false;
    if (r != ArkWrenchComponent::hitResult::none)
        I.meleeHits++;
    if (m_settings.interactDebugMarker)
        CryLog("ViewmodelTweaks: quick melee hit - result {} (0 none, 1 hit, 2 enemy), damage scale {:.2f}", (int)r, scale);
}

bool ModMain::InteractAnimAllowed(int type, int mode)
{
    InteractState& I = m_interact;
    const ViewmodelSettings& s = m_settings;
    I.skipReason.clear();
    if (!Active() || !s.interactEnabled) { I.skipReason = "disabled"; return false; }
    if (!m_offsetHookActive) { I.skipReason = "offset hooks not installed"; return false; }
    if (m_currentWeaponClass.empty() && !s.interactUnarmed) { I.skipReason = "no weapon out (enable 'also with no weapon')"; return false; }
    if (type < 0 || type >= 13 || !((s.interactTypeMask >> type) & 1)) { I.skipReason = "interaction type not enabled"; return false; }
    if (mode == (int)EArkInteractionMode::remoteManipulation && !s.interactRemoteMode) { I.skipReason = "remote manipulation"; return false; }
    const bool exam = ExaminingWorldUI();
    if (!exam && !s.interactWhileAiming && s.aimEnabled && m_aimBlend > 0.3f) { I.skipReason = "aiming"; return false; }
    if (!exam && m_wsReloading) { I.skipReason = "reloading"; return false; }
    if (!exam && (m_wsSwitching || m_wsDrawing || m_wsUnequipping)) { I.skipReason = "weapon switch"; return false; }
    if (exam && !s.interactExamination) { I.skipReason = "screen (examination mode) - disabled"; return false; }
    if (m_playerDead) { I.skipReason = "dead"; return false; }
    return true;
}

bool ModMain::FindInteractPoint(IEntity* pEntity, Vec3& outWorld) const
{
    if (!pEntity || !gEnv || !gEnv->pSystem)
        return false;
    const ViewmodelSettings& s = m_settings;
    AABB box(AABB::RESET);
    CEntity::FGetWorldBounds(static_cast<const CEntity*>(pEntity), box);
    const bool boxOk = Finite(box.min) && Finite(box.max) && box.min.x <= box.max.x && !box.IsEmpty()
        && (box.max - box.min).GetLengthSquared() < 50.0f * 50.0f;
    const Vec3 centre = boxOk ? box.GetCenter() : pEntity->GetWorldPos();
    if (!Finite(centre))
        return false;

    if (s.interactTargetMode == 0 && gEnv->pPhysicalWorld)
    {
        // Where the crosshair ray hits the object: a button's face, the side of a crate facing us.
        ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
        IPhysicalEntity* pSkip = (pPlayer && pPlayer->GetEntity()) ? pPlayer->GetEntity()->GetPhysics() : nullptr;
        const Matrix34 cam = gEnv->pSystem->GetViewCamera().GetMatrix();
        const Vec3 camPos = cam.GetTranslation();
        const Vec3 dir = cam.GetColumn1().GetNormalized();
        PaddedRayHit hit;
        const int n = gEnv->pPhysicalWorld->RayWorldIntersection(camPos, dir * 4.0f, ent_all,
            rwi_stop_at_pierceable | rwi_colltype_any(geom_colltype_ray | geom_colltype0 | geom_colltype_player), &hit, 1, pSkip);
        if (n > 0 && Finite(hit.pt) && Finite(hit.dist) && hit.dist > 0.0f)
        {
            // Accept the hit if it is on (or right next to) the object; otherwise something is in the way.
            const bool onObject = boxOk ? (box.GetDistance(hit.pt) < 0.25f) : ((hit.pt - centre).GetLength() < 1.5f);
            if (onObject)
            {
                outWorld = hit.pt;
                return true;
            }
        }
    }
    outWorld = centre;
    return true;
}

bool ModMain::OnInteract(void* pInteractionRaw, int mode)
{
    InteractState& I = m_interact;
    if (m_interactReentry || !pInteractionRaw)
        return false;
    ArkPlayerInteraction* pInteraction = static_cast<ArkPlayerInteraction*>(pInteractionRaw);
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || pInteraction != &pPlayer->m_interaction)
        return false; // not the player's (or the player is gone): leave the game alone

    const ArkInteractionInfo* pInfo = (mode >= 0 && mode < 4) ? &pInteraction->m_interactionInfo[(size_t)mode] : nullptr;
    const int type = pInfo ? (int)pInfo->m_interactionType : -1;
    I.lastType = type;
    I.lastMode = mode;
    IEntity* pEntity = (gEnv && gEnv->pEntitySystem && pInteraction->m_usableEntityId) ? gEnv->pEntitySystem->GetEntity(pInteraction->m_usableEntityId) : nullptr;
    I.lastClass = pEntity ? (pEntity->GetClass() && pEntity->GetClass()->GetName() ? pEntity->GetClass()->GetName() : "?") : "";
    I.lastText = (pInfo && pInfo->m_displayText.c_str()) ? pInfo->m_displayText.c_str() : "";
    I.lastEntity = pEntity ? I.lastClass : "(none)";
    if (pEntity && pEntity->GetName())
        I.lastEntity += std::string(" '") + pEntity->GetName() + "'";
    if (!I.lastText.empty())
        I.lastEntity += " \"" + I.lastText + "\"";

    if (!pEntity || type == (int)EArkInteractionType::none || type == (int)EArkInteractionType::unavailable)
    {
        I.skipReason = pEntity ? "nothing to do (type none/unavailable)" : "no target entity";
        I.skipped++;
        return false;
    }
    if (!InteractAnimAllowed(type, mode))
    {
        I.skipped++;
        return false;
    }
    const int style = ResolveStyle(type, mode, I.lastClass.c_str(), I.lastText.c_str());
    if (style == 2)
    {
        I.skipReason = "rule: no animation for this";
        I.skipped++;
        return false;
    }
    // Carrying is animated from PerformInteraction(carry) instead (see OnPerformCarry): that is the call that only
    // happens when the carry really starts - a short press on something that needs a hold does not get there -
    // and it carries the delay the game itself waits before picking the object up.
    if (type == (int)EArkInteractionType::carry)
    {
        I.skipReason = "carry: animated when the carry starts";
        return false;
    }

    Vec3 world;
    const bool haveTarget = m_settings.interactTargetMode != 2 && FindInteractPoint(pEntity, world);
    StartReach(style, haveTarget ? &world : nullptr);

    // Defer the game's side so the object reacts when the hand gets there, not before: after a fixed delay, or -
    // for grabs - when the reach is at its apex (the hand closest to the object).
    const bool apex = style == 1 && m_settings.interactGrabAtApex;
    const float fireDelay = apex ? CurStyle().ApexTime() : m_settings.interactFireDelay;
    if (m_settings.interactDefer && fireDelay > 0.0f)
    {
        if (I.pending)
        {
            // A second press before the first fired. Same target: it is one interaction, swallow the repeat
            // (the stored call covers it). Another target: the new press replaces the old one.
            if (I.entityId == pInteraction->m_usableEntityId && I.mode == mode)
                return true;
            I.dropped++;
        }
        I.pending = true;
        I.pInteraction = pInteraction;
        I.mode = mode;
        I.entityId = pInteraction->m_usableEntityId;
        I.fireIn = clamp_tpl(fireDelay, 0.0f, 1.0f);
        I.fireNow = false;
        I.deferred++;
        return true;
    }
    return false;
}

void ModMain::StartReach(int style, const Vec3* pWorld)
{
    InteractState& I = m_interact;
    I.style = style;
    const ReachStyle& st = CurStyle();
    // Restarting mid-animation: keep the hand where it is (no pop) by starting the new reach from the
    // current progress; a linear guess is plenty for a couple of frames of difference.
    float startTime = 0.0f;
    if (I.phase != InteractState::Idle && I.curve > 0.0f)
        startTime = max(st.windupTime, 0.0f) + clamp_tpl(I.curve, 0.0f, 1.0f) * max(st.reachTime, 0.0f);
    I.phase = st.windupTime > 0.0f && startTime <= 0.0f ? InteractState::Windup : InteractState::Reach;
    I.time = startTime;
    I.wind = 0.0f;
    I.returning = false;
    I.hasWorldTarget = pWorld != nullptr && Finite(*pWorld);
    if (I.hasWorldTarget)
        I.targetWorld = *pWorld;
    I.started++;
}

void ModMain::UpdateHover()
{
    InteractState& I = m_interact;
    const ViewmodelSettings& s = m_settings;
    I.hoverActive = false;
    I.hoverTargetValid = false;
    I.hoverType = -1;
    if (!s.interactHoverRest || I.examining || !Active() || !s.interactEnabled || m_playerDead || !m_offsetHookActive)
        return;
    if (m_currentWeaponClass.empty() ? !s.interactHoverUnarmed : !s.interactHoverWeapon)
        return;
    if (!s.interactHoverWhileAiming && s.aimEnabled && m_aimBlend > 0.05f)
        return; // aiming down sights: the support hand stays on the gun
    if (m_wsReloading || m_wsSwitching || m_wsDrawing || m_wsUnequipping)
        return;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !gEnv || !gEnv->pEntitySystem || !gEnv->pSystem)
        return;
    const ArkPlayerInteraction& in = pPlayer->m_interaction;
    if (!in.m_usableEntityId)
        return; // nothing usable under the crosshair (this is what the game shows its prompt for)
    IEntity* pEnt = gEnv->pEntitySystem->GetEntity(in.m_usableEntityId);
    if (!pEnt)
        return;
    const char* cls = (pEnt->GetClass() && pEnt->GetClass()->GetName()) ? pEnt->GetClass()->GetName() : "";
    // Any of the interaction modes the thing offers with a type we hold the hand up for - and no rule against it.
    int type = -1;
    for (size_t m = 0; m < 4 && type < 0; m++)
    {
        const int t = (int)in.m_interactionInfo[m].m_interactionType;
        if (t <= (int)EArkInteractionType::none || t >= 13 || t == (int)EArkInteractionType::unavailable) continue;
        if (!((s.interactHoverTypeMask >> t) & 1)) continue;
        bool hover = true;
        const char* text = in.m_interactionInfo[m].m_displayText.c_str();
        if (ResolveStyle(t, (int)m, cls, text, &hover) == 2 || !hover) continue;
        type = t;
    }
    if (type < 0)
        return;
    Vec3 world;
    if (!FindInteractPoint(pEnt, world))
        return;
    const Vec3 camPos = gEnv->pSystem->GetViewCamera().GetPosition();
    const float dist = Finite(camPos) ? (world - camPos).GetLength() : 1e9f;
    I.hoverType = type;
    I.hoverDist = dist;
    if (dist > clamp_tpl(s.interactHoverMaxDist, 0.2f, 5.0f))
        return;
    I.hoverActive = true;
    I.hoverTargetValid = true;
    I.hoverWorld = world;
}

void ModMain::ApplyExamineCVars()
{
    // The game decides per screen type whether pressing "use" zooms you in (ArkWorldUIManager::ShouldAutoExamineType
    // reads ui_examine_<type>; kiosks, keycard readers and dispensers never do, and their buttons take the press at
    // the crosshair). Our switches write those cvars; only on change, so console edits are left alone otherwise.
    if (!gEnv || !gEnv->pConsole)
        return;
    const ViewmodelSettings& s = m_settings;
    const char* names[4] = { "ui_examine_keypad", "ui_examine_fabricator", "ui_examine_securitystation", "ui_examine_workstation" };
    const int noZoom[4] = { s.interactNoZoomKeypad, s.interactNoZoomFabricator, s.interactNoZoomSecurity, s.interactNoZoomWorkstation };
    for (int i = 0; i < 4; i++)
    {
        const int want = (Active() && s.interactEnabled && noZoom[i]) ? 0 : 1; // 1 = the game's default for all four
        ICVar* pVar = gEnv->pConsole->GetCVar(names[i]);
        if (!pVar)
            continue;
        // The game (re)registers these with their defaults on level load, which undid an earlier write (3.10.2:
        // "needs to be toggled after every start"). So: whenever the live value is not what we want, write it.
        if (pVar->GetIVal() != want)
        {
            pVar->Set(want);
            m_uiExamineApplied[i] = want;
        }
    }
}

void ModMain::UpdateInteract(float dt)
{
    InteractState& I = m_interact;
    const ViewmodelSettings& s = m_settings;

    if (I.holdMode != 0)
    {
        // Posing mode: freeze the animation fully "in" so the pose can be edited live.
        I.phase = InteractState::Hold;
        I.curve = 1.0f;
        I.arc = 0.0f;
        I.returning = false;
        I.hasWorldTarget = false;
        // Use the style the edited pose is assigned to, so what is shown here is what a real interaction ends in.
        if (m_editPose >= 0 && m_editPose < (int)m_poses.size())
        {
            const std::string& nm = m_poses[(size_t)m_editPose].name;
            I.style = (m_stylePose[1] == nm && m_stylePose[0] != nm) ? 1 : ((m_stylePose[3] == nm && m_stylePose[0] != nm) ? 2 : 0);
        }
        const ReachStyle& st = CurStyle();
        I.wind = 0.0f;
        I.time = st.ApexTime() + max(st.holdTime, 0.0f); // leaving posing mode continues with the return
        if (!Active() || (m_currentWeaponClass.empty() && !s.interactUnarmed) || m_playerDead)
            I.holdMode = 0;
    }
    else if (I.phase != InteractState::Idle)
    {
        const ReachStyle& st = CurStyle();
        const float W = max(st.windupTime, 0.0f), R = max(st.reachTime, 0.0f), H = max(st.holdTime, 0.0f), T = max(st.returnTime, 0.0f);
        I.time += dt;
        if (I.time < W)
        {
            // Windup: the hand is pulled to the style's windup offset first (same easing as the reach), curve stays 0.
            I.phase = InteractState::Windup;
            const float u = I.time / W;
            I.wind = EaseCurve(s.interactEaseIn, u);
            I.curve = 0.0f;
            I.arc = 0.0f;
            I.returning = false;
        }
        else if (I.time < W + R)
        {
            I.phase = InteractState::Reach;
            const float u = R > 0.0f ? (I.time - W) / R : 1.0f;
            I.curve = EaseCurve(s.interactEaseIn, u);
            I.wind = W > 0.0f ? 1.0f - I.curve : 0.0f; // the pulled-back offset unwinds as the hand goes out
            I.arc = sinf(gf_PI * clamp_tpl(u, 0.0f, 1.0f));
            I.returning = false;
        }
        else if (I.time < W + R + H)
        {
            if (I.phase != InteractState::Hold && I.meleePending)
                DoMeleeHit(); // the punch lands
            if (I.phase != InteractState::Hold && s.interactDebugMarker)
                CryLog("ViewmodelTweaks: reach at target - view target ({:.2f} {:.2f} {:.2f}){} asked ({:.2f} {:.2f} {:.2f}) anim hand ({:.2f} {:.2f} {:.2f}) corr ({:.2f} {:.2f} {:.2f}) rest {:.2f} examining {} shift ({:.2f} {:.2f} {:.2f}) poseAbsPos {} hasWorldTarget {}",
                    I.targetView.x, I.targetView.y, I.targetView.z, I.clamped ? " CLAMPED" : "", I.desiredView.x, I.desiredView.y, I.desiredView.z,
                    I.handView.x, I.handView.y, I.handView.z, I.corr.x, I.corr.y, I.corr.z, I.restBlend, I.examining, I.bodyShift.x, I.bodyShift.y, I.bodyShift.z,
                    I.poseAbsolutePos, I.hasWorldTarget);
            I.phase = InteractState::Hold;
            I.curve = 1.0f;
            I.wind = 0.0f;
            I.arc = 0.0f;
            I.returning = false;
        }
        else if (I.time < W + R + H + T)
        {
            I.phase = InteractState::Return;
            I.wind = 0.0f;
            const float u = T > 0.0f ? (I.time - W - R - H) / T : 1.0f;
            I.curve = 1.0f - EaseCurve(s.interactEaseOut, u);
            I.arc = sinf(gf_PI * clamp_tpl(u, 0.0f, 1.0f));
            I.returning = true;
        }
        else
        {
            I.phase = InteractState::Idle;
            I.curve = 0.0f;
            I.wind = 0.0f;
            I.arc = 0.0f;
            I.meleePending = false;
        }
        // The animation must not outlive the conditions it started under.
        if (!Active() || !s.interactEnabled || (m_currentWeaponClass.empty() && !s.interactUnarmed) || m_playerDead)
        {
            I.phase = InteractState::Idle;
            I.curve = 0.0f;
            I.wind = 0.0f;
            I.arc = 0.0f;
            I.meleePending = false;
        }
    }

    if (I.pending)
    {
        I.fireIn -= dt;
        if (I.fireIn <= 0.0f)
            I.fireNow = true; // made in UpdateBeforeSystem, inside the game's own update window
    }
    UpdateCarry(dt);
    if (I.meleeCooldownLeft > 0.0f) I.meleeCooldownLeft = max(I.meleeCooldownLeft - dt, 0.0f);
    if (I.meleeKickTime >= 0.0f)
    {
        I.meleeKickTime += dt;
        if (I.meleeKickTime > max(s.meleeCamKickTime, 0.01f)) I.meleeKickTime = -1.0f;
    }
    // The equipped weapon's lowering: in from the windup, out from the return.
    {
        const bool lower = I.style == 2 && (I.phase == InteractState::Windup || I.phase == InteractState::Reach || I.phase == InteractState::Hold) && I.holdMode == 0;
        MoveTowards(I.meleeLowerBlend, lower ? 1.0f : 0.0f, max(s.meleeLowerTime, 0.01f), dt);
    }

    I.unarmed = m_currentWeaponClass.empty();
    I.examining = ExaminingWorldUI();
    I.frameDt = dt;
    if (I.weaponSeen != m_currentWeaponClass)
    {
        I.weaponSeen = m_currentWeaponClass;
        I.animIkWeightValid = false; // another weapon animates the IK weight differently: re-capture it
    }
    // Everything screen-specific (body shift, screen corrections, the longer reach) follows this instead of the
    // raw flag: in quickly, and out over interactExamLeaveTime, so leaving a screen does not snap the arm.
    MoveTowards(I.examBlend, I.examining ? 1.0f : 0.0f, I.examining ? 0.1f : max(s.interactExamLeaveTime, 0.01f), dt);
    UpdateHover();
    // Screens: bring the body forward when the asked wrist is beyond the arm (the arms hang far back from the
    // examination camera). The shoulder is read as seen, i.e. with the current offset; the rest position is that
    // minus the offset; then a small controller keeps shoulder-to-wrist at the arm length.
    if (I.examining && s.interactExamAutoBody && I.shoulderValid && Finite(I.desiredView) && (I.phase != InteractState::Idle || I.restBlend > 0.0f))
    {
        const Vec3 restShoulder = I.shoulderView - Vec3(s.interactExamBodyX, s.interactExamBodyY + I.autoBodyY, s.interactExamBodyZ);
        const float armLen = clamp_tpl(s.interactExamArmLength, 0.3f, 0.8f);
        // distance with the manual offset only, then how much extra forward is needed along the view axis
        const Vec3 fromShoulder = I.desiredView - (restShoulder + Vec3(s.interactExamBodyX, s.interactExamBodyY, s.interactExamBodyZ));
        const float lateral2 = fromShoulder.x * fromShoulder.x + fromShoulder.z * fromShoulder.z;
        float need = 0.0f;
        if (lateral2 < armLen * armLen)
            need = fromShoulder.y - sqrtf(armLen * armLen - lateral2); // forward offset that puts the wrist exactly at arm length
        else
            need = fromShoulder.y; // cannot be reached sideways anyway: at least line up
        need = clamp_tpl(need, 0.0f, 0.6f);
        MoveTowards(I.autoBodyY, need, 0.5f, dt); // ~0.5 s for the full range, no pops
    }
    else if (!I.examining)
        MoveTowards(I.autoBodyY, 0.0f, 0.5f, dt); // no snap on the way out (the whole shift fades with examBlend anyway)
    // On screens: the pointing hand rests in view between clicks instead of vanishing with the arms. Outside them,
    // optionally, while something usable is in front of us (UpdateHover).
    const bool examRest = s.interactExamination && s.interactExamRest && I.examining;
    I.restActive = Active() && s.interactEnabled && !m_playerDead && (examRest || I.hoverActive);
    MoveTowards(I.restBlend, I.restActive ? 1.0f : 0.0f, max(s.interactRestBlendTime, 0.01f), dt);
    if (I.restActive && I.phase == InteractState::Idle)
        I.style = 0; // the resting hand is the pointing (press) one
    // The resting hand's slow drift (a small figure-eight, like the aim sway): fades out during a reach.
    {
        I.swayTime += dt;
        if (I.swayTime > 100000.0f) I.swayTime = 0.0f;
        const float sw = clamp_tpl(I.restBlend, 0.0f, 1.0f) * (1.0f - (I.phase != InteractState::Idle ? clamp_tpl(I.curve, 0.0f, 1.0f) : 0.0f));
        const float f = 2.0f * gf_PI * clamp_tpl(s.interactRestSwayFreq, 0.0f, 3.0f);
        const float t = I.swayTime;
        const float amp = clamp_tpl(s.interactRestSwayPos, 0.0f, 0.1f) * sw;
        I.swayPos = Vec3(sinf(f * t), 0.3f * sinf(0.71f * f * t + 2.1f), 0.6f * sinf(1.37f * f * t + 1.0f)) * amp;
        const float rot = DEG2RAD(clamp_tpl(s.interactRestSwayRot, 0.0f, 30.0f)) * sw;
        I.swayRot = SafeNormalized(Quat::CreateRotationXYZ(Ang3(rot * 0.6f * sinf(1.37f * f * t + 0.5f), rot * 0.4f * sinf(0.9f * f * t + 1.7f), rot * sinf(f * t + 0.3f))));
        if (!Finite(I.swayPos)) I.swayPos = Vec3(ZERO);
        if (!SaneQuat(I.swayRot)) I.swayRot = Quat(IDENTITY);
    }
    UpdateArmsVisibility();
    UpdateExamZoom();
}

void ModMain::FireDeferredInteract()
{
    InteractState& I = m_interact;
    if (!I.pending || !I.fireNow)
        return;
    I.pending = false;
    I.fireNow = false;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || I.pInteraction != &pPlayer->m_interaction)
    {
        I.dropped++;
        return; // player gone (level change) - the call is stale
    }
    ArkPlayerInteraction* pInteraction = static_cast<ArkPlayerInteraction*>(I.pInteraction);
    // The call only makes sense against a live target. Interact() itself reads m_usableEntityId, so what it
    // would act on is whatever the selector holds now: the original object (fine), another one (only if the
    // user allows it), or nothing (drop).
    const unsigned nowId = pInteraction->m_usableEntityId;
    IEntity* pNow = (gEnv && gEnv->pEntitySystem && nowId) ? gEnv->pEntitySystem->GetEntity(nowId) : nullptr;
    if (!pNow || (nowId != I.entityId && m_settings.interactCancelRetarget))
    {
        I.dropped++;
        return;
    }
    if (I.mode >= 0 && I.mode < 4)
    {
        const int typeNow = (int)pInteraction->m_interactionInfo[(size_t)I.mode].m_interactionType;
        if (typeNow == (int)EArkInteractionType::none || typeNow == (int)EArkInteractionType::unavailable)
        {
            I.dropped++;
            return;
        }
    }
    if (m_playerDead || !ArkPlayer::GetInstancePtr())
    {
        I.dropped++;
        return;
    }
    m_interactReentry = true;
    s_hookInteract.InvokeOrig(pInteraction, (EArkInteractionMode)I.mode);
    m_interactReentry = false;
    I.fired++;
}

void ModMain::PushInteractReach(void* pModifier, void* pSkelPose, const QuatT& camAbs)
{
    using namespace PreyInternals;
    InteractState& I = m_interact;
    const RenderLockState& R = m_render;
    const ViewmodelSettings& s = m_settings;
    const int joint = m_lock.leftIkJoint;
    if (!pModifier || !pSkelPose || joint < 0 || !SaneQuatT(camAbs))
    {
        I.addValid = false;
        return;
    }
    const QuatT* pAbs = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, joint);
    if (!pAbs || !SaneQuatT(*pAbs))
    {
        I.addValid = false;
        return;
    }

    // Examination mode: the real camera is away from the head while the arms stay with the body (plus a user /
    // automatic body offset). The whole skeleton is moved over to the camera HERE, in the pose modifier (an
    // additive position on the root joint), so that everything read back next frame - the hand for the closed
    // loop, the IK target for the additive chain - is the moved skeleton (DEVNOTES, "rules" 1-5).
    QuatT camReach = camAbs; // everything in the REAL camera's frame
    Vec3 bodyShift(ZERO);
    // Frames without a reach push: the chain still has to know about the shift (pushed above) or the next reach
    // would start from a read-back that carries it - one frame of the hand off by the whole shift.
    auto noReachThisFrame = [&]() {
        if (bodyShift.GetLengthSquared() > 0.0f)
        {
            I.lastAdd = QuatT(IDENTITY);
            I.lastAdd.t = bodyShift;
            I.lastReach = Vec3(ZERO);
            I.addValid = true;
        }
        else
            I.addValid = false;
    };
    const float examBlend = clamp_tpl(I.examBlend, 0.0f, 1.0f);
    I.bodyShiftActive = false;
    if (examBlend > 0.0f)
    {
        if (ArkPlayer* pP = ArkPlayer::GetInstancePtr())
        {
            const QuatT camBone = pP->GetBoneTransform(BONE_CAMERA);
            const Vec3 bodyOff = camAbs.q * Vec3(s.interactExamBodyX, s.interactExamBodyY + I.autoBodyY, s.interactExamBodyZ);
            const Vec3 shift = (camAbs.t - camBone.t + bodyOff) * examBlend; // fades out over interactExamLeaveTime after the screen
            if (Finite(camBone.t) && Finite(shift) && shift.GetLengthSquared() < 10.0f * 10.0f)
            {
                bodyShift = shift;
                // On the root joint only: a position pushed on a joint travels down to its children, so this moves the
                // whole body once (pushing it on every joint moved the hand once per ancestor).
                VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, 0, OP_ADDITIVE, &bodyShift);
                I.bodyShift = bodyShift;
                I.bodyShiftActive = true;
            }
        }
    }
    // Animated (pre-modifier) target joint of last frame = last frame's final joint minus what we added (the
    // body shift of last frame is part of "what we added": lastAdd carries it). An additive push is applied
    // exactly, so this is not a guess - the only things that can go wrong are a skeleton that did not update
    // (the read-back is last frame's again: keep the reconstruction we have) or something else moving the
    // joint by a lot (animation change, our own push not taken): then the chain is RESET - one frame without
    // the reach, so that the next read-back is the pure animation - instead of picking a hypothesis. The old
    // "applied / skipped" test could pick wrong and then never recover: a wrong baseline produces a wrong push,
    // which produces a read-back that fits the wrong hypothesis again, and the hand flails.
    QuatT anim = *pAbs;
    bool chainReset = false;
    const bool skeletonUpdated = !I.finalPrevValid || (anim.t - I.finalPrev).GetLengthSquared() > 1e-8f;
    I.finalPrev = anim.t;
    I.finalPrevValid = true;
    if (I.addValid)
    {
        if (!skeletonUpdated)
            anim = I.animIk; // nothing new to learn from a stale pose
        else
        {
            anim.t -= I.lastAdd.t;
            anim.q = SafeNormalized((!I.lastAdd.q) * anim.q);
            // the animated IK target of the arms does not move 30 cm in one frame; if it seems to, our model of the
            // chain is wrong: reset it
            if (!I.chainJustReset && (anim.t - I.animIk.t).GetLengthSquared() > 0.3f * 0.3f)
            {
                I.pushesNotApplied++;
                I.chainResets++;
                chainReset = true;
                anim = *pAbs; // best available: only the shift is known to be in there (pushed again below, alone)
                anim.t -= bodyShift;
            }
        }
    }
    I.animIk = anim;
    I.chainJustReset = chainReset; // the frame after a reset has no trustworthy previous animation to compare with
    if (chainReset)
    {
        // For this one frame push only what is known exactly: the shift and last frame's reach (repeated, so the
        // hand does not pop back to the animation for a frame). Next frame's read-back minus that is then the
        // pure animation, and the chain starts clean. The closed loop starts over as well.
        Vec3 repeat = I.lastReach;
        if (!Finite(repeat) || repeat.GetLengthSquared() > 3.0f * 3.0f)
            repeat = Vec3(ZERO);
        if (repeat.GetLengthSquared() > 0.0f)
            VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, joint, OP_ADDITIVE, &repeat);
        I.lastAdd = QuatT(IDENTITY);
        I.lastAdd.t = bodyShift + repeat;
        I.addValid = true;
        I.corr = Vec3(ZERO);
        I.desiredPrevValid = false;
        PushHandPose(pModifier, pSkelPose, camReach, max(clamp_tpl(I.curve, 0.0f, 1.0f), clamp_tpl(I.restBlend, 0.0f, 1.0f)));
        return;
    }

    const float wind = I.phase != InteractState::Idle ? clamp_tpl(I.wind, 0.0f, 1.0f) : 0.0f;
    const bool reaching = I.phase != InteractState::Idle && (I.curve > 0.0f || wind > 0.0f);
    const float curve = reaching ? clamp_tpl(I.curve, 0.0f, 1.0f) : 0.0f;
    const float rest = clamp_tpl(I.restBlend, 0.0f, 1.0f);
    if (!reaching && rest <= 0.0f)
    {
        noReachThisFrame();
        I.corr = Vec3(ZERO);
        I.desiredPrevValid = false;
        I.animIkWeightValid = false; // next frame's read-back of the weight joint is the animation again
        PushHandPose(pModifier, pSkelPose, camAbs, 0.0f);
        return;
    }
    const ReachStyle& st = CurStyle();
    // Fingers / hand orientation: same progress as the reach (posing mode 1 = pose only, the hand stays where
    // the animation has it). While resting on a screen the pose is held at the rest blend.
    PushHandPose(pModifier, pSkelPose, camReach, max(max(curve, wind), rest));
    if (I.holdMode == 1)
    {
        noReachThisFrame();
        return;
    }

    // Everything in view space (camera at the origin: X right, Y forward, Z up).
    // The reach starts from where the IK target WILL be after the body shift (the shift travels down from the root
    // to this joint too). Measuring it before the shift had the push overshoot by the whole shift - 0.4-0.8 m on a
    // screen - which the closed loop then spent itself (saturated) trying to take back.
    const QuatT camInv = camReach.GetInverted();
    const Vec3 handView = camInv * (anim.t + bodyShift);
    Vec3 targetView(s.interactTestX, s.interactTestY, s.interactTestZ);
    if (I.hasWorldTarget)
    {
        // World -> model (entity) space -> view space. The arms live in the player entity's space.
        ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
        IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
        if (pEnt)
        {
            const Quat entRot = SafeNormalized(pEnt->GetWorldRotation());
            const Vec3 model = (!entRot) * (I.targetWorld - pEnt->GetWorldPos());
            if (Finite(model))
                targetView = camAbs.GetInverted() * model; // relative to the REAL camera: that is where it must appear
        }
    }
    targetView += Vec3(st.offX, st.offY, st.offZ);
    if (I.poseAbsolutePos)
        targetView = I.poseAbsView; // the pose says where the hand goes, the object does not matter
    // The target itself is only kept in front of the camera; the reach envelope is applied to the WRIST below
    // (target + pose offset + corrections), since that is what the arm has to get to - with the fingertip 15 cm
    // ahead of the wrist a screen at 0.75 m is still touchable.
    if (!Finite(targetView))
        targetView = Vec3(0.0f, 0.4f, 0.0f);
    targetView.y = max(targetView.y, max(s.interactMinForward, 0.02f));
    I.clamped = false;
    // Nose-to-nose with a keypad: the camera is 20-30 cm from the surface, and a hand touching it would have to sit
    // right on the lens (or inside the wall). Nothing to be done there - no reach.
    I.examTooClose = I.examining && I.hasWorldTarget && targetView.y < clamp_tpl(s.interactExamMinTargetDist, 0.0f, 1.0f);
    if (I.examTooClose)
    {
        noReachThisFrame();
        I.corr = Vec3(ZERO);
        I.desiredPrevValid = false;
        I.targetView = targetView;
        return;
    }
    I.targetView = targetView;
    I.handView = handView;

    // Base of the reach: the animated hand, or - resting - the resting spot in view. Hovering over something usable
    // the spot is pulled toward it (interactHoverTowards); the spot is smoothed so a screen <-> hover hand-over
    // glides, and the slow drift rides on top.
    const Vec3 restNormal(s.interactRestX, s.interactRestY, s.interactRestZ);
    const Vec3 restExam(s.interactRestExamX, s.interactRestExamY, s.interactRestExamZ);
    Vec3 restWant = restNormal + (restExam - restNormal) * examBlend;
    if (const WeaponSettings* pW = InteractWeaponEntry(); pW && SanePose(pW->interactRest))
        restWant += pW->interactRest.Pos() * (1.0f - examBlend); // this weapon's own spot (not on screens: no weapon there)
    if (I.hoverActive && I.hoverTargetValid && !I.examining)
    {
        ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
        IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
        if (pEnt)
        {
            const Quat entRot = SafeNormalized(pEnt->GetWorldRotation());
            const Vec3 model = (!entRot) * (I.hoverWorld - pEnt->GetWorldPos());
            const Vec3 hoverView = camAbs.GetInverted() * model;
            if (Finite(hoverView) && hoverView.y > 0.05f)
                restWant += (hoverView - restWant) * clamp_tpl(s.interactHoverTowards, 0.0f, 1.0f);
        }
    }
    if (!I.restPointValid || rest <= 0.001f || !Finite(I.restPointView))
        I.restPointView = restWant;
    else
        I.restPointView += (restWant - I.restPointView) * clamp_tpl(1.0f - expf(-max(I.frameDt, 0.0f) / 0.12f), 0.0f, 1.0f);
    I.restPointValid = true;
    const Vec3 restView = I.restPointView + I.swayPos;
    // Where the hand comes up FROM. Normally where the animation has it (on the weapon's grip). With the support
    // hand off the weapon - one-handed weapons animate the left IK weight at 0, and there is no weapon at all
    // when unarmed - the animated hand is somewhere off screen, and a blend from there crosses into view in a
    // couple of frames whatever the blend time: the hand "pops". So it comes up from a fixed spot below the
    // view instead (global, or the weapon's own), and the IK weight is on from the first frame.
    Vec3 startView = handView;
    I.hiddenStartUsed = false;
    if (s.interactHiddenStart && SupportHandOffWeapon() && examBlend < 0.5f)
    {
        Vec3 sp(s.interactStartX, s.interactStartY, s.interactStartZ);
        if (const WeaponSettings* pW = InteractWeaponEntry(); pW && SanePose(pW->interactStart) && pW->interactStart.Pos().GetLengthSquared() > 1e-6f)
            sp = pW->interactStart.Pos();
        if (Finite(sp))
        {
            startView = sp;
            I.hiddenStartUsed = true;
        }
    }
    const Vec3 base = startView + (restView - startView) * rest;
    if (st.along != 0.0f && targetView.GetLengthSquared() > 1e-6f)
        targetView += targetView.GetNormalized() * clamp_tpl(st.along, -0.5f, 0.5f); // along the camera -> target line
    Vec3 desired = base + (targetView - base) * (curve * clamp_tpl(st.amount, 0.0f, 2.0f));
    desired += Vec3(st.windX, st.windY, st.windZ) * wind; // the windup keyframe: pulled back before the reach
    const float ax = I.returning ? st.retArcX : st.arcX;
    const float az = I.returning ? st.retArcZ : st.arcZ;
    desired += Vec3(ax * I.arc, 0.0f, az * I.arc);
    desired += I.poseOffsetView; // the pose's wrist offset (already weighted), e.g. so the fingertip and not the wrist lands on the target
    if (const WeaponSettings* pW = InteractWeaponEntry(); pW && SanePose(pW->interact))
        desired += pW->interact.Pos() * curve; // this weapon's correction (its grip puts the hand somewhere else to start with)
    if (examBlend > 0.0f && SanePose(s.examCorr))
        desired += s.examCorr.Pos() * curve * examBlend;   // screens only: their own correction on top (fading out after)
    // Reach envelope on the wrist: a point outside the box is pulled in ALONG THE LINE FROM THE CAMERA (the whole
    // vector scaled by one factor) so the hand still points at the target instead of sliding along a wall of the
    // box (a clamp per axis left the hand at the box's floor but at the object's distance: "stops short" of
    // things on the ground). Screens sit further away than most interactions, so they get their own forward
    // limit. The envelope takes effect in proportion to the blend: at the start of a reach or of the resting
    // blend the hand is still where the animation has it, and that place may well be outside the box (one-handed
    // weapons keep the support hand off screen) - clamping it there snapped the hand into the box in one frame.
    {
        const Vec3 unclamped = desired;
        const float relax = 1.0f + (clamp_tpl(st.envelopeScale, 0.5f, 3.0f) - 1.0f) * curve; // the style's relaxation, with the reach
        const float maxFwd = max(s.interactMaxForward + (s.interactExamMaxForward - s.interactMaxForward) * examBlend, 0.1f) * relax;
        const float side = max(s.interactMaxSide, 0.05f) * relax, up = max(s.interactMaxUp, 0.05f) * relax, down = max(s.interactMaxDown, 0.05f) * relax;
        float sc = 1.0f;
        if (desired.y > maxFwd) sc = min(sc, maxFwd / desired.y);
        if (desired.x > side) sc = min(sc, side / desired.x);
        if (desired.x < -side) sc = min(sc, -side / desired.x);
        if (desired.z > up) sc = min(sc, up / desired.z);
        if (desired.z < -down) sc = min(sc, -down / desired.z);
        Vec3 clamped = desired * clamp_tpl(sc, 0.0f, 1.0f);
        clamped.y = max(clamped.y, max(s.interactMinForward, 0.02f) * 0.5f);
        const float k = max(max(curve, wind), rest);
        desired = unclamped + (clamped - unclamped) * k;
        I.clamped = (unclamped - clamped).GetLengthSquared() > 1e-6f;
    }
    I.desiredView = desired;

    // Closed loop on the HAND joint. The push moves the IK target; where the hand itself ends up differs from
    // that by whatever the rig does (effector offset, partial IK weight) - and that differs per weapon. Last
    // frame's final hand (with the camera of that frame, exact from the render side) against what was asked of
    // it gives the error; integrating it into the push makes the hand land on the point regardless.
    if (s.interactCorrGain > 0.0f && R.camValid && SaneQuatT(R.camModel) && R.leftHand >= 0)
    {
        const QuatT* pHandAbs = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, R.leftHand);
        if (pHandAbs && SaneQuatT(*pHandAbs))
        {
            I.handActualView = R.camModel.GetInverted() * pHandAbs->t;
            I.handActualValid = Finite(I.handActualView);
        }
        if (R.leftUpperArm >= 0)
        {
            const QuatT* pSh = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, R.leftUpperArm);
            if (pSh && SaneQuatT(*pSh))
            {
                I.shoulderView = R.camModel.GetInverted() * pSh->t; // last frame's final pose: with the render shift, i.e. as seen
                I.shoulderValid = Finite(I.shoulderView);
            }
        }
        if (pHandAbs && SaneQuatT(*pHandAbs) && I.desiredPrevValid && SaneQuatT(I.camReachPrev))
        {
            // Measured in the camera the push was computed with (last frame's), not the current one: otherwise
            // every camera movement shows up as a hand error and gets integrated, and the hand chases the turn.
            const Vec3 handActual = I.camReachPrev.GetInverted() * pHandAbs->t;
            const Vec3 err = I.desiredPrev - handActual;
            I.corrError = Finite(err) ? err.GetLength() : 0.0f;
            if (Finite(err) && err.GetLengthSquared() < 0.5f * 0.5f)
            {
                I.corr += err * clamp_tpl(s.interactCorrGain, 0.0f, 1.0f);
                const float maxCorr = 0.35f;
                if (I.corr.GetLengthSquared() > maxCorr * maxCorr)
                    I.corr = I.corr.GetNormalized() * maxCorr;
            }
        }
        I.desiredPrev = desired;
        I.desiredPrevValid = true;
        I.camReachPrev = camReach;
    }
    else
    {
        I.corr = Vec3(ZERO);
        I.desiredPrevValid = false;
    }
    if (!Finite(I.corr))
    {
        I.corr = Vec3(ZERO);
        m_nanRecoveries++;
    }

    QuatT add;
    add.t = camReach.q * (desired + I.corr * curve - handView); // view-space delta -> model space (rotation only)
    add.q = Quat(IDENTITY);
    // When a pose is in charge of the wrist through the IK target joint (an override), no additive rotation on
    // top of it - the two would fight and the additive bookkeeping would be off.
    const bool poseOwnsIkRot = I.poseApplied && (s.interactWristMode == 0 || s.interactWristMode == 2);
    if (!poseOwnsIkRot && s.interactRotate && (st.pitch != 0.0f || st.yaw != 0.0f || st.roll != 0.0f))
    {
        const Quat rotView = Quat::CreateRotationXYZ(Ang3(DEG2RAD(st.pitch * curve), DEG2RAD(st.roll * curve), DEG2RAD(st.yaw * curve)));
        add.q = SafeNormalized(camReach.q * rotView * (!camReach.q)); // the same rotation expressed in model space
    }
    // (3 m: with the support hand animated off screen - one-handed weapons, no weapon - the additive from there to a
    // punch 75 cm ahead is well over 1.5 m; the old limit dropped the push for a frame at the apex, and the hand
    // popped to the animation and back.)
    if (!Finite(add.t) || add.t.GetLengthSquared() > 3.0f * 3.0f || !SaneQuat(add.q))
    {
        noReachThisFrame();
        m_nanRecoveries++;
        return;
    }
    VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, joint, OP_ADDITIVE, &add.t);
    // Hand off the weapon (one-handed weapon, no weapon): the animated arm hangs somewhere off screen, and the IK
    // solving from that shoulder to a target in view gives a crooked, over-stretched arm. The shoulder (and,
    // rig permitting, the elbow) is moved to where a hand held up in view would have it - global spot plus the
    // weapon's own - for as long as the hand is up.
    if (I.hiddenStartUsed)
    {
        const float k = max(max(curve, wind), rest);
        Vec3 sh(s.interactStartShoulder.posX, s.interactStartShoulder.posY, s.interactStartShoulder.posZ);
        Vec3 el(s.interactStartElbow.posX, s.interactStartElbow.posY, s.interactStartElbow.posZ);
        if (const WeaponSettings* pW = InteractWeaponEntry(); pW)
        {
            if (SanePose(pW->interactStartShoulder)) sh += pW->interactStartShoulder.Pos();
            if (SanePose(pW->interactStartElbow)) el += pW->interactStartElbow.Pos();
        }
        if (k > 0.0f && R.leftUpperArm >= 0 && sh.GetLengthSquared() > 1e-8f)
        {
            const Vec3 off = camReach.q * (sh * k);
            if (Finite(off)) VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, R.leftUpperArm, OP_ADDITIVE, &off);
        }
        const int forearm = R.leftSubtreeParent.empty() ? -1 : R.leftSubtreeParent[0];
        if (k > 0.0f && forearm >= 0 && el.GetLengthSquared() > 1e-8f)
        {
            const Vec3 off = camReach.q * (el * k);
            if (Finite(off)) VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, forearm, OP_ADDITIVE, &off);
        }
    }
    // The windup's shoulder / elbow / forearm: additive on the arm joints (the limb IK re-solves the arm from the
    // moved shoulder; whether it keeps a moved elbow is the rig's call), scaled by the windup and, optionally,
    // kept through the strike.
    {
        const float wk = clamp_tpl(wind + curve * clamp_tpl(st.windKeep, 0.0f, 1.0f), 0.0f, 1.0f);
        if (wk > 0.0f)
        {
            if (R.leftUpperArm >= 0 && (st.windShX != 0.0f || st.windShY != 0.0f || st.windShZ != 0.0f))
            {
                const Vec3 off = camReach.q * (Vec3(st.windShX, st.windShY, st.windShZ) * wk);
                if (Finite(off)) VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, R.leftUpperArm, OP_ADDITIVE, &off);
            }
            const int forearm = R.leftSubtreeParent.empty() ? -1 : R.leftSubtreeParent[0];
            if (forearm >= 0 && (st.windElX != 0.0f || st.windElY != 0.0f || st.windElZ != 0.0f))
            {
                const Vec3 off = camReach.q * (Vec3(st.windElX, st.windElY, st.windElZ) * wk);
                if (Finite(off)) VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, forearm, OP_ADDITIVE, &off);
            }
            if (forearm >= 0 && (st.windFaPitch != 0.0f || st.windFaYaw != 0.0f || st.windFaRoll != 0.0f))
            {
                const QuatT* pFa = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, forearm);
                if (pFa && SaneQuatT(*pFa))
                {
                    const Quat local = SafeNormalized(Quat::CreateRotationXYZ(Ang3(DEG2RAD(st.windFaPitch), DEG2RAD(st.windFaRoll), DEG2RAD(st.windFaYaw))));
                    const Quat model = SafeNormalized(pFa->q * local * (!pFa->q));
                    const Quat q = SafeNormalized(Quat::CreateNlerp(Quat(IDENTITY), model, wk));
                    if (SaneQuat(q)) VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, forearm, OP_ADDITIVE, &q);
                }
            }
        }
    }
    if (!poseOwnsIkRot)
        VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, joint, OP_ADDITIVE, &add.q);
    if (s.interactForceLeftIk && I.weightJoint >= 0)
    {
        // Same op the game uses for the right arm's weight joint every frame (eOp_OverrideRelative, x = weight).
        // One-handed weapons (wrench, grenades) animate the left weight at 0 - the hand is wherever the animation
        // has it, off screen - so forcing 1 snaps it to the IK target in one frame. The weight is blended in with
        // the reach / rest instead, from the animated value (captured on the first frame we push).
        float w = 1.0f;
        if (s.interactIkWeightRamp)
        {
            if (!I.animIkWeightValid && I.weightJointParent >= 0)
            {
                const QuatT* pJ = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, I.weightJoint);
                const QuatT* pP = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, I.weightJointParent);
                if (pJ && pP && SaneQuatT(*pJ) && SaneQuatT(*pP))
                {
                    const Vec3 rel = (!pP->q) * (pJ->t - pP->t);
                    if (Finite(rel)) { I.animIkWeight = clamp_tpl(rel.x, 0.0f, 1.0f); I.animIkWeightValid = true; }
                }
            }
            const float blend = max(max(curve, wind), rest);
            const float animW = I.animIkWeightValid ? I.animIkWeight : 0.0f;
            w = I.hiddenStartUsed ? 1.0f : clamp_tpl(animW + (1.0f - animW) * blend, 0.0f, 1.0f); // from the hidden spot the IK is on from the first frame

        }
        const Vec3 wv(w, 0.0f, 0.0f);
        VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushPosition, I.weightJoint, 1, &wv);
        I.ikWeightPushed = w;
    }
    I.lastAdd = add;
    I.lastAdd.t += bodyShift; // the IK target joint received the body shift as well
    I.lastReach = add.t;
    I.addValid = true;
    I.pushes++;
}

void ModMain::LogHandJoints()
{
    using namespace PreyInternals;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    ICharacterInstance* pChar = (pPlayer && pPlayer->GetEntity()) ? pPlayer->GetEntity()->GetCharacter(0) : nullptr;
    if (!pChar)
    {
        CryLog("ViewmodelTweaks: no player character to list joints of");
        return;
    }
    void* pSkel = VCall<void*>(pChar, VT_ICharacterInstance_GetIDefaultSkeleton);
    if (!pSkel)
        return;
    const int count = (int)VCall<unsigned>(pSkel, VT_IDefaultSkeleton_GetJointCount);
    CryLog("ViewmodelTweaks: first-person arms skeleton, {} joints (hand / arm / target / blend / ik ones):", count);
    for (int i = 0; i < count && i < 4096; i++)
    {
        const char* n = VCall<const char*>(pSkel, VT_IDefaultSkeleton_GetJointNameByID, i);
        if (!n) continue;
        std::string lower(n);
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("hand") != std::string::npos || lower.find("arm") != std::string::npos || lower.find("target") != std::string::npos
            || lower.find("blend") != std::string::npos || lower.find("ik") != std::string::npos || lower.find("prop") != std::string::npos)
        {
            const int parent = VCall<int>(pSkel, VT_IDefaultSkeleton_GetJointParentIDByID, i);
            const char* pn = parent >= 0 ? VCall<const char*>(pSkel, VT_IDefaultSkeleton_GetJointNameByID, parent) : "";
            CryLog("  [{}] {}  (parent [{}] {})", i, n, parent, pn ? pn : "");
        }
    }
}

//---------------------------------------------------------------------------------
// Hand poses
//---------------------------------------------------------------------------------
// The fingers are ordinary joints under the hand joint, and the operator queue takes parent-relative orientation
// overrides (eOp_OverrideRelative), so a pose is one quaternion per subtree joint. The hand itself gets either
// an absolute (model-space) orientation = camera * user rotation - fixed in view space, whatever the arm does -
// or a forearm-relative one. Everything is pushed parents-first (CryEngine keeps parent index < child index),
// blended from the animated pose captured the moment the pose started coming in.
HandPose* ModMain::FindPose(const char* name)
{
    if (!name || !*name)
        return nullptr;
    for (HandPose& p : m_poses)
        if (p.name == name) return &p;
    return nullptr;
}

const ReachStyle& ModMain::CurStyle() const
{
    const ViewmodelSettings& s = m_settings;
    if (m_interact.style == 1)
        return s.grab;
    if (m_interact.style == 2)
        return s.punch;
    return (m_interact.examining && s.interactExamGentle) ? s.pressExam : s.press;
}

const HandPose* ModMain::RestPose() const
{
    const std::string& name = m_stylePose[2].empty() ? m_stylePose[0] : m_stylePose[2];
    if (name.empty())
        return nullptr;
    for (const HandPose& p : m_poses)
        if (p.name == name) return &p;
    return nullptr;
}

const HandPose* ModMain::ActivePose() const
{
    const InteractState& I = m_interact;
    if (I.holdMode != 0)
        return (m_editPose >= 0 && m_editPose < (int)m_poses.size()) ? &m_poses[(size_t)m_editPose] : nullptr;
    const std::string& name = m_stylePose[PoseSlotOfStyle(I.style)];
    if (name.empty())
        return nullptr;
    for (const HandPose& p : m_poses)
        if (p.name == name) return &p;
    return nullptr;
}

void ModMain::PushHandPose(void* pModifier, void* pSkelPose, const QuatT& camAbs, float weight)
{
    using namespace PreyInternals;
    InteractState& I = m_interact;
    RenderLockState& R = m_render;
    const ViewmodelSettings& s = m_settings;
    I.poseJointsPushed = 0;
    I.poseWeight = 0.0f;
    I.poseName.clear();
    I.poseOffsetView = Vec3(ZERO);
    I.poseAbsolutePos = false;

    if (!pModifier || !pSkelPose || R.leftHand < 0 || R.leftSubtree.empty() || R.leftSubtree.size() != R.leftSubtreeParent.size()
        || R.leftSubtreeDefaultRel.size() != R.leftSubtree.size())
    {
        I.poseApplied = false;
        return;
    }
    const size_t n = R.leftSubtree.size();

    // Previous frame's final pose of the whole subtree (and the forearm), model space.
    auto absOf = [&](int id) -> const QuatT* {
        if (id < 0) return nullptr;
        const QuatT* p = VCall<const QuatT*>(pSkelPose, VT_ISkeletonPose_GetAbsJointByID, id);
        return (p && SaneQuatT(*p)) ? p : nullptr;
    };
    std::vector<Quat> relNow(n, Quat(IDENTITY));
    bool allOk = true;
    for (size_t i = 0; i < n && allOk; i++)
    {
        const QuatT* pJ = absOf(R.leftSubtree[i]);
        const QuatT* pP = absOf(R.leftSubtreeParent[i]);
        if (!pJ || !pP) { allOk = false; break; }
        relNow[i] = SafeNormalized((!pP->q) * pJ->q);
    }
    const QuatT* pHand = absOf(R.leftHand);
    const QuatT* pForearm = absOf(R.leftSubtreeParent.empty() ? -1 : R.leftSubtreeParent[0]);
    const QuatT* pIk = absOf(m_lock.leftIkJoint);
    if (!allOk || !pHand || !pForearm)
    {
        I.poseApplied = false;
        return;
    }

    // Two poses can be in play: the resting hand's and the reach's. Between clicks on a screen (or hovering over
    // something usable) the hand shows the rest pose; as a reach progresses (curve 0 -> 1) it blends into the
    // reach pose and back, so the fingers never pop from one to the other.
    const HandPose* pPose = ActivePose();
    const HandPose* pRest = (I.holdMode != 0) ? nullptr : RestPose();
    const float restNow = (I.holdMode != 0) ? 0.0f : clamp_tpl(I.restBlend, 0.0f, 1.0f);
    const float curveNow = (I.holdMode != 0) ? 1.0f : max(clamp_tpl(I.curve, 0.0f, 1.0f), I.phase != InteractState::Idle ? clamp_tpl(I.wind, 0.0f, 1.0f) : 0.0f); // the pose forms during the windup too
    const float k = restNow > 0.0f ? curveNow : 1.0f; // 0 = all rest pose, 1 = all reach pose
    if (!pPose) pPose = pRest;
    if (!pRest) pRest = pPose;
    const float amountReach = (I.holdMode != 0) ? 1.0f : clamp_tpl(m_stylePoseAmount[PoseSlotOfStyle(I.style)], 0.0f, 1.0f);
    const float amountRest = clamp_tpl(m_stylePose[2].empty() ? m_stylePoseAmount[0] : m_stylePoseAmount[2], 0.0f, 1.0f);
    const float amount = amountRest + (amountReach - amountRest) * k;
    const float w = clamp_tpl(weight * amount, 0.0f, 1.0f);
    if (!pPose || !pRest || w <= 0.0f)
    {
        I.poseApplied = false;
        return;
    }
    // The animated pose to blend from: captured on the first frame we push (last frame was pure animation),
    // kept for as long as we keep pushing (from then on the final pose is ours, not the animation's).
    if (!I.poseApplied || !I.animValid || I.animRel.size() != n)
    {
        I.animRel = relNow;
        I.animHandAbs = pHand->q;
        I.animIkQ = pIk ? pIk->q : pHand->q;
        I.animValid = true;
    }
    const Vec3 restOff = pRest->absolutePos ? Vec3(ZERO) : pRest->Pos();
    const Vec3 reachOff = pPose->absolutePos ? Vec3(ZERO) : pPose->Pos();
    I.poseOffsetView = (restOff + (reachOff - restOff) * k) * w;
    I.poseAbsolutePos = pPose->absolutePos != 0;
    I.poseAbsView = pPose->Pos();

    // Wrist: an absolute orientation in view space. Two ways to get it into the rig, selectable because it is
    // not known which one the animation-driven IK honours: the IK target joint's orientation (if the limb IK
    // copies it to the hand) and the hand joint's rotation relative to the forearm (recomputed by the IK from
    // the relative pose, so it survives the solve; uses last frame's forearm, exact once the pose holds).
    Quat wristView = SafeNormalized(Quat::CreateNlerp(pRest->HandRotView(), pPose->HandRotView(), k));
    // The style's "hand rotation at the target" (press / grab / punch pitch, yaw, roll), with the reach. When a pose
    // owns the wrist the additive route in PushInteractReach is skipped, so it is folded in here instead.
    if (I.holdMode == 0 && s.interactRotate && curveNow > 0.0f)
    {
        const ReachStyle& st = CurStyle();
        if (st.pitch != 0.0f || st.yaw != 0.0f || st.roll != 0.0f)
            wristView = SafeNormalized(Quat::CreateRotationXYZ(Ang3(DEG2RAD(st.pitch * curveNow), DEG2RAD(st.roll * curveNow), DEG2RAD(st.yaw * curveNow))) * wristView);
    }
    if (restNow > 0.0f && SaneQuat(I.swayRot))
        wristView = SafeNormalized(I.swayRot * wristView); // the resting hand's slow drift (already scaled)
    if (const WeaponSettings* pW = InteractWeaponEntry(); pW && SanePose(pW->interact))
        wristView = pW->interact.Rot() * wristView; // per-weapon wrist correction
    if (I.examBlend > 0.0f && SanePose(s.examCorr))
        wristView = SafeNormalized(Quat::CreateNlerp(Quat(IDENTITY), s.examCorr.Rot(), clamp_tpl(I.examBlend, 0.0f, 1.0f))) * wristView; // screens only, fading out on the way back
    const Quat wristAbs = SafeNormalized(camAbs.q * wristView);
    if ((s.interactWristMode == 0 || s.interactWristMode == 2) && m_lock.leftIkJoint >= 0 && pIk)
    {
        Quat q = SafeNormalized(Quat::CreateNlerp(I.animIkQ, wristAbs, w));
        if (SaneQuat(q))
        {
            VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, m_lock.leftIkJoint, OP_OVERRIDE, &q);
            I.poseJointsPushed++;
        }
    }
    if (s.interactWristMode == 1 || s.interactWristMode == 2)
    {
        const Quat handAbs = SafeNormalized(Quat::CreateNlerp(I.animHandAbs, wristAbs, w));
        Quat rel = SafeNormalized((!pForearm->q) * handAbs);
        if (SaneQuat(rel))
        {
            VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, R.leftHand, 1 /*eOp_OverrideRelative*/, &rel);
            I.poseJointsPushed++;
        }
    }
    // Forearm: a per-weapon twist / bend (rotation about the forearm's own axes, eOp_Additive expressed in model
    // space with last frame's forearm frame). The hand's orientation is pushed absolutely above, so it does not
    // inherit this - only the forearm turns under the wrist, which is what fixes an over-rotated-looking wrist.
    if (const WeaponSettings* pW = InteractWeaponEntry(); pW && SanePose(pW->interactForearm) && !R.leftSubtreeParent.empty() && R.leftSubtreeParent[0] >= 0)
    {
        const PoseOffset& f = pW->interactForearm;
        if (f.pitch != 0.0f || f.yaw != 0.0f || f.roll != 0.0f)
        {
            const Quat local = SafeNormalized(f.Rot());
            const Quat model = SafeNormalized(pForearm->q * local * (!pForearm->q));
            const Quat q = SafeNormalized(Quat::CreateNlerp(Quat(IDENTITY), model, w));
            if (SaneQuat(q))
            {
                VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, R.leftSubtreeParent[0], OP_ADDITIVE, &q);
                I.poseJointsPushed++;
            }
        }
    }
    // Fingers (and anything else under the hand), parents first: bind pose * adjustment, absolute - the weapon's
    // grip only supplies the start of the blend.
    for (size_t i = 0; i < n; i++)
    {
        const int id = R.leftSubtree[i];
        if (id == R.leftHand) continue;
        const HandPoseJoint* pJ = pPose->Find(R.leftSubtreeNames[i].c_str());
        const HandPoseJoint* pJr = pRest->Find(R.leftSubtreeNames[i].c_str());
        const Quat tReach = SafeNormalized(R.leftSubtreeDefaultRel[i] * (pJ ? pJ->Adjust() : Quat(IDENTITY)));
        const Quat tRest = (pRest == pPose) ? tReach : SafeNormalized(R.leftSubtreeDefaultRel[i] * (pJr ? pJr->Adjust() : Quat(IDENTITY)));
        const Quat target = (pRest == pPose) ? tReach : SafeNormalized(Quat::CreateNlerp(tRest, tReach, k));
        Quat q = SafeNormalized(Quat::CreateNlerp(I.animRel[i], target, w));
        if (!SaneQuat(q)) continue;
        VCall<void>(pModifier, VT_IAnimationOperatorQueue_PushOrientation, id, 1 /*eOp_OverrideRelative*/, &q);
        I.poseJointsPushed++;
    }
    I.poseApplied = true;
    I.poseWeight = w;
    I.poseName = pPose->name;
}

bool ModMain::ExaminingWorldUI() const
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
        return false;
    const ArkExaminationMode& em = pPlayer->m_examinationMode;
    return em.m_examinationState == ArkExaminationMode::EArkExaminationState::active
        && em.m_examinationType == ArkExaminationMode::EArkExaminationType::worldUI;
}

bool ModMain::CursorWorldPoint(Vec3& out)
{
    // In examination mode the mouse turns the camera (ArkExaminationMode::UpdateView accumulates
    // ArkPlayerInput::GetRotation into m_localRotation) and the HUD reticle sits at the centre: what you click is
    // what the centre of the view is on. A ray from the view camera along its axis hits the screen.
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !gEnv || !gEnv->pSystem || !gEnv->pPhysicalWorld)
        return false;
    const CCamera& cam = gEnv->pSystem->GetViewCamera();
    const Vec3 camPos = cam.GetPosition();
    Vec3 dir = cam.GetMatrix().GetColumn1();
    if (!Finite(dir) || dir.GetLengthSquared() < 1e-6f)
        return false;
    dir.Normalize();
    IPhysicalEntity* pSkip = pPlayer->GetEntity() ? pPlayer->GetEntity()->GetPhysics() : nullptr;
    PaddedRayHit hit;
    const int n = gEnv->pPhysicalWorld->RayWorldIntersection(camPos, dir * 3.0f, ent_all,
        rwi_stop_at_pierceable | rwi_colltype_any(geom_colltype_ray | geom_colltype0 | geom_colltype_player), &hit, 1, pSkip);
    if (n > 0 && Finite(hit.pt))
    {
        out = hit.pt;
        if (m_settings.interactDebugMarker)
            CryLog("ViewmodelTweaks: screen ray hit at {:.2f} m, normal ({:.2f} {:.2f} {:.2f}), surface {}, {}", hit.dist, hit.n.x, hit.n.y, hit.n.z, hit.surface_idx,
                hit.pCollider ? "physical entity" : "no collider");
        return true;
    }
    if (m_settings.interactDebugMarker)
        CryLog("ViewmodelTweaks: screen ray hit nothing within 3 m - using the examined entity's distance");
    // Nothing solid under the cursor (screens are not always physicalized): a point at the examined entity's distance.
    float dist = 0.6f;
    if (IEntity* pTarget = (gEnv->pEntitySystem && pPlayer->m_examinationMode.m_targetEntity) ? gEnv->pEntitySystem->GetEntity(pPlayer->m_examinationMode.m_targetEntity) : nullptr)
    {
        const Vec3 tp = pTarget->GetWorldPos();
        if (Finite(tp))
            dist = clamp_tpl((tp - camPos) | dir, 0.25f, 2.0f);
    }
    out = camPos + dir * dist;
    return true;
}

void ModMain::UpdateExamZoom()
{
    // The screen zoom is a zoom-manager entry at priority "normal" (ArkExaminationMode::UpdateView ->
    // SetDesiredHFOV(worldUI zoom, 0, false, 3)); an entry of ours at "high" wins over it while it lives.
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
    {
        m_examZoomHandle = 0;
        return;
    }
    ArkPlayerZoomManager& zoom = pPlayer->m_zoomManager;
    const ViewmodelSettings& s = m_settings;
    SCVars* pCVars = *g_pGameCVars;
    const float baseHfov = (pCVars && pCVars->cl_hfov > 1.0f) ? pCVars->cl_hfov : 90.0f;
    const bool want = Active() && s.interactEnabled && s.interactExamination && s.interactExamFovMode != 0 && m_interact.examining && !m_playerDead;
    const float hfov = clamp_tpl(s.interactExamFovMode == 2 ? s.interactExamFov : baseHfov, 20.0f, 140.0f);
    if (want && m_examZoomHandle == 0)
    {
        m_examZoomHandle = zoom.SetDesiredHFOV(hfov, 0.25f, false, EArkZoomPriority::high);
        m_examZoomHfov = hfov;
    }
    else if (want && fabsf(hfov - m_examZoomHfov) > 0.01f)
    {
        zoom.UpdateDesiredHFOV(m_examZoomHandle, hfov, 0.1f, false);
        m_examZoomHfov = hfov;
    }
    else if (!want && m_examZoomHandle != 0)
    {
        zoom.ClearDesiredHFOV(m_examZoomHandle, 0.25f, false);
        m_examZoomHandle = 0;
    }
}

void ModMain::UpdateArmsVisibility()
{
    InteractState& I = m_interact;
    const ViewmodelSettings& s = m_settings;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
    if (!pEnt)
    {
        I.armsForced = false;
        return;
    }
    CEntity* pCEnt = static_cast<CEntity*>(pEnt);
    const unsigned flags = CEntity::FGetSlotFlags(pCEnt, 0);
    I.slotFlagsNow = flags;
    const bool hiddenState = I.unarmed || I.examining;           // states in which the game keeps the arms out of sight
    const bool reachActive = (I.phase != InteractState::Idle && (I.curve > 0.001f || I.wind > 0.001f)) || I.restBlend > 0.001f;
    const bool want = s.interactShowArms && hiddenState && reachActive && Active() && s.interactEnabled;
    constexpr unsigned ENTITY_SLOT_RENDER_FLAG = 1u;
    if (want && !I.armsForced && !(flags & ENTITY_SLOT_RENDER_FLAG))
    {
        I.savedSlotFlags = flags;
        I.armsForcedWhile = I.examining ? 1 : 2; // which state had the arms hidden when we took over
        CEntity::FSetSlotFlags(pCEnt, 0, flags | ENTITY_SLOT_RENDER_FLAG);
        I.armsForced = true;
    }
    else if (!want && I.armsForced)
    {
        // Give the hidden state back only if the state that hid the arms is still on. Leaving a screen, the game
        // itself turns the arms back on (SetExamining_Internal restores the render flag); writing the saved
        // "hidden" flags over that left the arms - and every weapon drawn afterwards - invisible until the next
        // screen fixed it (3.9.4 and earlier).
        const bool sameState = I.armsForcedWhile == 1 ? I.examining : (I.unarmed && !I.examining);
        if (sameState && (flags & ~ENTITY_SLOT_RENDER_FLAG) == (I.savedSlotFlags & ~ENTITY_SLOT_RENDER_FLAG))
            CEntity::FSetSlotFlags(pCEnt, 0, I.savedSlotFlags);
        I.armsForced = false;
    }
}

fs::path ModMain::GetPosesPath() const
{
    return GetWeaponsPath().parent_path() / "Vee.ViewmodelTweaks.poses.xml";
}

void ModMain::LoadPoses()
{
    m_poses.clear();
    pugi::xml_document doc;
    const fs::path path = GetPosesPath();
    if (doc.load_file(path.c_str()))
    {
        pugi::xml_node root = doc.child("HandPoses");
        for (pugi::xml_node n : root.children("Pose"))
        {
            HandPose p;
            p.name = n.attribute("name").as_string("");
            if (p.name.empty()) continue;
            p.posX = n.attribute("pos_x").as_float(0.0f);
            p.posY = n.attribute("pos_y").as_float(0.0f);
            p.posZ = n.attribute("pos_z").as_float(0.0f);
            p.absolutePos = n.attribute("absolute_pos").as_int(0);
            p.handPitch = n.attribute("hand_pitch").as_float(0.0f);
            p.handYaw = n.attribute("hand_yaw").as_float(0.0f);
            p.handRoll = n.attribute("hand_roll").as_float(0.0f);
            if (!Finite(p.Pos())) p.posX = p.posY = p.posZ = 0.0f;
            if (!Finite(Vec3(p.handPitch, p.handYaw, p.handRoll))) p.handPitch = p.handYaw = p.handRoll = 0.0f;
            for (pugi::xml_node j : n.children("Joint"))
            {
                HandPoseJoint hj;
                hj.name = j.attribute("name").as_string("");
                if (hj.name.empty()) continue;
                hj.pitch = j.attribute("pitch").as_float(0.0f);
                hj.yaw = j.attribute("yaw").as_float(0.0f);
                hj.roll = j.attribute("roll").as_float(0.0f);
                if (!Finite(Vec3(hj.pitch, hj.yaw, hj.roll))) hj.pitch = hj.yaw = hj.roll = 0.0f;
                p.joints.push_back(hj);
            }
            m_poses.push_back(p);
        }
        for (pugi::xml_node n : root.children("Style"))
        {
            const char* sn = n.attribute("name").as_string("");
            const int idx = strcmp(sn, "grab") == 0 ? 1 : (strcmp(sn, "rest") == 0 ? 2 : (strcmp(sn, "punch") == 0 ? 3 : 0));
            m_stylePose[idx] = n.attribute("pose").as_string(m_stylePose[idx].c_str());
            m_stylePoseAmount[idx] = n.attribute("amount").as_float(1.0f);
            if (!Finite(m_stylePoseAmount[idx])) m_stylePoseAmount[idx] = 1.0f;
        }
        m_rules.clear();
        pugi::xml_node rules = root.child("Rules");
        if (rules)
        {
            for (pugi::xml_node n : rules.children("Rule"))
            {
                InteractRule r;
                r.type = n.attribute("type").as_int(-1);
                r.mode = n.attribute("mode").as_int(-1);
                r.classContains = n.attribute("class").as_string("");
                r.textContains = n.attribute("text").as_string("");
                r.style = clamp_tpl(n.attribute("style").as_int(0), 0, 2);
                r.hover = n.attribute("hover").as_int(1);
                r.note = n.attribute("note").as_string("");
                m_rules.push_back(r);
            }
        }
        else
            SeedDefaultRules(); // a file from before the rules existed
        CryLog("ViewmodelTweaks: loaded {} hand pose(s) and {} rule(s) from {}", m_poses.size(), m_rules.size(), path.u8string());
    }
    else
        SeedDefaultRules();
    if (m_poses.empty())
        SeedDefaultPoses(m_poses, m_stylePose);
    m_editPose = clamp_tpl(m_editPose, 0, (int)m_poses.size() - 1);
    m_posesDirty = false;
}

void ModMain::SeedDefaultPoses(std::vector<HandPose>& poses, std::string* pStylePose)
{
    // The poses as shaped in play (3.10.3): the pointing finger for presses, and the relaxed version the resting hand shows.
    auto joint = [](HandPose& p, const char* n, float pitch, float yaw, float roll) {
        HandPoseJoint j; j.name = n; j.pitch = pitch; j.yaw = yaw; j.roll = roll; p.joints.push_back(j);
    };
    HandPose point;
    point.name = "point";
    point.posX = -0.0404f; point.posY = 0.0006f; point.posZ = -0.0573f;
    point.handPitch = 12.68f; point.handYaw = 83.65f; point.handRoll = -9.05f;
    joint(point, "l_thumb1_jnt", 47, -26, -11);
    joint(point, "l_pinky1_jnt", 0, -77, 0);  joint(point, "l_pinky2_jnt", 0, -82, 0);  joint(point, "l_pinky3_jnt", 0, -84, 0);
    joint(point, "l_ring1_jnt", 0, -76, 0);   joint(point, "l_ring2_jnt", 0, -87, 0);   joint(point, "l_ring3_jnt", 0, -90, 0);
    joint(point, "l_middle1_jnt", 0, -79, 0); joint(point, "l_middle2_jnt", 0, -84, 0); joint(point, "l_middle3_jnt", 0, -79, 0);
    joint(point, "l_thumb2_jnt", 0, -55, 0);  joint(point, "l_thumb3_jnt", 0, -84, 0);
    HandPose rest;
    rest.name = "point_rest";
    rest.posX = -0.0404f; rest.posY = -0.0061f; rest.posZ = -0.0573f;
    rest.handPitch = 68.10f; rest.handYaw = 87.56f; rest.handRoll = -33.24f;
    joint(rest, "l_thumb1_jnt", 53, -10, -32);
    joint(rest, "l_pinky1_jnt", 0, 13, 0);   joint(rest, "l_pinky2_jnt", 0, -42, 0);  joint(rest, "l_pinky3_jnt", 0, -47, 0);
    joint(rest, "l_ring1_jnt", 0, 3, 0);     joint(rest, "l_ring2_jnt", 0, -39, 0);   joint(rest, "l_ring3_jnt", 0, -42, 0);
    joint(rest, "l_middle1_jnt", 0, -8, 0);  joint(rest, "l_middle2_jnt", 0, -50, 0); joint(rest, "l_middle3_jnt", 0, -53, 0);
    joint(rest, "l_thumb2_jnt", 0, -21, 8);  joint(rest, "l_thumb3_jnt", 0, -21, 0);
    joint(rest, "l_index2_jnt", 0, -16, 0);  joint(rest, "l_index3_jnt", 0, -16, 0);
    HandPose punch = point; // the fist: the pointing hand with the index curled too, held at a fixed spot ahead
    punch.name = "punch";
    punch.posY = 0.7482f;
    punch.absolutePos = 1;
    joint(punch, "l_index1_jnt", 0, -77, -11); joint(punch, "l_index2_jnt", 0, -55, 0); joint(punch, "l_index3_jnt", 0, -118, 0);
    poses.push_back(point);
    poses.push_back(rest);
    poses.push_back(punch);
    if (pStylePose)
    {
        pStylePose[0] = "point";
        pStylePose[1] = "";
        pStylePose[2] = "point_rest";
        pStylePose[3] = "punch";
    }
}

void ModMain::SavePoses()
{
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("HandPoses");
    root.append_attribute("comment") = "Support-hand poses for the interaction animation. pos_*: wrist offset from the reach target (m, view space: x right, y forward, z up). hand_*: wrist orientation in view space (degrees). Joint: rotation relative to the parent joint as an adjustment (degrees) on the skeleton's bind pose.";
    for (const HandPose& p : m_poses)
    {
        pugi::xml_node n = root.append_child("Pose");
        n.append_attribute("name") = p.name.c_str();
        n.append_attribute("pos_x") = p.posX;
        n.append_attribute("pos_y") = p.posY;
        n.append_attribute("pos_z") = p.posZ;
        n.append_attribute("absolute_pos") = p.absolutePos;
        n.append_attribute("hand_pitch") = p.handPitch;
        n.append_attribute("hand_yaw") = p.handYaw;
        n.append_attribute("hand_roll") = p.handRoll;
        for (const HandPoseJoint& j : p.joints)
        {
            if (!j.Adjusted()) continue;
            pugi::xml_node jn = n.append_child("Joint");
            jn.append_attribute("name") = j.name.c_str();
            jn.append_attribute("pitch") = j.pitch;
            jn.append_attribute("yaw") = j.yaw;
            jn.append_attribute("roll") = j.roll;
        }
    }
    for (int i = 0; i < 4; i++)
    {
        pugi::xml_node sn = root.append_child("Style");
        sn.append_attribute("name") = i == 1 ? "grab" : (i == 2 ? "rest" : (i == 3 ? "punch" : "press"));
        sn.append_attribute("pose") = m_stylePose[i].c_str();
        sn.append_attribute("amount") = m_stylePoseAmount[i];
    }
    pugi::xml_node rules = root.append_child("Rules");
    rules.append_attribute("comment") = "Which animation an interaction gets; first match wins. type / mode: EArkInteractionType / EArkInteractionMode, -1 = any. class / text: substring of the entity class / prompt text, case-insensitive. style: 0 press, 1 grab, 2 none. hover: 0 = no hovering hand for it.";
    for (const InteractRule& r : m_rules)
    {
        pugi::xml_node rn = rules.append_child("Rule");
        rn.append_attribute("type") = r.type;
        rn.append_attribute("mode") = r.mode;
        rn.append_attribute("class") = r.classContains.c_str();
        rn.append_attribute("text") = r.textContains.c_str();
        rn.append_attribute("style") = r.style;
        rn.append_attribute("hover") = r.hover;
        rn.append_attribute("note") = r.note.c_str();
    }
    const fs::path path = GetPosesPath();
    if (!doc.save_file(path.c_str()))
        CryError("ViewmodelTweaks: failed to save {}", path.u8string());
    else
        CryLog("ViewmodelTweaks: saved {} hand pose(s) to {}", m_poses.size(), path.u8string());
    m_posesDirty = false;
}

void ModMain::UpdateConvergence(float dt)
{
    const ViewmodelSettings& s = m_settings;

    const bool wantConverge = s.convergeEnabled != 0;
    const bool wantWall = s.wallPushEnabled != 0;
    const bool wantBlock = s.aimWallBlockEnabled != 0 && s.aimEnabled != 0;
    if (!Active() || (!wantConverge && !wantWall && !wantBlock) || !gEnv || !gEnv->pPhysicalWorld || !gEnv->pSystem)
    {
        m_convergeTargetYaw = m_convergeTargetPitch = 0.0f;
        m_convergeYaw = m_convergePitch = 0.0f;
        m_wallPush = m_wallPushTarget = 0.0f;
        m_wallBlend = 0.0f;
        m_convergeHit = false;
        m_aimBlockedByWall = false;
        return;
    }

    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
        return;
    IPhysicalEntity* pSkip = pPlayer->GetEntity() ? pPlayer->GetEntity()->GetPhysics() : nullptr;

    // Previous frame's rendered camera is plenty accurate for a smoothed convergence target.
    const Matrix34 cam = gEnv->pSystem->GetViewCamera().GetMatrix();
    const Vec3 camPos = cam.GetTranslation();
    const Vec3 dir = cam.GetColumn1().GetNormalized(); // CryEngine forward = +Y

    const float maxDist = clamp_tpl(s.convergeMaxDist, 1.0f, 200.0f);
    PaddedRayHit hit; // padded: the game's ray_hit may be larger than the SDK's
    const int n = gEnv->pPhysicalWorld->RayWorldIntersection(camPos, dir * maxDist, ent_all,
        rwi_stop_at_pierceable | rwi_colltype_any(geom_colltype_ray | geom_colltype0 | geom_colltype_player), &hit, 1, pSkip);
    m_convergeHit = (n > 0);
    m_convergeDist = (m_convergeHit && Finite(hit.dist)) ? hit.dist : maxDist;

    // --- Convergence: point the barrel at the impact point --------------------------------------
    float yaw = 0.0f, pitch = 0.0f;
    if (wantConverge)
    {
        // Weapon offset from the camera in view space (exact, measured at render time while not aiming).
        const Vec3 w = (m_render.hipValid && Finite(m_render.hipRelCam.t)) ? m_render.hipRelCam.t : Vec3(0.0f, 0.25f, -0.1f);
        const float d = max((Finite(m_convergeDist) ? m_convergeDist : 100.0f) - w.y, 0.15f); // distance from roughly the weapon to the target
        yaw = RAD2DEG(atan2f(w.x, d));   // weapon right of the eye -> turn left (+yaw)
        pitch = RAD2DEG(atan2f(-w.z, d)); // weapon below the eye -> tilt up (+pitch)
        const float maxA = clamp_tpl(s.convergeMaxAngle, 0.0f, 45.0f);
        yaw = clamp_tpl(yaw, -maxA, maxA) * clamp_tpl(s.convergeStrength, 0.0f, 1.0f);
        pitch = clamp_tpl(pitch, -maxA, maxA) * clamp_tpl(s.convergeStrength, 0.0f, 1.0f);
    }
    m_convergeTargetYaw = yaw;
    m_convergeTargetPitch = pitch;
    {
        const float tau = max(s.convergeSmoothTime, 0.0f);
        const float k = (tau > 0.0005f) ? (1.0f - expf(-dt / tau)) : 1.0f;
        m_convergeYaw += (yaw - m_convergeYaw) * k;
        m_convergePitch += (pitch - m_convergePitch) * k;
        if (!Finite(m_convergeYaw) || !Finite(m_convergePitch))
        {
            m_convergeYaw = m_convergePitch = 0.0f;
            m_nanRecoveries++;
        }
    }

    // --- Wall pull-back: move the weapon towards the camera when something is close ------------
    float push = 0.0f;
    if (wantWall && m_convergeHit)
    {
        const WeaponSettings* pW = FindCurrentWeapon();
        const float amount = pW ? pW->wallPush : WeaponSettings().wallPush;
        const float startD = max(s.wallPushStartDist, 0.05f);
        const float fullD = clamp_tpl(s.wallPushFullDist, 0.0f, startD - 0.01f);
        // 0 at startD, 1 at fullD (and closer), eased so it does not kick in abruptly.
        const float t = SmoothStep01((startD - m_convergeDist) / (startD - fullD));
        push = clamp_tpl(amount, 0.0f, 0.5f) * t;
    }
    m_wallPushTarget = push;
    {
        const float tau = max(s.wallPushSmoothTime, 0.0f);
        const float k = (tau > 0.0005f) ? (1.0f - expf(-dt / tau)) : 1.0f;
        m_wallPush += (push - m_wallPush) * k;
        if (!Finite(m_wallPush))
        {
            m_wallPush = 0.0f;
            m_nanRecoveries++;
        }
    }

    // --- Near-wall pose blend: how much of its full pull-back the weapon has used (0 = free, 1 = against the
    // wall), remapped through the start/full biases and eased. Inherits the pull-back smoothing.
    {
        float blend = 0.0f;
        if (wantWall && s.wallPoseEnabled)
        {
            const WeaponSettings* pW = FindCurrentWeapon();
            const float amount = clamp_tpl(pW ? pW->wallPush : WeaponSettings().wallPush, 0.0f, 0.5f);
            if (amount > 0.001f)
            {
                const float frac = clamp_tpl(m_wallPush / amount, 0.0f, 1.0f);
                const float start = clamp_tpl(s.wallPoseStart, 0.0f, 0.99f);
                const float full = clamp_tpl(s.wallPoseFull, start + 0.01f, 1.0f);
                blend = SmoothStep01((frac - start) / (full - start)) * clamp_tpl(pW ? pW->wallPoseAmount : 1.0f, 0.0f, 1.0f);
            }
        }
        // Looking steeply up or down the pose stops making sense (the wall is no longer where the muzzle would
        // go); give way to the plain pull-back with the camera pitch.
        m_camPitchDeg = RAD2DEG(asinf(clamp_tpl(dir.z, -1.0f, 1.0f)));
        {
            const float a0 = clamp_tpl(s.wallPosePitchStart, 0.0f, 89.0f);
            const float a1 = clamp_tpl(s.wallPosePitchFull, a0 + 0.5f, 90.0f);
            const float t = SmoothStep01((fabsf(m_camPitchDeg) - a0) / (a1 - a0));
            m_wallPitchFade = 1.0f - t * clamp_tpl(s.wallPosePitchStrength, 0.0f, 1.0f);
            if (!Finite(m_wallPitchFade)) m_wallPitchFade = 1.0f;
        }
        blend *= m_wallPitchFade;
        m_wallBlend = Finite(blend) ? blend : 0.0f;
    }

    // --- Aim block: no ironsights while the weapon would be poking into the wall -----------------
    // Reach estimate = how far the weapon sits in front of the eye + its wall pull-back (longer
    // weapons have more), times the user's tolerance. Small hysteresis so it cannot flicker.
    if (wantBlock)
    {
        const WeaponSettings* pW = FindCurrentWeapon();
        const float reach = max(m_render.hipRelCam.t.y, 0.15f) + (pW ? pW->wallPush : WeaponSettings().wallPush);
        m_aimBlockDist = reach * clamp_tpl(s.aimWallBlockScale, 0.1f, 5.0f);
        if (!m_convergeHit)
            m_aimBlockedByWall = false;
        else if (m_aimBlockedByWall)
            m_aimBlockedByWall = m_convergeDist < m_aimBlockDist * 1.1f;
        else
            m_aimBlockedByWall = m_convergeDist < m_aimBlockDist;
    }
    else
    {
        m_aimBlockedByWall = false;
    }
}

float ModMain::GetAimSensitivityMultiplier(float currentMultiplier, float zoomedMultiplier)
{
    // Only while our own aim zoom is active, and only for the camera-speed call (the same function
    // also blends the walk-speed multiplier; we tell the two apart by the weapon's zoomed camera stat).
    if (m_cameraZoomHandle == 0 || m_settings.aimSensMode == 0)
        return 0.0f;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    CArkWeapon* pWeapon = pPlayer ? pPlayer->m_weaponComponent.GetEquippedWeapon() : nullptr;
    if (!pWeapon)
        return 0.0f;
    if (fabsf(pWeapon->GetZoomedCameraSpeedStat() - zoomedMultiplier) > 1e-6f)
        return 0.0f; // walk speed call
    m_sensHookCalls++;
    const float scale = (m_settings.aimSensMode == 1) ? clamp_tpl(m_settings.aimCameraZoomFactor, 0.2f, 1.5f)
                                                       : clamp_tpl(m_settings.aimSensScale, 0.05f, 3.0f);
    return currentMultiplier * scale;
}

float ModMain::GetSpreadMultiplier(const CArkItem* pWeapon)
{
    if (!Active() || !pWeapon)
        return 1.0f;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !pPlayer->GetEntity() || pWeapon->GetOwnerId() != pPlayer->GetEntity()->GetId())
        return 1.0f; // not the player's weapon
    m_spreadHookCalls++;
    // Per-weapon factors (the pistol and the shotgun share the game's "shotgun" weapon class).
    IEntity* pEnt = pWeapon->GetEntity();
    if (!pEnt || !pEnt->GetClass())
        return 1.0f;
    const WeaponSettings* pW = FindWeapon(pEnt->GetClass()->GetName());
    if (!pW)
        return 1.0f;
    const float ab = m_settings.aimEnabled ? SmoothStep01(m_aimBlend) : 0.0f;
    const float hip = clamp_tpl(pW->hipSpreadMult, 0.0f, 5.0f);
    const float ads = clamp_tpl(pW->aimSpreadMult, 0.0f, 5.0f);
    const float m = LERP(hip, ads, ab);
    return Finite(m) ? clamp_tpl(m, 0.0f, 5.0f) : 1.0f;
}

//! One log line per shot with everything that decides the pellet pattern. The shotgun and the pistol share
//! CArkWeaponShotgun: pellets are laid out in a grid spanning the "ShotgunSpreadConeDegrees" cone around an
//! aim point, and that aim point is either the exact camera target ("accurate shot", which the shotgun's
//! fAccurateShotChance=1 normally guarantees) or a random point inside the current dispersion. "aim off-axis"
//! is the angle between the camera axis and the aim point: ~0 means the shot was straight and the pattern is
//! the cone alone; anything larger means the dispersion is live and adds to the pattern.
void ModMain::OnSpawnPellets(const void* pWeapon, const Vec3& position, const Vec3& aimPoint, bool bShootStraight)
{
    if (!m_settings.spreadDebug || !pWeapon || !gEnv || !gEnv->pSystem)
        return;
    const CArkWeaponShotgun* w = static_cast<const CArkWeaponShotgun*>(pWeapon);
    const Matrix34 cam = gEnv->pSystem->GetViewCamera().GetMatrix();
    const Vec3 camPos = cam.GetTranslation();
    const Vec3 fwd = cam.GetColumn1().GetNormalized();
    const Vec3 toAim = aimPoint - camPos;
    const float dist = toAim.GetLength();
    const float offDeg = (dist > 0.001f) ? RAD2DEG(acosf(clamp_tpl(fwd.Dot(toAim / dist), -1.0f, 1.0f))) : 0.0f;
    const unsigned outcome = *reinterpret_cast<const unsigned*>(reinterpret_cast<const char*>(pWeapon) + 0x4D0);
    // ArkStats lives at weapon+0x1A8 as { uint ownerId; uint nextModifierId; ... }; the second word counts every
    // stat modifier ever applied to this weapon, so it shows at a glance whether a weapon mod's modifiers have
    // been applied more than once (they are additive and appended to a list, so re-application stacks).
    const unsigned statMods = *reinterpret_cast<const unsigned*>(reinterpret_cast<const char*>(pWeapon) + 0x1AC);
    CryLog("ViewmodelTweaks[spread] {}: cone {:.3f}->{:.3f} deg | pellets {}x{} | stat modifiers applied {} | "
           "disp min {:.3f}->{:.3f} max {:.3f}->{:.3f} cur {:.3f} | accuracy outcome 0x{:08X} | "
           "target off-axis {:.3f} deg at {:.2f} m | straight {} | mult {:.3f} (aim blend {:.2f})",
        m_currentWeaponClass, s_dbgConeOrig, s_dbgConeOut,
        s_dbgStatInt[0], s_dbgStatInt[1], statMods,
        s_dbgDispMinOrig, s_dbgDispMinOut, s_dbgDispMaxOrig, s_dbgDispMaxOut, w->m_weaponDispersion,
        outcome, offDeg, dist, (int)bShootStraight,
        GetSpreadMultiplier(reinterpret_cast<const CArkItem*>(pWeapon)),
        m_settings.aimEnabled ? SmoothStep01(m_aimBlend) : 0.0f);
}

//---------------------------------------------------------------------------------
// State
//---------------------------------------------------------------------------------
void ModMain::UpdateBlendStates(float dt)
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();

    bool crouching = false;
    bool aiming = false;

    if (pPlayer)
    {
        // Dead (ragdoll / death model) or just revived (model reloaded, attachments recreated):
        // keep our hands off the skeleton and the weapon attachment for a moment.
        const bool dead = pPlayer->m_playerComponent.GetHealthComponent().IsDead();
        if (m_playerDead && !dead)
            m_reviveGuard = 0.75f;
        m_playerDead = dead;
        if (m_reviveGuard > 0.0f)
            m_reviveGuard -= dt;
        if (dead)
            m_aimKeyHeld = m_settings.aimToggle ? false : m_aimKeyHeld;

        const EStance stance = pPlayer->m_stance;
        crouching = (stance == EStance::STANCE_SNEAK || stance == EStance::STANCE_CRAWL);
        // Zero-G: the thruster boost reports as sprinting, but there is no running body to lower the weapon
        // for, so it only counts as a sprint if the user asks for it.
        m_feel.zeroG = pPlayer->m_movementFSM.IsInZeroG();
        m_feel.sprinting = !dead && pPlayer->m_movementFSM.IsSprinting() && (!m_feel.zeroG || m_settings.sprintInZeroG);

        m_currentWeaponClass = GetWeaponClassName(pPlayer);

        // Weapon state: reloading, and "switching" = holster / draw in progress or any weapon action that
        // leaves the weapon not ready (select, reload action, charge release).
        {
            const ArkPlayerWeaponComponent& wc = pPlayer->m_weaponComponent;
            CArkWeapon* pWeapon = wc.GetEquippedWeapon();
            if (pWeapon && pWeapon->GetOwnerId() != pPlayer->GetEntity()->GetId())
                pWeapon = nullptr;
            m_wsReloading = pWeapon && pWeapon->m_bIsReloading;
            // "Ready to attack" is cleared for select, reload AND every fire action (pump / bolt / beam cycle),
            // so it cannot be used on its own; it only tells us when a freshly drawn weapon has finished coming up.
            m_wsReady = !pWeapon || pWeapon->m_bIsReadyToAttack;
            m_wsUnequipping = (pWeapon && pWeapon->m_bIsUnequipping) || wc.m_bIsUnequipping
                || (wc.m_toBeEquippedWeaponId != 0 && wc.m_toBeEquippedWeaponId != wc.m_equippedWeaponId);
            const unsigned curId = pWeapon ? wc.m_equippedWeaponId : 0u;
            if (curId != m_wsLastWeaponId)
            {
                m_wsLastWeaponId = curId;
                m_wsDrawing = curId != 0; // a new weapon: its draw animation is playing until it reports ready
                m_wsDrawTimer = 0.0f;
            }
            if (m_wsDrawing)
            {
                // The ready flag comes back at equip time, before the raise animation ends, so it is only a lower
                // bound: also wait out the configured draw time (and never more than a few seconds in any case).
                m_wsDrawTimer += dt;
                const float minHold = clamp_tpl(m_settings.aimSwitchDelay, 0.0f, 3.0f);
                if ((m_wsReady && m_wsDrawTimer >= minHold) || m_wsDrawTimer > max(3.0f, minHold))
                    m_wsDrawing = false;
            }
            m_wsSwitching = m_wsUnequipping || m_wsDrawing;
        }

        // Aim key held/toggled, weapon allows aiming, and the player is actually controlling the
        // character (no cursor on screen: menus, inventory, our own settings window...).
        const WeaponSettings* pW = FindCurrentWeapon();
        const bool weaponAllows = !m_currentWeaponClass.empty() && (!pW || pW->aimAllowed);
        aiming = Active() && m_settings.aimEnabled && m_aimKeyHeld && weaponAllows && !m_mouseCaptured && !IsHardwareCursorVisible()
                 && !(m_settings.aimWallBlockEnabled && m_aimBlockedByWall) && !dead && m_reviveGuard <= 0.0f
                 && !(m_settings.sprintPoseEnabled && m_settings.sprintBlocksAim && m_feel.sprinting)
                 && !(m_settings.aimBlockReload && m_wsReloading)
                 && !(m_settings.aimBlockSwitch && m_wsSwitching);
    }
    else
    {
        m_aimKeyHeld = false;
        m_currentWeaponClass.clear();
        m_feel.sprinting = false;
        m_feel.zeroG = false;
        m_wsReloading = m_wsUnequipping = m_wsSwitching = m_wsDrawing = false;
        m_wsReady = true;
        m_wsLastWeaponId = 0;
    }

    if (!aiming && m_settings.aimToggle && (IsHardwareCursorVisible() || !pPlayer))
        m_aimKeyHeld = false; // drop a stale toggle when a menu opens

    m_isCrouching = crouching;
    m_isAiming = aiming;

    UpdateCameraZoom(aiming);

    MoveTowards(m_crouchBlend, crouching ? 1.0f : 0.0f, m_settings.crouchTime, dt);
    MoveTowards(m_aimBlend, aiming ? 1.0f : 0.0f, m_settings.aimTime, dt);
    MoveTowards(m_reloadFade, (m_settings.reloadFadesOffsets && m_wsReloading && Active()) ? 1.0f : 0.0f, m_settings.reloadFadeTime, dt);
    if (!Finite(m_crouchBlend)) { m_crouchBlend = 0.0f; m_nanRecoveries++; }
    if (!Finite(m_aimBlend)) { m_aimBlend = 0.0f; m_nanRecoveries++; }
    if (!Finite(m_reloadFade)) { m_reloadFade = 0.0f; m_nanRecoveries++; }
}

//---------------------------------------------------------------------------------
// Feel: sprint pose, aim sway & settle, view drag. Pure state, evaluated once per frame; the outputs
// are added in ApplyOffset (hip path) and ComputeAimLocal (aim path).
//---------------------------------------------------------------------------------
static inline float WrapAngle(float a)
{
    while (a > gf_PI) a -= 2.0f * gf_PI;
    while (a < -gf_PI) a += 2.0f * gf_PI;
    return a;
}

void ModMain::UpdateFeel(float dt, ArkPlayer* pPlayer)
{
    const ViewmodelSettings& s = m_settings;
    FeelState& f = m_feel;
    dt = clamp_tpl(dt, 0.0f, 0.1f);
    if (!f.lastLookValid)
        f.steadyLeft = s.steadyDuration; // first frame (or after a reset): a full breath

    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
    if (!pEnt)
    {
        f = FeelState();
        f.steadyLeft = s.steadyDuration;
        return;
    }

    // --- Player speed (horizontal) ---
    f.speed = 0.0f;
    if (IPhysicalEntity* pPhys = pEnt->GetPhysics())
    {
        PreyStatusLiving living; // NOT pe_status_living: see the struct's comment
        if (pPhys->GetStatus(&living) && Finite(living.vel))
            f.speed = Vec2(living.vel.x, living.vel.y).GetLength();
    }
    f.speedNorm = clamp_tpl(f.speed / 3.0f, 0.0f, 1.5f);

    // --- Sprint blend and phase ---
    if (f.sprinting)
        f.timeSinceSprint = 0.0f;
    else
        f.timeSinceSprint += dt;
    const bool wantSprintPose = s.sprintPoseEnabled && f.sprinting;
    MoveTowards(f.sprintBlend, wantSprintPose ? 1.0f : 0.0f, wantSprintPose ? s.sprintBlendIn : s.sprintBlendOut, dt);
    f.sprintOut = PoseOffset();
    if (f.sprintBlend > 0.0f)
    {
        f.sprintOut.AddScaled(s.sprint, f.sprintBlend);
        if (s.sprintSwayEnabled)
        {
            f.sprintPhase = fmodf(f.sprintPhase + 2.0f * gf_PI * max(s.sprintSwayFreq, 0.01f) * dt, 2.0f * gf_PI);
            const float k = f.sprintBlend;
            PoseOffset sw;
            sw.posX = s.sprintSwayPos * sinf(f.sprintPhase);
            sw.posZ = s.sprintSwayPos * 0.6f * sinf(2.0f * f.sprintPhase);
            sw.roll = s.sprintSwayRot * sinf(f.sprintPhase);
            sw.pitch = s.sprintSwayRot * 0.4f * sinf(2.0f * f.sprintPhase + 0.5f);
            f.sprintOut.AddScaled(sw, k);
        }
    }
    else
        f.sprintPhase = 0.0f;

    // --- Aim sway & settle ---
    f.swayOut = PoseOffset();
    if (m_aimBlend > 0.0f)
        f.aimTime += dt;
    else
        f.aimTime = 0.0f;
    // Breath: held only while aiming and with breath left; recovers otherwise.
    const bool wantSteady = s.steadyEnabled && f.steadyHeld && m_isAiming;
    if (wantSteady && f.steadyLeft > 0.0f)
    {
        f.steadyActive = true;
        f.steadyLeft = max(0.0f, f.steadyLeft - dt);
    }
    else
    {
        f.steadyActive = false;
        if (s.steadyRecover > 0.01f)
            f.steadyLeft = min(s.steadyDuration, f.steadyLeft + dt * s.steadyDuration / s.steadyRecover);
        else
            f.steadyLeft = s.steadyDuration;
    }
    if (s.aimSwayEnabled && m_aimBlend > 0.0f)
    {
        f.swayPhase = fmodf(f.swayPhase + 2.0f * gf_PI * max(s.aimSwayFreq, 0.01f) * dt, 2.0f * gf_PI);
        float amp = 1.0f;
        if (s.aimSwaySettleTime > 0.01f)
            amp *= 1.0f + (max(s.aimSwayInitial, 1.0f) - 1.0f) * expf(-f.aimTime / s.aimSwaySettleTime);
        amp *= 1.0f + s.aimSwayMoveMult * f.speedNorm;
        if (s.aimSwaySprintRecover > 0.01f)
            amp *= 1.0f + s.aimSwaySprintPenalty * expf(-f.timeSinceSprint / s.aimSwaySprintRecover);
        if (f.steadyActive)
            amp *= 1.0f - clamp_tpl(s.steadyReduce, 0.0f, 1.0f);
        f.swayAmplitude = amp;
        const float ph = f.swayPhase;
        // Figure-eight: slow axis sideways, twice the rate vertically; the rotation is what moves the sights.
        f.swayOut.posX = s.aimSwayPos * amp * sinf(ph);
        f.swayOut.posZ = s.aimSwayPos * amp * 0.5f * sinf(2.0f * ph + 1.57f);
        f.swayOut.yaw = s.aimSwayRot * amp * sinf(ph + 0.3f);
        f.swayOut.pitch = s.aimSwayRot * amp * 0.6f * sinf(2.0f * ph);
    }
    else
    {
        f.swayAmplitude = 0.0f;
        if (m_aimBlend <= 0.0f)
            f.swayPhase = 0.0f;
    }

    // --- View drag (spring on the turn rate) ---
    f.dragHipOut = PoseOffset();
    f.dragAimOut = PoseOffset();
    {
        const Quat camQ = pPlayer->m_camera.m_rotation;
        const bool lookOk = camQ.IsValid();
        const Ang3 look = lookOk ? SafeAng3(camQ) : Ang3(ZERO);
        const float yaw = look.z, pitch = look.x;
        float dYaw = 0.0f, dPitch = 0.0f;
        if (lookOk && f.lastLookValid && dt > 0.0f)
        {
            dYaw = WrapAngle(yaw - f.lastYaw);
            dPitch = WrapAngle(pitch - f.lastPitch);
        }
        f.lastYaw = yaw; f.lastPitch = pitch; f.lastLookValid = lookOk;
        // Ignore the one-frame jumps of teleports / loads (and anything that is not a number).
        if (!(fabsf(dYaw) < DEG2RAD(60.0f)) || !(fabsf(dPitch) < DEG2RAD(60.0f)) || dt <= 0.0f)
            f.yawRate = f.pitchRate = 0.0f;
        else
        {
            f.yawRate = dYaw / dt;
            f.pitchRate = dPitch / dt;
        }
        if (!(fabsf(f.dragX) < 1e3f) || !(fabsf(f.dragY) < 1e3f) || !(fabsf(f.dragVX) < 1e5f) || !(fabsf(f.dragVY) < 1e5f))
            f.dragX = f.dragY = f.dragVX = f.dragVY = 0.0f; // never let a bad frame poison the spring

        // The spring always runs (towards zero when disabled) so toggling the feature fades instead of snapping.
        {
            const float w = clamp_tpl(s.dragStiffness, 0.5f, 60.0f);
            const float c = 2.0f * clamp_tpl(s.dragDamping, 0.1f, 2.0f) * w;
            // Sub-step for stability at low frame rates.
            const int n = max(1, (int)ceilf(dt / 0.008f));
            const float h = dt / n;
            const float tx = s.dragEnabled ? f.yawRate : 0.0f;
            const float ty = s.dragEnabled ? f.pitchRate : 0.0f;
            for (int i = 0; i < n; i++)
            {
                const float a1 = w * w * (tx - f.dragX) - c * f.dragVX;
                f.dragVX += a1 * h; f.dragX += f.dragVX * h;
                const float a2 = w * w * (ty - f.dragY) - c * f.dragVY;
                f.dragVY += a2 * h; f.dragY += f.dragVY * h;
            }
            const float sign = s.dragLead ? 1.0f : -1.0f;
            // Turning left = positive yaw rate. Leading: the weapon moves/turns left with it.
            PoseOffset d;
            d.posX = clamp_tpl(-sign * f.dragX * s.dragPos, -s.dragMaxPos, s.dragMaxPos);
            d.posZ = clamp_tpl(sign * f.dragY * s.dragPos * s.dragPitchScale, -s.dragMaxPos, s.dragMaxPos);
            d.yaw = clamp_tpl(sign * f.dragX * s.dragRot, -s.dragMaxRot, s.dragMaxRot);
            d.pitch = clamp_tpl(sign * f.dragY * s.dragRot * s.dragPitchScale, -s.dragMaxRot, s.dragMaxRot);
            f.dragHipOut = d;
            f.dragAimOut = PoseOffset();
            f.dragAimOut.AddScaled(d, clamp_tpl(s.dragAimScale, 0.0f, 1.0f));
        }
    }
}

void ModMain::SanitizeSettings()
{
    // Persisted numbers (cvars, weapons.xml) are the one place a bad value could come back session after
    // session; anything that is not a number goes back to its default.
    ViewmodelSettings& s = m_settings;
    const ViewmodelSettings def;
    int fixed = 0;
    auto fixF = [&](float& v, float d) { if (!Finite(v)) { v = d; fixed++; } };
    fixF(s.crouchTime, def.crouchTime); fixF(s.reloadFadeTime, def.reloadFadeTime); fixF(s.aimSwitchDelay, def.aimSwitchDelay); fixF(s.aimBobAmount, def.aimBobAmount); fixF(s.aimBobTau, def.aimBobTau);
    fixF(s.aimAnimRecoil, def.aimAnimRecoil); fixF(s.aimAnimSway, def.aimAnimSway); fixF(s.aimSensScale, def.aimSensScale);
    fixF(s.aimTime, def.aimTime); fixF(s.aimFov, def.aimFov); fixF(s.aimCameraZoomFactor, def.aimCameraZoomFactor);
    fixF(s.nudgePosSpeed, def.nudgePosSpeed); fixF(s.nudgeRotSpeed, def.nudgeRotSpeed); fixF(s.reticleY, def.reticleY);
    fixF(s.convergeStrength, def.convergeStrength); fixF(s.convergeMaxAngle, def.convergeMaxAngle);
    fixF(s.convergeSmoothTime, def.convergeSmoothTime); fixF(s.convergeMaxDist, def.convergeMaxDist);
    fixF(s.aimWallBlockScale, def.aimWallBlockScale); fixF(s.wallPushStartDist, def.wallPushStartDist);
    fixF(s.wallPushFullDist, def.wallPushFullDist); fixF(s.wallPushSmoothTime, def.wallPushSmoothTime);
    fixF(s.wallPoseStart, def.wallPoseStart); fixF(s.wallPoseFull, def.wallPoseFull); fixF(s.wallPoseConvergeFade, def.wallPoseConvergeFade);
    fixF(s.wallPosePitchStart, def.wallPosePitchStart); fixF(s.wallPosePitchFull, def.wallPosePitchFull); fixF(s.wallPosePitchStrength, def.wallPosePitchStrength);
    fixF(s.sprintBlendIn, def.sprintBlendIn); fixF(s.sprintBlendOut, def.sprintBlendOut);
    fixF(s.sprintSwayPos, def.sprintSwayPos); fixF(s.sprintSwayRot, def.sprintSwayRot); fixF(s.sprintSwayFreq, def.sprintSwayFreq);
    fixF(s.aimSwayPos, def.aimSwayPos); fixF(s.aimSwayRot, def.aimSwayRot); fixF(s.aimSwayFreq, def.aimSwayFreq);
    fixF(s.aimSwayInitial, def.aimSwayInitial); fixF(s.aimSwaySettleTime, def.aimSwaySettleTime); fixF(s.aimSwayMoveMult, def.aimSwayMoveMult);
    fixF(s.aimSwaySprintPenalty, def.aimSwaySprintPenalty); fixF(s.aimSwaySprintRecover, def.aimSwaySprintRecover);
    fixF(s.steadyReduce, def.steadyReduce); fixF(s.steadyDuration, def.steadyDuration); fixF(s.steadyRecover, def.steadyRecover);
    fixF(s.dragPos, def.dragPos); fixF(s.dragRot, def.dragRot); fixF(s.dragStiffness, def.dragStiffness); fixF(s.dragDamping, def.dragDamping);
    fixF(s.dragMaxPos, def.dragMaxPos); fixF(s.dragMaxRot, def.dragMaxRot); fixF(s.dragAimScale, def.dragAimScale); fixF(s.dragPitchScale, def.dragPitchScale);
    fixF(s.worldFov, def.worldFov); fixF(s.sprintSensScale, def.sprintSensScale); fixF(s.fov, def.fov);
    fixF(s.interactFireDelay, def.interactFireDelay); fixF(s.interactMaxForward, def.interactMaxForward); fixF(s.interactMinForward, def.interactMinForward);
    fixF(s.interactMaxSide, def.interactMaxSide); fixF(s.interactMaxUp, def.interactMaxUp); fixF(s.interactMaxDown, def.interactMaxDown);
    fixF(s.interactCorrGain, def.interactCorrGain); fixF(s.interactTestX, def.interactTestX); fixF(s.interactExamMaxForward, def.interactExamMaxForward);
    fixF(s.interactExamFov, def.interactExamFov); fixF(s.interactExamArmLength, def.interactExamArmLength); fixF(s.interactExamMinTargetDist, def.interactExamMinTargetDist); fixF(s.interactExamBodyX, def.interactExamBodyX); fixF(s.interactExamBodyY, def.interactExamBodyY); fixF(s.interactExamBodyZ, def.interactExamBodyZ);
    if (s.examCorr.Sanitize(def.examCorr)) fixed++;
    fixF(s.interactRestExamX, def.interactRestExamX); fixF(s.interactRestExamY, def.interactRestExamY); fixF(s.interactRestExamZ, def.interactRestExamZ);
    fixF(s.interactRestX, def.interactRestX); fixF(s.interactRestY, def.interactRestY); fixF(s.interactRestZ, def.interactRestZ); fixF(s.interactRestBlendTime, def.interactRestBlendTime); fixF(s.interactTestY, def.interactTestY); fixF(s.interactTestZ, def.interactTestZ);
    auto fixStyle = [&](ReachStyle& r, const ReachStyle& d) {
        fixF(r.reachTime, d.reachTime); fixF(r.holdTime, d.holdTime); fixF(r.returnTime, d.returnTime); fixF(r.amount, d.amount);
        fixF(r.offX, d.offX); fixF(r.offY, d.offY); fixF(r.offZ, d.offZ); fixF(r.arcX, d.arcX); fixF(r.arcZ, d.arcZ);
        fixF(r.retArcX, d.retArcX); fixF(r.retArcZ, d.retArcZ); fixF(r.pitch, d.pitch); fixF(r.yaw, d.yaw); fixF(r.roll, d.roll);
        fixF(r.envelopeScale, d.envelopeScale); fixF(r.along, d.along);
        fixF(r.windupTime, d.windupTime); fixF(r.windX, d.windX); fixF(r.windY, d.windY); fixF(r.windZ, d.windZ);
        fixF(r.windShX, d.windShX); fixF(r.windShY, d.windShY); fixF(r.windShZ, d.windShZ); fixF(r.windElX, d.windElX); fixF(r.windElY, d.windElY); fixF(r.windElZ, d.windElZ);
        fixF(r.windFaPitch, d.windFaPitch); fixF(r.windFaYaw, d.windFaYaw); fixF(r.windFaRoll, d.windFaRoll); fixF(r.windKeep, d.windKeep);
    };
    fixStyle(s.press, def.press);
    fixStyle(s.grab, def.grab);
    fixStyle(s.pressExam, def.pressExam);
    fixStyle(s.punch, def.punch);
    if (s.meleeLower.Sanitize(def.meleeLower)) fixed++;
    if (s.interactStartShoulder.Sanitize(def.interactStartShoulder)) fixed++;
    if (s.interactStartElbow.Sanitize(def.interactStartElbow)) fixed++;
    fixF(s.meleeImpulseScale, def.meleeImpulseScale); fixF(s.meleeLowerTime, def.meleeLowerTime);
    fixF(s.meleeDamage, def.meleeDamage); fixF(s.meleeCooldown, def.meleeCooldown); fixF(s.meleeCamKick, def.meleeCamKick); fixF(s.meleeCamKickYaw, def.meleeCamKickYaw); fixF(s.meleeCamKickTime, def.meleeCamKickTime);
    fixF(s.interactCarryHoldTime, def.interactCarryHoldTime); fixF(s.interactStartX, def.interactStartX); fixF(s.interactStartY, def.interactStartY); fixF(s.interactStartZ, def.interactStartZ);
    fixF(s.interactExamLeaveTime, def.interactExamLeaveTime); fixF(s.interactRestSwayPos, def.interactRestSwayPos); fixF(s.interactRestSwayRot, def.interactRestSwayRot);
    fixF(s.interactRestSwayFreq, def.interactRestSwayFreq); fixF(s.interactHoverMaxDist, def.interactHoverMaxDist); fixF(s.interactHoverTowards, def.interactHoverTowards);
    if (s.base.Sanitize(def.base)) fixed++;
    if (s.crouch.Sanitize(def.crouch)) fixed++;
    if (s.sprint.Sanitize(def.sprint)) fixed++;
    for (auto& kv : m_weapons)
    {
        WeaponSettings& w = kv.second;
        const WeaponSettings* pB = WeaponSettings::BuiltIn(kv.first.c_str());
        WeaponSettings d;
        if (pB) d = *pB; else d.aim = WeaponSettings::DefaultAim();
        if (w.hip.Sanitize(d.hip)) fixed++;
        if (w.aim.Sanitize(d.aim)) fixed++;
        if (w.wall.Sanitize(d.wall)) fixed++;
        if (w.interact.Sanitize(d.interact)) fixed++;
        if (w.interactRest.Sanitize(d.interactRest)) fixed++;
        if (w.interactForearm.Sanitize(d.interactForearm)) fixed++;
        if (w.interactStart.Sanitize(d.interactStart)) fixed++;
        if (w.interactStartShoulder.Sanitize(d.interactStartShoulder)) fixed++;
        if (w.interactStartElbow.Sanitize(d.interactStartElbow)) fixed++;
        fixF(w.wallPush, d.wallPush); fixF(w.wallPoseAmount, d.wallPoseAmount); fixF(w.fireCoupling, d.fireCoupling); fixF(w.fireCouplingTime, d.fireCouplingTime);
        fixF(w.aimRecoilScale, d.aimRecoilScale); fixF(w.aimKickScale, d.aimKickScale); fixF(w.aimSpreadMult, d.aimSpreadMult); fixF(w.hipSpreadMult, d.hipSpreadMult);
    }
    if (fixed > 0)
    {
        CryLog("ViewmodelTweaks: {} persisted setting(s) were not numbers and were reset to defaults", fixed);
        m_nanRecoveries += fixed;
    }
}

void ModMain::SanitizeFeel()
{
    FeelState& f = m_feel;
    const bool ok = SanePose(f.sprintOut) && SanePose(f.swayOut) && SanePose(f.dragHipOut) && SanePose(f.dragAimOut)
        && Finite(f.sprintBlend) && Finite(f.sprintPhase) && Finite(f.swayPhase) && Finite(f.aimTime) && Finite(f.steadyLeft)
        && Finite(f.dragX) && Finite(f.dragY) && Finite(f.dragVX) && Finite(f.dragVY) && Finite(f.speed);
    if (!ok)
    {
        const float breath = Finite(f.steadyLeft) ? f.steadyLeft : m_settings.steadyDuration;
        f = FeelState();
        f.steadyLeft = breath;
        m_nanRecoveries++;
    }
}

QuatT ModMain::ComputeAimLocal() const
{
    // Weapon-local (post-multiplied) extras: rotate in place around the weapon's pivot so the sights move
    // off the crosshair, translate along the weapon's own axes.
    PoseOffset total;
    if (SanePose(m_feel.swayOut))
        total.AddScaled(m_feel.swayOut, 1.0f);      // both are zero when their feature is off
    if (SanePose(m_feel.dragAimOut))
        total.AddScaled(m_feel.dragAimOut, 1.0f);
    if (total.IsZero())
        return QuatT(IDENTITY);
    return total.AsQuatT();
}

void ModMain::UpdateCameraZoom(bool aiming)
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
    {
        m_cameraZoomHandle = 0; // the zoom manager is gone with the player
        return;
    }

    ArkPlayerZoomManager& zoom = pPlayer->m_zoomManager;
    const bool want = aiming && m_settings.aimCameraZoom != 0;
    SCVars* pCVars = *g_pGameCVars;
    const float baseHfov = (pCVars && pCVars->cl_hfov > 1.0f) ? pCVars->cl_hfov : 90.0f;
    const float factor = clamp_tpl(m_settings.aimCameraZoomFactor, 0.2f, 1.5f);
    const float hfov = baseHfov * factor;

    if (want && m_cameraZoomHandle == 0)
    {
        m_cameraZoomHandle = zoom.SetDesiredHFOV(hfov, m_settings.aimTime, false, EArkZoomPriority::high);
        m_cameraZoomHfov = hfov;
    }
    else if (want && fabsf(hfov - m_cameraZoomHfov) > 0.01f)
    {
        zoom.UpdateDesiredHFOV(m_cameraZoomHandle, hfov, 0.05f, false);
        m_cameraZoomHfov = hfov;
    }
    else if (!want && m_cameraZoomHandle != 0)
    {
        zoom.ClearDesiredHFOV(m_cameraZoomHandle, m_settings.aimTime, false);
        m_cameraZoomHandle = 0;
    }
}

//---------------------------------------------------------------------------------
// Built-in per-weapon defaults (tuned on the stock weapons; the user's weapons.xml overrides them)
//---------------------------------------------------------------------------------
namespace
{
    struct BuiltInWeapon
    {
        const char* cls;
        bool aimAllowed;
        float wallPush;
        float fireCoupling, fireCouplingTime, aimRecoilScale, aimKickScale;
        float aimSpread, hipSpread;
        PoseOffset hip;
        PoseOffset aim;
        PoseOffset wall;    //!< near-wall pose (blended in with the pull-back)
    };
    PoseOffset P(float x, float y, float z, float pitch = 0.0f, float yaw = 0.0f, float roll = 0.0f)
    {
        PoseOffset p;
        p.posX = x; p.posY = y; p.posZ = z; p.pitch = pitch; p.yaw = yaw; p.roll = roll;
        return p;
    }
    const BuiltInWeapon* BuiltInTable(size_t& count)
    {
        // Aim pose = weapon attachment relative to the camera (m / deg), x = 0 means centered on the crosshair.
        static const BuiltInWeapon table[] = {
            //  class                          aim    wall   fireC fireT recoil kick  aimSp hipSp  hip offset                                   aim pose                                      near-wall pose
            { "ArkWeaponPistol",               true,  0.066f, 1.0f, 0.40f, 3.0f, 0.30f, 0.45f, 1.0f, P(0, 0, 0),                                  P(0.0f, 0.250f, -0.1327f, 0.04f), P(0, -0.02f, 0, 12.0f) },
            { "ArkWeaponShotgun",              true,  0.134f, 1.0f, 0.75f, 1.0f, 0.15f, 0.45f, 1.0f, P(0, 0, -0.0323f, 3.23f),                    P(0.0f, 0.0575f, -0.0956f, 0.73f), P(0.01f, -0.05f, -0.02f, 42.0f, -4.0f, 6.0f) },
            { "ArkWeaponGooGun",               true,  0.157f, 0.35f, 0.30f, 1.0f, 0.20f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0.0f, 0.250f, -0.1707f), P(0, -0.04f, -0.01f, 28.0f, -3.0f) },
            { "ArkWeaponStunGun",              true,  0.060f, 0.35f, 0.30f, 1.0f, 0.25f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0.0f, 0.250f, -0.1487f), P(0, -0.02f, 0, 12.0f) },
            { "ArkWeaponToyGun",               true,  0.129f, 0.35f, 0.30f, 1.0f, 0.30f, 1.0f, 1.0f, P(0, -0.014f, -0.0403f, 4.84f, 5.44f),       P(0.0f, 0.0564f, -0.0954f), P(0, -0.04f, -0.01f, 26.0f, -3.0f) },
            { "ArkWeaponInstalaser",           true,  0.216f, 0.35f, 0.30f, 1.0f, 0.35f, 1.0f, 1.0f, P(0, 0, -0.0362f, 5.13f, 2.58f),             P(0.165f, -0.2295f, 0.0511f, 0.0f, 0.01f), P(0, -0.05f, -0.02f, 30.0f, -6.0f, 4.0f) },
            { "ArkWeaponWrench",               false, 0.026f, 0.35f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, P(-0.0177f, 0.0242f, 0.0398f, 4.83f, 0.0f, -5.99f), P(0, 0.25f, -0.06f), P(0, 0, 0) },
            { "ArkWeaponEMPGrenade",           false, 0.060f, 0.35f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0, 0.25f, -0.06f), P(0, 0, 0) },
            { "ArkWeaponLureGrenade",          false, 0.060f, 0.35f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0, 0.25f, -0.06f), P(0, 0, 0) },
            { "ArkWeaponRecyclerGrenade",      false, 0.000f, 0.35f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0, 0.25f, -0.06f), P(0, 0, 0) },
            { "ArkWeaponNullwaveTransmitter",  false, 0.060f, 0.35f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0, 0.25f, -0.06f), P(0, 0, 0) },
            { "ArkWeaponExplosiveGrenade",     false, 0.060f, 0.35f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, P(0, 0, 0),                                  P(0, 0.25f, -0.06f), P(0, 0, 0) },
        };
        count = sizeof(table) / sizeof(table[0]);
        return table;
    }
}

namespace
{
    //! Interaction reach correction per weapon (hand position, view space m), as lined up in play (3.10.3): the grip
    //! decides where the support hand starts, and with it how the fingertip sits relative to the wrist at the end.
    struct BuiltInInteract { const char* cls; float x, y, z, pitch, yaw, roll; float faPitch, faYaw, faRoll; };
    const BuiltInInteract s_builtInInteract[] = {
        //  class                           hand position            wrist rotation        forearm rotation
        { "ArkWeaponEMPGrenade",          0.0f,    -0.1429f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponGooGun",              0.0f,     0.0f,    0.0f,    0, 0, 0,              32.19f, -25.95f, 9.53f },
        { "ArkWeaponInstalaser",          0.0f,    -0.1429f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponLureGrenade",         0.0f,    -0.1485f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponNullwaveTransmitter", 0.0f,    -0.1493f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponPistol",              0.0f,    -0.1364f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponRecyclerGrenade",     0.0f,    -0.1541f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponShotgun",            -0.0065f, -0.0007f, 0.0f,    6.35f, -3.72f, 10.07f, 39.75f, -54.85f, -14.78f },
        { "ArkWeaponStunGun",             0.0f,    -0.1026f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "ArkWeaponToyGun",              0.0f,     0.0f,    0.0f,    0, 0, 0,              29.89f, -65.69f, 0.0f },
        { "ArkWeaponWrench",              0.0109f, -0.1324f, 0.0092f, 0, 0, 0,              0, 0, 0 },
        { "ArkWeaponExplosiveGrenade",    0.0f,    -0.1429f, 0.0f,    0, 0, 0,              0, 0, 0 },
        { "_none",                       -0.0020f, -0.1479f, 0.0f,    0, 0, 0,              0, 0, 0 },
    };
    void ApplyBuiltInInteract(const char* cls, WeaponSettings& w)
    {
        // one-handed: the support hand is animated out of view
        for (const char* oneHanded : { "ArkWeaponWrench", "ArkWeaponEMPGrenade", "ArkWeaponLureGrenade", "ArkWeaponRecyclerGrenade", "ArkWeaponExplosiveGrenade", "ArkWeaponNullwaveTransmitter" })
            if (strcmp(oneHanded, cls) == 0) w.interactHandOff = 1;
        for (const char* twoHanded : { "ArkWeaponPistol", "ArkWeaponShotgun", "ArkWeaponGooGun", "ArkWeaponStunGun", "ArkWeaponToyGun", "ArkWeaponInstalaser" })
            if (strcmp(twoHanded, cls) == 0) w.interactHandOff = 0;
        for (const BuiltInInteract& b : s_builtInInteract)
            if (strcmp(b.cls, cls) == 0)
            {
                w.interact.posX = b.x; w.interact.posY = b.y; w.interact.posZ = b.z;
                w.interact.pitch = b.pitch; w.interact.yaw = b.yaw; w.interact.roll = b.roll;
                w.interactForearm.pitch = b.faPitch; w.interactForearm.yaw = b.faYaw; w.interactForearm.roll = b.faRoll;
                return;
            }
    }
}

const WeaponSettings* WeaponSettings::BuiltIn(const char* weaponClass)
{
    static std::map<std::string, WeaponSettings> s_cache;
    if (!weaponClass || !*weaponClass)
        return nullptr;
    auto it = s_cache.find(weaponClass);
    if (it != s_cache.end())
        return &it->second;
    if (strcmp(weaponClass, "_none") == 0)
    {
        WeaponSettings w; // no weapon: only the interaction correction means anything here
        w.aim = WeaponSettings::DefaultAim();
        ApplyBuiltInInteract(weaponClass, w);
        w.valid = false;
        return &s_cache.emplace(weaponClass, w).first->second;
    }
    size_t n = 0;
    const BuiltInWeapon* t = BuiltInTable(n);
    for (size_t i = 0; i < n; i++)
    {
        if (strcmp(t[i].cls, weaponClass) != 0)
            continue;
        WeaponSettings w;
        ApplyBuiltInInteract(weaponClass, w);
        w.aimAllowed = t[i].aimAllowed;
        w.wallPush = t[i].wallPush;
        w.fireCoupling = t[i].fireCoupling;
        w.fireCouplingTime = t[i].fireCouplingTime;
        w.aimRecoilScale = t[i].aimRecoilScale;
        w.aimKickScale = t[i].aimKickScale;
        w.aimSpreadMult = t[i].aimSpread;
        w.hipSpreadMult = t[i].hipSpread;
        w.hip = t[i].hip;
        w.aim = t[i].aim;
        w.wall = t[i].wall;
        w.valid = false; // built-in: not written to the XML until the user changes it
        return &s_cache.emplace(weaponClass, w).first->second;
    }
    return nullptr;
}

const WeaponSettings* ModMain::FindWeapon(const char* weaponClass) const
{
    if (!weaponClass || !*weaponClass)
        return nullptr;
    auto it = m_weapons.find(weaponClass);
    if (it != m_weapons.end())
        return &it->second;
    return WeaponSettings::BuiltIn(weaponClass);
}

const WeaponSettings* ModMain::FindCurrentWeapon() const
{
    return FindWeapon(m_currentWeaponClass.c_str());
}

const WeaponSettings* ModMain::InteractWeaponEntry() const
{
    return FindWeapon(m_currentWeaponClass.empty() ? "_none" : m_currentWeaponClass.c_str());
}

WeaponSettings& ModMain::GetCurrentWeapon()
{
    const std::string& key = m_currentWeaponClass.empty() ? std::string("_none") : m_currentWeaponClass;
    auto it = m_weapons.find(key);
    if (it == m_weapons.end())
    {
        WeaponSettings w;
        if (const WeaponSettings* pBuiltIn = WeaponSettings::BuiltIn(key.c_str()))
            w = *pBuiltIn;
        else
            w.aim = WeaponSettings::DefaultAim();
        w.valid = false;
        it = m_weapons.emplace(key, w).first;
    }
    return it->second;
}

PoseOffset& ModMain::GetActivePose(const char** outName)
{
    if (m_isAiming && m_settings.aimEnabled)
    {
        if (outName) *outName = "aim pose (this weapon)";
        return GetCurrentWeapon().aim;
    }
    if (m_settings.nudgeTarget == 1 && !m_currentWeaponClass.empty())
    {
        if (outName) *outName = "hip offset (this weapon)";
        return GetCurrentWeapon().hip;
    }
    if (m_isCrouching && m_settings.crouchEnabled)
    {
        if (outName) *outName = "crouch offset (global)";
        return m_settings.crouch;
    }
    if (outName) *outName = "standing offset (global)";
    return m_settings.base;
}

void ModMain::UpdateNudge(float dt)
{
    if (!m_settings.nudgeKeys || m_mouseCaptured || IsHardwareCursorVisible())
        return;

    bool any = false;
    for (float v : m_nudgeAxis)
        any |= (v != 0.0f);
    if (!any)
        return;

    PoseOffset& pose = GetActivePose(nullptr);
    const float mul = m_nudgeSlow ? 0.2f : 1.0f;
    for (int i = 0; i < 3; i++)
        pose.Axis(i) += m_nudgeAxis[i] * m_settings.nudgePosSpeed * mul * dt;
    for (int i = 3; i < 6; i++)
        pose.Axis(i) += m_nudgeAxis[i] * m_settings.nudgeRotSpeed * mul * dt;

    const bool editingWeapon = m_isAiming || (!m_isAiming && m_settings.nudgeTarget == 1);
    if (editingWeapon && !m_currentWeaponClass.empty())
    {
        GetCurrentWeapon().valid = true;
        m_weaponsDirty = true;
    }
}

//---------------------------------------------------------------------------------
// Input
//---------------------------------------------------------------------------------
bool ModMain::HandleNudgeKey(EKeyId key, bool pressed)
{
    const float v = pressed ? 1.0f : 0.0f;
    auto axis = [&](int posAxis, int rotAxis, float dir) {
        // Clear both candidates so a mode switch mid-press cannot leave an axis stuck.
        m_nudgeAxis[posAxis] = 0.0f;
        m_nudgeAxis[rotAxis] = 0.0f;
        m_nudgeAxis[m_nudgeRotMode ? rotAxis : posAxis] = dir * v;
        return true;
    };

    if (m_settings.nudgeLayout == 0)
    {
        switch (key)
        {
        case eKI_NP_0: m_nudgeRotMode = pressed; return true;
        case eKI_NP_Period: m_nudgeSlow = pressed; return true;
        case eKI_NP_4: return axis(0, 4, -1.0f); // left  / yaw -
        case eKI_NP_6: return axis(0, 4, +1.0f); // right / yaw +
        case eKI_NP_8: return axis(2, 3, +1.0f); // up    / pitch +
        case eKI_NP_2: return axis(2, 3, -1.0f); // down  / pitch -
        case eKI_NP_7: return axis(1, 5, +1.0f); // fwd   / roll +
        case eKI_NP_1: return axis(1, 5, -1.0f); // back  / roll -
        default: return false;
        }
    }
    else
    {
        switch (key)
        {
        case eKI_H: m_nudgeRotMode = pressed; return true;
        case eKI_N: m_nudgeSlow = pressed; return true;
        case eKI_J: return axis(0, 4, -1.0f); // left  / yaw -
        case eKI_L: return axis(0, 4, +1.0f); // right / yaw +
        case eKI_I: return axis(2, 3, +1.0f); // up    / pitch +
        case eKI_K: return axis(2, 3, -1.0f); // down  / pitch -
        case eKI_U: return axis(1, 5, +1.0f); // fwd   / roll +
        case eKI_O: return axis(1, 5, -1.0f); // back  / roll -
        default: return false;
        }
    }
}

bool ModMain::OnInputEvent(const SInputEvent& event)
{
    // Only buttons/keys, not axes.
    if (event.deviceType != eIDT_Keyboard && event.deviceType != eIDT_Mouse && event.deviceType != eIDT_Gamepad)
        return false;
    if (event.state != eIS_Pressed && event.state != eIS_Released)
        return false;

    const bool pressed = (event.state == eIS_Pressed);

    if (m_waitingForAimKey)
    {
        if (pressed)
        {
            if (event.keyId != eKI_Escape)
                m_settings.aimKey = (int)event.keyId;
            m_waitingForAimKey = false;
            m_aimKeyHeld = false;
            return true;
        }
        return false;
    }

    if (m_waitingForExamKey != 0)
    {
        if (pressed)
        {
            if (event.keyId != eKI_Escape)
                (m_waitingForExamKey == 1 ? m_settings.interactExamKey : m_settings.interactExamKey2) = (int)event.keyId;
            m_waitingForExamKey = 0;
            return true;
        }
        return false;
    }

    if (m_waitingForSteadyKey)
    {
        if (pressed)
        {
            if (event.keyId != eKI_Escape)
                m_settings.steadyKey = (int)event.keyId;
            m_waitingForSteadyKey = false;
            return true;
        }
        return false;
    }
    if (m_settings.steadyKey != 0 && (int)event.keyId == m_settings.steadyKey)
    {
        m_feel.steadyHeld = pressed;
        if (Active() && m_settings.steadyEnabled && m_settings.steadyConsumeKey && (int)event.keyId != m_settings.aimKey)
            return true;
    }

    // Nudge keys (only while enabled). They are swallowed so the game does not react to them.
    if (Active() && m_settings.nudgeKeys && event.deviceType == eIDT_Keyboard && !m_mouseCaptured && !IsHardwareCursorVisible())
    {
        if (HandleNudgeKey(event.keyId, pressed))
            return true;
    }

    // Quick melee key.
    if (m_waitingForMeleeKey)
    {
        if (pressed)
        {
            if (event.keyId != eKI_Escape)
                m_settings.meleeKey = (int)event.keyId;
            m_waitingForMeleeKey = false;
            return true;
        }
        return false;
    }
    if (m_settings.meleeKey != 0 && (int)event.keyId == m_settings.meleeKey && Active() && m_settings.interactEnabled && m_settings.meleeEnabled
        && !m_mouseCaptured && !IsHardwareCursorVisible() && !ExaminingWorldUI())
    {
        if (pressed)
            StartMelee();
        if (m_settings.meleeConsumeKey)
            return true;
    }

    // Clicks on in-world screens / keypads: the reach goes to the cursor. The event is not swallowed.
    if (pressed && Active() && m_settings.interactEnabled && m_settings.interactExamination
        && ((int)event.keyId == m_settings.interactExamKey || (int)event.keyId == m_settings.interactExamKey2)
        && ExaminingWorldUI() && !m_mouseCaptured)
    {
        Vec3 p;
        m_interact.examCursorValid = CursorWorldPoint(p);
        if (m_interact.examCursorValid)
        {
            m_interact.examCursorWorld = p;
            m_interact.markerTimer = 3.0f;
            if (m_settings.interactDebugMarker && gEnv && gEnv->pSystem)
            {
                const CCamera& vc = gEnv->pSystem->GetViewCamera();
                const Vec3 cp = vc.GetPosition(), cd = vc.GetMatrix().GetColumn1();
                ArkPlayer* pP = ArkPlayer::GetInstancePtr();
                const Ang3 lr = pP ? pP->m_examinationMode.m_localRotation : Ang3(ZERO);
                CryLog("ViewmodelTweaks: screen click - view camera pos ({:.2f} {:.2f} {:.2f}) dir ({:.2f} {:.2f} {:.2f}), hit ({:.2f} {:.2f} {:.2f}) dist {:.2f}, "
                       "exam local rotation ({:.2f} {:.2f} {:.2f}), camModel t ({:.2f} {:.2f} {:.2f}) valid {}, view {}x{}",
                    cp.x, cp.y, cp.z, cd.x, cd.y, cd.z, p.x, p.y, p.z, (p - cp).GetLength(), lr.x, lr.y, lr.z,
                    m_render.camModel.t.x, m_render.camModel.t.y, m_render.camModel.t.z, m_render.camValid,
                    vc.GetViewSurfaceX(), vc.GetViewSurfaceZ());
            }
            m_interact.lastType = (int)EArkInteractionType::scriptDefined;
            m_interact.lastMode = 0;
            m_interact.lastEntity = "screen cursor";
            if (InteractAnimAllowed((int)EArkInteractionType::scriptDefined, 0))
                StartReach(0, &p);
            else
                m_interact.skipped++;
        }
    }

    if (!Active() || !m_settings.aimEnabled || (int)event.keyId != m_settings.aimKey)
        return false;

    if (m_settings.aimToggle)
    {
        if (pressed && !m_mouseCaptured && !IsHardwareCursorVisible())
            m_aimKeyHeld = !m_aimKeyHeld;
    }
    else
    {
        m_aimKeyHeld = pressed;
    }

    return m_settings.aimConsumeKey != 0;
}

const char* ModMain::GetKeyName(int keyId) const
{
    if (gCL && gCL->cl)
    {
        const auto& names = gCL->cl->GetKeyNames();
        auto it = names.left.find((EKeyId)keyId);
        if (it != names.left.end())
            return it->second.c_str();
    }
    static char buf[32];
    snprintf(buf, sizeof(buf), "key 0x%X", keyId);
    return buf;
}

//---------------------------------------------------------------------------------
// Weapon FOV
//---------------------------------------------------------------------------------
void ModMain::EnforceWeaponFov()
{
    if (!gEnv || !gEnv->pRenderer)
        return;

    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
        return;

    const ArkPlayerZoomManager& zoom = pPlayer->m_zoomManager;

    // When the game "locks" the near FOV it wants the weapon rendered with the world FOV
    // (cl_fov). Respect that.
    if (zoom.m_nearFOVLockedCount != 0)
        return;

    if (!m_settings.fovEnabled || m_settings.bypass)
    {
        // Put the stock value back once when the override gets disabled.
        if (m_lastAppliedFov >= 0.0f)
        {
            float stock = PreyInternals::STOCK_WEAPON_FOV;
            gEnv->pRenderer->EF_Query(EFQ_SetDrawNearFov, stock);
            m_lastAppliedFov = -1.0f;
        }
        return;
    }

    // The game scales the near FOV proportionally to the current horizontal FOV while zooming
    // (ArkPlayerZoomManager::Update): 55 degrees at cl_hfov, then near *= new/old on every HFOV
    // change. Mirror that relative to cl_hfov. (Do NOT derive the reference from the zoom stack:
    // it is normally empty, and our own aim-zoom entry would become the "base".)
    SCVars* pCVars = *g_pGameCVars;
    const float baseHfov = (pCVars && pCVars->cl_hfov > 1.0f) ? pCVars->cl_hfov : zoom.m_currentHFOV;

    float userFov = m_settings.fov;
    if (m_settings.aimEnabled && m_settings.aimFovEnabled)
        userFov = LERP(m_settings.fov, m_settings.aimFov, SmoothStep01(m_aimBlend));

    const float scale = (baseHfov > 1.0f) ? (zoom.m_currentHFOV / baseHfov) : 1.0f;
    float desired = clamp_tpl(userFov, 5.0f, 170.0f) * scale;

    float current = 0.0f;
    gEnv->pRenderer->EF_Query(EFQ_GetDrawNearFov, current);

    if (fabsf(current - desired) > 0.001f)
    {
        gEnv->pRenderer->EF_Query(EFQ_SetDrawNearFov, desired);
        m_lastAppliedFov = desired;
    }
}

void ModMain::UpdateCameraInputOverrides()
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !gEnv || !gEnv->pConsole)
        return;
    const ViewmodelSettings& s = m_settings;

    // World FOV: the game's own options slider writes cl_hfov; we just keep it where the user wants.
    if (s.worldFovEnabled)
    {
        if (ICVar* pVar = gEnv->pConsole->GetCVar("cl_hfov"))
        {
            const float want = clamp_tpl(s.worldFov, 50.0f, 140.0f);
            if (fabsf(pVar->GetFVal() - want) > 0.01f)
                pVar->Set(want);
        }
    }

    // Sprint look-sensitivity scale (ArkPlayerInput::GetRotation multiplies the look delta by
    // m_sprintCameraRotationRateScale while the movement FSM says "sprinting"). Loaded from the
    // player's Lua params, so re-apply every frame.
    float& sprintScale = pPlayer->m_input.m_sprintCameraRotationRateScale;
    if (m_gameSprintSensScale < 0.0f)
        m_gameSprintSensScale = sprintScale;
    if (s.sprintSensEnabled)
    {
        sprintScale = clamp_tpl(s.sprintSensScale, 0.1f, 3.0f);
        m_sprintSensApplied = true;
    }
    else if (m_sprintSensApplied)
    {
        sprintScale = m_gameSprintSensScale; // switched off: put the game's value back
        m_sprintSensApplied = false;
    }
}

void ModMain::UpdateReticle()
{
    // The reticle cvars' change handling writes into the player's reticle state; with no player
    // (main menu, loading) that is a null dereference inside the game. Only touch them in a level.
    if (!gEnv || !gEnv->pConsole || !ArkPlayer::GetInstancePtr())
    {
        m_reticlePlayerSeen = false;
        m_reticleStyleBeforeHide = -1;
        return;
    }
    const ViewmodelSettings& s = m_settings;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    // A level was (re)loaded: the game re-applies its profile, so our style has to be applied once more.
    const bool reapply = !m_reticlePlayerSeen;
    m_reticlePlayerSeen = true;

    // The reticle doubles as the mouse cursor on in-world screens (the HUD gets "reticlePosition" events
    // from the examination mode and draws the very same reticle sprite there). hud_reticleSetting 0 hides
    // that sprite as well, so while a screen is being used we put the reticle back and hide it again after.
    static_assert(offsetof(ArkExaminationMode, m_reticlePos) == 0x60, "ArkExaminationMode layout");
    static_assert(offsetof(ArkExaminationMode, m_examinationState) == 0x98, "ArkExaminationMode layout");
    const ArkExaminationMode& exam = pPlayer->m_examinationMode;
    const bool usingScreen = exam.m_examinationState == ArkExaminationMode::EArkExaminationState::active
        && exam.m_examinationType == ArkExaminationMode::EArkExaminationType::worldUI;

    // --- Position (g_reticleYPercentage) ---
    if (s.reticleMode != 0)
    {
        if (ICVar* pVar = gEnv->pConsole->GetCVar("g_reticleYPercentage"))
        {
            const float want = (s.reticleMode == 1) ? 0.5f : clamp_tpl(s.reticleY, 0.0f, 1.0f);
            if (fabsf(pVar->GetFVal() - want) > 0.0001f)
                pVar->Set(want);
        }
    }

    // --- Style / visibility (hud_reticleSetting: 0 none, 1 default, 2 dot) ---
    ICVar* pStyle = gEnv->pConsole->GetCVar("hud_reticleSetting");
    if (!pStyle)
        return;
    // Right after a (re)load the cvar still holds the game's own option value (the profile was just
    // applied): remember it, so "Hidden" and "hide while aiming" know what to bring back for screens.
    if (reapply && pStyle->GetIVal() > 0 && pStyle->GetIVal() != m_reticleStyleApplied)
        m_reticleGameStyle = pStyle->GetIVal();
    const bool hide = s.aimHideReticle && s.aimEnabled && SmoothStep01(m_aimBlend) > 0.5f;
    int want = -1; // -1 = leave alone
    switch (s.reticleStyle)
    {
    case 1: want = 1; break;
    case 2: want = 2; break;
    case 3: want = 0; break;
    default: break;
    }
    if (hide)
    {
        if (m_reticleStyleBeforeHide < 0)
            m_reticleStyleBeforeHide = (want >= 0) ? want : pStyle->GetIVal(); // remember what to go back to
        want = 0;
    }
    else if (m_reticleStyleBeforeHide >= 0)
    {
        if (want < 0)
            want = m_reticleStyleBeforeHide; // style is "leave alone": put back what the game had
        m_reticleStyleBeforeHide = -1;
    }
    if (want == 0 && usingScreen)
    {
        // Cursor needed: show the reticle the game itself would show (or the default one if the game's
        // own option is "off" too - otherwise there would be no cursor at all).
        const int base = (s.reticleStyle == 1 || s.reticleStyle == 2) ? s.reticleStyle : m_reticleGameStyle;
        want = (base > 0) ? base : 1;
    }
    // Apply only when OUR desired value changes (or a level was loaded) - never every frame, so the
    // game's own handling of the option is never fought.
    if (want >= 0 && (want != m_reticleStyleApplied || reapply))
    {
        if (pStyle->GetIVal() != want)
            pStyle->Set(want);
        m_reticleStyleApplied = want;
    }
    else if (want < 0)
        m_reticleStyleApplied = -1;
}

//---------------------------------------------------------------------------------
// Pop tracer: what the filters output vs. what the weapon actually does, per update
//---------------------------------------------------------------------------------
void ModMain::RecordTrace()
{
    constexpr size_t N = 1500; // ~30 s at 50 updates/s
    if (m_trace.size() != N)
    {
        m_trace.assign(N, TraceSample());
        m_traceHead = 0;
        m_traceCount = 0;
    }
    TraceSample smp;
    smp.t = (gEnv && gEnv->pTimer) ? (float)(gEnv->pTimer->GetAsyncTime().GetValue() % 100000000000LL) * 1e-5f : 0.0f;
    smp.dt = m_dtUsed;
    smp.cYaw = m_convergeYaw; smp.cYawT = m_convergeTargetYaw;
    smp.cPitch = m_convergePitch; smp.cPitchT = m_convergeTargetPitch;
    smp.wall = m_wallPush; smp.wallT = m_wallPushTarget;
    smp.dist = m_convergeDist;
    smp.attachValid = m_render.attachValid;
    if (m_render.attachValid && SaneQuatT(m_render.weaponRelCam, 5.0f))
    {
        const Ang3 a = SafeAng3(m_render.weaponRelCam.q);
        smp.mPitch = RAD2DEG(a.x); smp.mRoll = RAD2DEG(a.y); smp.mYaw = RAD2DEG(a.z);
        smp.mX = m_render.weaponRelCam.t.x; smp.mY = m_render.weaponRelCam.t.y; smp.mZ = m_render.weaponRelCam.t.z;
    }
    smp.lookYaw = RAD2DEG(SafeAng3(m_gameOffsets[0].q).z);
    smp.aimBlend = m_aimBlend;
    smp.sprintBlend = m_feel.sprintBlend;
    smp.dragYaw = m_feel.dragHipOut.yaw;
    smp.pwa = m_diag.pwaUpdatesLastFrame; smp.ctx = m_diag.ctxUpdatesLastFrame; smp.cam = m_render.callsLastFrame;

    // Pop detector: the weapon turned by a lot in one update while the smoothed value barely moved.
    const TraceSample& prev = m_trace[(m_traceHead + N - 1) % N];
    if (m_traceAuto && m_traceCount > 10 && prev.attachValid && smp.attachValid)
    {
        const float dMeasured = fabsf(smp.mYaw - prev.mYaw) + fabsf(smp.mPitch - prev.mPitch);
        const float dFilter = fabsf(smp.cYaw - prev.cYaw) + fabsf(smp.cPitch - prev.cPitch);
        const float dLook = fabsf(smp.lookYaw - prev.lookYaw);
        if (dMeasured > 3.0f && dFilter < 0.25f * dMeasured && dLook < 0.25f * dMeasured && smp.t - m_traceLastSave > 20.0f)
            m_traceStatus = "pop!"; // saved below, after this sample is in the buffer
    }
    m_trace[m_traceHead] = smp;
    m_traceHead = (m_traceHead + 1) % N;
    if (m_traceCount < N) m_traceCount++;
    if (m_traceStatus == "pop!")
        SaveTrace("auto (weapon jumped while the filter did not)");
}

void ModMain::SaveTrace(const char* reason)
{
    const fs::path path = GetWeaponsPath().parent_path() / "Vee.ViewmodelTweaks.trace.csv";
    FILE* f = fopen(path.u8string().c_str(), "w");
    if (!f)
    {
        m_traceStatus = "could not write " + path.u8string();
        return;
    }
    fprintf(f, "# %s | smoothing converge=%.3f wall=%.3f | %d samples, newest last\n", reason, m_settings.convergeSmoothTime, m_settings.wallPushSmoothTime, (int)m_traceCount);
    fprintf(f, "t,dt_ms,conv_yaw,conv_yaw_target,conv_pitch,conv_pitch_target,wall_cm,wall_target_cm,ray_dist,meas_yaw,meas_pitch,meas_roll,meas_x,meas_y,meas_z,look_yaw,aim_blend,sprint_blend,drag_yaw,pwa,ctx,cam,attach\n");
    const size_t N = m_trace.size();
    for (size_t i = 0; i < m_traceCount; i++)
    {
        const TraceSample& x = m_trace[(m_traceHead + N - m_traceCount + i) % N];
        fprintf(f, "%.4f,%.2f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d\n",
            x.t, x.dt * 1000.0f, x.cYaw, x.cYawT, x.cPitch, x.cPitchT, x.wall * 100.0f, x.wallT * 100.0f, x.dist,
            x.mYaw, x.mPitch, x.mRoll, x.mX, x.mY, x.mZ, x.lookYaw, x.aimBlend, x.sprintBlend, x.dragYaw, x.pwa, x.ctx, x.cam, x.attachValid ? 1 : 0);
    }
    fclose(f);
    m_traceSaves++;
    m_traceLastSave = m_traceCount ? m_trace[(m_traceHead + N - 1) % N].t : 0.0f;
    char buf[256];
    snprintf(buf, sizeof(buf), "trace #%d saved (%s)", m_traceSaves, reason);
    m_traceStatus = buf;
    CryLog("ViewmodelTweaks: {} -> {}", buf, path.u8string());
}

//---------------------------------------------------------------------------------
// Per-weapon persistence
//---------------------------------------------------------------------------------
fs::path ModMain::GetWeaponsPath() const
{
    fs::path dir;
    if (gCL && gCL->conf)
        dir = gCL->conf->getConfigPath("Vee.ViewmodelTweaks").parent_path();
    if (dir.empty() && gCL && gCL->cl)
        dir = gCL->cl->GetModsPath() / "config";
    return dir / "Vee.ViewmodelTweaks.weapons.xml";
}

static void ReadPose(pugi::xml_node n, const char* prefix, PoseOffset& p, const PoseOffset& def)
{
    auto attr = [&](const char* suffix, float d) {
        std::string name = std::string(prefix) + suffix;
        return n.attribute(name.c_str()).as_float(d);
    };
    p.posX = attr("x", def.posX); p.posY = attr("y", def.posY); p.posZ = attr("z", def.posZ);
    p.pitch = attr("pitch", def.pitch); p.yaw = attr("yaw", def.yaw); p.roll = attr("roll", def.roll);
}

static void WritePose(pugi::xml_node n, const char* prefix, const PoseOffset& p)
{
    auto attr = [&](const char* suffix, float v) {
        std::string name = std::string(prefix) + suffix;
        n.append_attribute(name.c_str()) = v;
    };
    attr("x", p.posX); attr("y", p.posY); attr("z", p.posZ);
    attr("pitch", p.pitch); attr("yaw", p.yaw); attr("roll", p.roll);
}

void ModMain::LoadWeapons()
{
    m_weapons.clear();
    pugi::xml_document doc;
    const fs::path path = GetWeaponsPath();
    if (!doc.load_file(path.c_str()))
        return;
    for (pugi::xml_node n : doc.child("Weapons").children("Weapon"))
    {
        const char* cls = n.attribute("class").as_string("");
        if (!*cls)
            continue;
        WeaponSettings w;
        w.aimAllowed = n.attribute("aim_allowed").as_bool(true);
        w.wallPush = n.attribute("wall_push").as_float(WeaponSettings().wallPush);
        w.fireCoupling = n.attribute("fire_coupling").as_float(WeaponSettings().fireCoupling);
        w.fireCouplingTime = n.attribute("fire_coupling_time").as_float(WeaponSettings().fireCouplingTime);
        w.aimRecoilScale = n.attribute("aim_recoil_scale").as_float(WeaponSettings().aimRecoilScale);
        w.aimKickScale = n.attribute("aim_kick_scale").as_float(WeaponSettings().aimKickScale);
        w.aimSpreadMult = n.attribute("aim_spread_mult").as_float(WeaponSettings().aimSpreadMult);
        w.hipSpreadMult = n.attribute("hip_spread_mult").as_float(WeaponSettings().hipSpreadMult);
        ReadPose(n, "hip_", w.hip, PoseOffset());
        ReadPose(n, "aim_", w.aim, WeaponSettings::DefaultAim());
        ReadPose(n, "interact_", w.interact, PoseOffset());
        ReadPose(n, "interact_rest_", w.interactRest, PoseOffset());
        ReadPose(n, "interact_forearm_", w.interactForearm, PoseOffset());
        ReadPose(n, "interact_start_", w.interactStart, PoseOffset());
        ReadPose(n, "interact_start_shoulder_", w.interactStartShoulder, PoseOffset());
        ReadPose(n, "interact_start_elbow_", w.interactStartElbow, PoseOffset());
        {
            // Files written before the near-wall pose existed keep the built-in pose for that weapon.
            const WeaponSettings* pB = WeaponSettings::BuiltIn(cls);
            w.interactHandOff = clamp_tpl(n.attribute("interact_hand_off").as_int(pB ? pB->interactHandOff : 2), 0, 2);
            ReadPose(n, "wall_", w.wall, pB ? pB->wall : PoseOffset());
            w.wallPoseAmount = n.attribute("wall_pose_amount").as_float(pB ? pB->wallPoseAmount : 1.0f);
        }
        // Backwards compatibility with 1.3.0 files (aim pose stored as x/y/z/pitch/yaw/roll)
        if (n.attribute("x") && !n.attribute("aim_x"))
            ReadPose(n, "", w.aim, WeaponSettings::DefaultAim());
        w.valid = true;
        m_weapons[cls] = w;
    }
    CryLog("ViewmodelTweaks: loaded {} weapon entries from {}", m_weapons.size(), path.u8string());
    SanitizeSettings();
}

void ModMain::SaveWeapons()
{
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("Weapons");
    root.append_attribute("comment") = "Per-weapon settings. hip_*: additive offset while not aiming. aim_*: weapon relative to camera while aiming. wall_push: pull-back (m) against walls. wall_*: near-wall pose blended in with the pull-back, wall_pose_amount its multiplier. Meters (x right, y forward, z up), degrees.";
    for (auto& kv : m_weapons)
    {
        if (!kv.second.valid)
            continue;
        pugi::xml_node n = root.append_child("Weapon");
        n.append_attribute("class") = kv.first.c_str();
        n.append_attribute("aim_allowed") = kv.second.aimAllowed;
        n.append_attribute("wall_push") = kv.second.wallPush;
        n.append_attribute("fire_coupling") = kv.second.fireCoupling;
        n.append_attribute("fire_coupling_time") = kv.second.fireCouplingTime;
        n.append_attribute("aim_recoil_scale") = kv.second.aimRecoilScale;
        n.append_attribute("aim_kick_scale") = kv.second.aimKickScale;
        n.append_attribute("aim_spread_mult") = kv.second.aimSpreadMult;
        n.append_attribute("hip_spread_mult") = kv.second.hipSpreadMult;
        WritePose(n, "hip_", kv.second.hip);
        WritePose(n, "aim_", kv.second.aim);
        WritePose(n, "wall_", kv.second.wall);
        n.append_attribute("wall_pose_amount") = kv.second.wallPoseAmount;
        WritePose(n, "interact_", kv.second.interact);
        WritePose(n, "interact_rest_", kv.second.interactRest);
        WritePose(n, "interact_forearm_", kv.second.interactForearm);
        WritePose(n, "interact_start_", kv.second.interactStart);
        WritePose(n, "interact_start_shoulder_", kv.second.interactStartShoulder);
        WritePose(n, "interact_start_elbow_", kv.second.interactStartElbow);
        n.append_attribute("interact_hand_off") = kv.second.interactHandOff;
    }
    const fs::path path = GetWeaponsPath();
    if (!doc.save_file(path.c_str()))
        CryError("ViewmodelTweaks: failed to save {}", path.u8string());
    m_weaponsDirty = false;
}

//---------------------------------------------------------------------------------
// Mod Initialization
//---------------------------------------------------------------------------------
void ModMain::FillModInfo(ModDllInfoEx& info)
{
    info.modName = "Vee.ViewmodelTweaks";
    info.logTag = "ViewmodelTweaks";
    info.supportsHotReload = true;
}

void ModMain::InitHooks()
{
    s_hookPwaUpdate.SetHookFunc(&CProceduralWeaponAnimation_Update_Hook);
    s_hookPwaContextUpdate.SetHookFunc(&CProceduralWeaponAnimationContext_Update_Hook);
    s_hookLookCompute.SetHookFunc(&LookCompute_Hook);
    s_hookStrafeCompute.SetHookFunc(&StrafeCompute_Hook);
    s_hookRecoilCompute.SetHookFunc(&RecoilCompute_Hook);
    s_hookBumpCompute.SetHookFunc(&BumpCompute_Hook);
    s_hookHfovMultiplier.SetHookFunc(&ArkPlayerZoomManager_GetHFOVDependentMultiplier_Hook);
    s_hookUpdateView.SetHookFunc(&ArkPlayerCamera_UpdateView_Hook);
    s_hookDispMin.SetHookFunc(&CArkWeaponShotgun_GetDispersionMinimum_Hook);
    s_hookDispMax.SetHookFunc(&CArkWeaponShotgun_GetDispersionMaximum_Hook);
    s_hookGetStatFloat.SetHookFunc(&CArkWeapon_GetStatFloat_Hook);
    s_hookSpawnPellets.SetHookFunc(&CArkWeaponShotgun_SpawnPellets_Hook);
    s_hookGetStatInt.SetHookFunc(&CArkWeapon_GetStatInt_Hook);
    s_hookFireWeapon.SetHookFunc(&CArkWeapon_FireWeapon_Hook);
    s_hookInteract.SetHookFunc(&ArkPlayerInteraction_Interact_Hook);
    s_hookStartCarrying.SetHookFunc(&ArkPlayerCarry_StartCarrying_Hook);
    s_hookPerformInteraction.SetHookFunc(&ArkPlayerInteraction_PerformInteraction_Hook);
    s_hookWeaponImpulse.SetHookFunc(&ArkWeaponUtils_DoWeaponImpulse_Hook);
}

static void RegisterPoseCVars(PoseOffset& p, const char* prefix, const char* what)
{
    // Names: vm_<prefix>pos_x ... e.g. vm_pos_x, vm_crouch_pos_x, vm_aim_pos_x.
    // The console keeps the name/help pointers, so they must outlive this call.
    static std::deque<std::string> s_strings;
    auto name = [&](const char* suffix) -> const char* {
        s_strings.push_back(std::string("vm_") + prefix + suffix);
        return s_strings.back().c_str();
    };
    auto help = [&](const char* text) -> const char* {
        s_strings.push_back(std::string("Viewmodel Tweaks (") + what + "): " + text);
        return s_strings.back().c_str();
    };
    REGISTER_CVAR2(name("pos_x"), &p.posX, p.posX, VF_DUMPTOCHAIR, help("right(+)/left(-) offset in meters"));
    REGISTER_CVAR2(name("pos_y"), &p.posY, p.posY, VF_DUMPTOCHAIR, help("forward(+)/back(-) offset in meters"));
    REGISTER_CVAR2(name("pos_z"), &p.posZ, p.posZ, VF_DUMPTOCHAIR, help("up(+)/down(-) offset in meters"));
    REGISTER_CVAR2(name("rot_pitch"), &p.pitch, p.pitch, VF_DUMPTOCHAIR, help("pitch offset in degrees (tilt up/down)"));
    REGISTER_CVAR2(name("rot_yaw"), &p.yaw, p.yaw, VF_DUMPTOCHAIR, help("yaw offset in degrees (turn left/right)"));
    REGISTER_CVAR2(name("rot_roll"), &p.roll, p.roll, VF_DUMPTOCHAIR, help("roll offset in degrees"));
}

static void RegisterReachStyleCVars(ReachStyle& r, const char* prefix, const char* what)
{
    static std::deque<std::string> s_strings;
    auto name = [&](const char* suffix) -> const char* {
        s_strings.push_back(std::string("vm_interact_") + prefix + suffix);
        return s_strings.back().c_str();
    };
    auto help = [&](const char* text) -> const char* {
        s_strings.push_back(std::string("Viewmodel Tweaks (interaction ") + what + "): " + text);
        return s_strings.back().c_str();
    };
    REGISTER_CVAR2(name("reach_time"), &r.reachTime, r.reachTime, VF_DUMPTOCHAIR, help("seconds for the hand to reach the target"));
    REGISTER_CVAR2(name("hold_time"), &r.holdTime, r.holdTime, VF_DUMPTOCHAIR, help("seconds the hand stays there"));
    REGISTER_CVAR2(name("return_time"), &r.returnTime, r.returnTime, VF_DUMPTOCHAIR, help("seconds for the hand to come back"));
    REGISTER_CVAR2(name("amount"), &r.amount, r.amount, VF_DUMPTOCHAIR, help("fraction of the way to the target (1 = touch)"));
    REGISTER_CVAR2(name("off_x"), &r.offX, r.offX, VF_DUMPTOCHAIR, help("target offset right (m)"));
    REGISTER_CVAR2(name("off_y"), &r.offY, r.offY, VF_DUMPTOCHAIR, help("target offset forward (m, negative = stop short)"));
    REGISTER_CVAR2(name("off_z"), &r.offZ, r.offZ, VF_DUMPTOCHAIR, help("target offset up (m)"));
    REGISTER_CVAR2(name("arc_x"), &r.arcX, r.arcX, VF_DUMPTOCHAIR, help("sideways bulge of the way out (m)"));
    REGISTER_CVAR2(name("arc_z"), &r.arcZ, r.arcZ, VF_DUMPTOCHAIR, help("vertical bulge of the way out (m)"));
    REGISTER_CVAR2(name("return_arc_x"), &r.retArcX, r.retArcX, VF_DUMPTOCHAIR, help("sideways bulge of the way back (m)"));
    REGISTER_CVAR2(name("return_arc_z"), &r.retArcZ, r.retArcZ, VF_DUMPTOCHAIR, help("vertical bulge of the way back (m)"));
    REGISTER_CVAR2(name("rot_pitch"), &r.pitch, r.pitch, VF_DUMPTOCHAIR, help("hand pitch at the target (deg)"));
    REGISTER_CVAR2(name("rot_yaw"), &r.yaw, r.yaw, VF_DUMPTOCHAIR, help("hand yaw at the target (deg)"));
    REGISTER_CVAR2(name("rot_roll"), &r.roll, r.roll, VF_DUMPTOCHAIR, help("hand roll at the target (deg)"));
    REGISTER_CVAR2(name("envelope"), &r.envelopeScale, r.envelopeScale, VF_DUMPTOCHAIR, help("reach envelope limits times this while it plays (1 = as set)"));
    REGISTER_CVAR2(name("along"), &r.along, r.along, VF_DUMPTOCHAIR, help("target moved along the camera -> target line (m, + = into the object)"));
    REGISTER_CVAR2(name("windup_time"), &r.windupTime, r.windupTime, VF_DUMPTOCHAIR, help("seconds of windup before the reach (0 = none)"));
    REGISTER_CVAR2(name("windup_x"), &r.windX, r.windX, VF_DUMPTOCHAIR, help("windup: hand pulled right (m)"));
    REGISTER_CVAR2(name("windup_y"), &r.windY, r.windY, VF_DUMPTOCHAIR, help("windup: hand pulled forward (m, negative = back)"));
    REGISTER_CVAR2(name("windup_z"), &r.windZ, r.windZ, VF_DUMPTOCHAIR, help("windup: hand pulled up (m)"));
    REGISTER_CVAR2(name("windup_shoulder_x"), &r.windShX, r.windShX, VF_DUMPTOCHAIR, help("windup: shoulder moved right (m)"));
    REGISTER_CVAR2(name("windup_shoulder_y"), &r.windShY, r.windShY, VF_DUMPTOCHAIR, help("windup: shoulder moved forward (m)"));
    REGISTER_CVAR2(name("windup_shoulder_z"), &r.windShZ, r.windShZ, VF_DUMPTOCHAIR, help("windup: shoulder moved up (m)"));
    REGISTER_CVAR2(name("windup_elbow_x"), &r.windElX, r.windElX, VF_DUMPTOCHAIR, help("windup: elbow moved right (m)"));
    REGISTER_CVAR2(name("windup_elbow_y"), &r.windElY, r.windElY, VF_DUMPTOCHAIR, help("windup: elbow moved forward (m)"));
    REGISTER_CVAR2(name("windup_elbow_z"), &r.windElZ, r.windElZ, VF_DUMPTOCHAIR, help("windup: elbow moved up (m)"));
    REGISTER_CVAR2(name("windup_forearm_pitch"), &r.windFaPitch, r.windFaPitch, VF_DUMPTOCHAIR, help("windup: forearm pitch (deg)"));
    REGISTER_CVAR2(name("windup_forearm_yaw"), &r.windFaYaw, r.windFaYaw, VF_DUMPTOCHAIR, help("windup: forearm yaw (deg)"));
    REGISTER_CVAR2(name("windup_forearm_roll"), &r.windFaRoll, r.windFaRoll, VF_DUMPTOCHAIR, help("windup: forearm roll (deg)"));
    REGISTER_CVAR2(name("windup_keep"), &r.windKeep, r.windKeep, VF_DUMPTOCHAIR, help("how much of the shoulder / elbow / forearm windup stays through the strike (0..1)"));
}

void ModMain::RegisterCVars()
{
    ViewmodelSettings& s = m_settings;
    REGISTER_CVAR2("vm_enabled", &s.enabled, s.enabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: enable position/rotation offsets (0/1)");
    REGISTER_CVAR2("vm_bypass", &s.bypass, s.bypass, VF_DUMPTOCHAIR, "Viewmodel Tweaks: vanilla viewmodel - switch off every viewmodel feature except reticle, world FOV and sprint sensitivity (0/1)");
    RegisterPoseCVars(s.base, "", "global standing");

    REGISTER_CVAR2("vm_crouch_enabled", &s.crouchEnabled, s.crouchEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: blend in the crouch pose while sneaking (0/1)");
    RegisterPoseCVars(s.crouch, "crouch_", "global crouch, added to standing");
    REGISTER_CVAR2("vm_crouch_time", &s.crouchTime, s.crouchTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: stand<->crouch pose transition time in seconds");

    REGISTER_CVAR2("vm_aim_enabled", &s.aimEnabled, s.aimEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: enable the aim key / aim pose (0/1)");
    REGISTER_CVAR2("vm_aim_key", &s.aimKey, s.aimKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: EKeyId of the aim key (257 = right mouse button)");
    REGISTER_CVAR2("vm_aim_toggle", &s.aimToggle, s.aimToggle, VF_DUMPTOCHAIR, "Viewmodel Tweaks: 0 = hold to aim, 1 = toggle");
    REGISTER_CVAR2("vm_aim_consume_key", &s.aimConsumeKey, s.aimConsumeKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: swallow the aim key so the game's own binding on it is ignored (0/1)");
    REGISTER_CVAR2("vm_aim_render_lock", &s.aimRenderLock, s.aimRenderLock, VF_DUMPTOCHAIR, "Viewmodel Tweaks: place the weapon exactly at render time, after the camera is final (0/1)");
    REGISTER_CVAR2("vm_aim_hands_follow", &s.aimHandsFollow, s.aimHandsFollow, VF_DUMPTOCHAIR, "Viewmodel Tweaks: move the hand joints with the render-time placement (0/1)");
    REGISTER_CVAR2("vm_aim_left_hand_follow", &s.aimLeftHandFollow, s.aimLeftHandFollow, VF_DUMPTOCHAIR, "Viewmodel Tweaks: also move the support hand when it is on the weapon (0/1)");
    REGISTER_CVAR2("vm_aim_anim_recoil", &s.aimAnimRecoil, s.aimAnimRecoil, VF_DUMPTOCHAIR, "Viewmodel Tweaks: multiplier on the game's recoil/bump weapon motion while locked (0..3)");
    REGISTER_CVAR2("vm_aim_bob_tau", &s.aimBobTau, s.aimBobTau, VF_DUMPTOCHAIR, "Viewmodel Tweaks: head-bob separation time constant in seconds");
    REGISTER_CVAR2("vm_aim_bob", &s.aimBobAmount, s.aimBobAmount, VF_DUMPTOCHAIR, "Viewmodel Tweaks: head-bob coupling while locked (0 = still sights, 1 = weapon lags the head fully, negative = leads)");
    REGISTER_CVAR2("vm_aim_anim_sway", &s.aimAnimSway, s.aimAnimSway, VF_DUMPTOCHAIR, "Viewmodel Tweaks: how much of the game's look/strafe sway to keep while locked (0..1)");
    REGISTER_CVAR2("vm_aim_sens_mode", &s.aimSensMode, s.aimSensMode, VF_DUMPTOCHAIR, "Viewmodel Tweaks: ADS look sensitivity. 0 = game default (weapon's zoomed multiplier), 1 = match the camera zoom factor, 2 = custom (vm_aim_sens_scale)");
    REGISTER_CVAR2("vm_aim_sens_scale", &s.aimSensScale, s.aimSensScale, VF_DUMPTOCHAIR, "Viewmodel Tweaks: custom ADS look sensitivity multiplier (mode 2)");
    REGISTER_CVAR2("vm_aim_time", &s.aimTime, s.aimTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hip<->aim transition time in seconds");
    REGISTER_CVAR2("vm_aim_ignores_crouch", &s.aimIgnoresCrouch, s.aimIgnoresCrouch, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fade the crouch pose out while aiming (0/1)");
    REGISTER_CVAR2("vm_aim_fov_enabled", &s.aimFovEnabled, s.aimFovEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: use a separate weapon FOV while aiming (0/1)");
    REGISTER_CVAR2("vm_aim_fov", &s.aimFov, s.aimFov, VF_DUMPTOCHAIR, "Viewmodel Tweaks: weapon FOV while aiming, in degrees");
    REGISTER_CVAR2("vm_aim_camera_zoom", &s.aimCameraZoom, s.aimCameraZoom, VF_DUMPTOCHAIR, "Viewmodel Tweaks: zoom the camera while aiming (0/1)");
    REGISTER_CVAR2("vm_aim_camera_zoom_factor", &s.aimCameraZoomFactor, s.aimCameraZoomFactor, VF_DUMPTOCHAIR, "Viewmodel Tweaks: camera HFOV multiplier while aiming (1 = none)");

    REGISTER_CVAR2("vm_nudge_keys", &s.nudgeKeys, s.nudgeKeys, VF_DUMPTOCHAIR, "Viewmodel Tweaks: nudge keys edit the active pose (0/1)");
    REGISTER_CVAR2("vm_nudge_layout", &s.nudgeLayout, s.nudgeLayout, VF_DUMPTOCHAIR, "Viewmodel Tweaks: nudge key layout. 0 = numpad (4/6 8/2 7/1, NP0 rotate, NP. slow), 1 = IJKL (J/L I/K U/O, H rotate, N slow)");
    REGISTER_CVAR2("vm_nudge_target", &s.nudgeTarget, s.nudgeTarget, VF_DUMPTOCHAIR, "Viewmodel Tweaks: what the nudge keys edit while not aiming. 0 = global standing/crouch offset (by stance), 1 = this weapon's hip offset");
    REGISTER_CVAR2("vm_nudge_pos_speed", &s.nudgePosSpeed, s.nudgePosSpeed, VF_DUMPTOCHAIR, "Viewmodel Tweaks: nudge speed in m/s");
    REGISTER_CVAR2("vm_nudge_rot_speed", &s.nudgeRotSpeed, s.nudgeRotSpeed, VF_DUMPTOCHAIR, "Viewmodel Tweaks: nudge speed in deg/s");

    REGISTER_CVAR2("vm_converge", &s.convergeEnabled, s.convergeEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hip-fire weapon convergence on the crosshair's impact point (0/1)");
    REGISTER_CVAR2("vm_converge_strength", &s.convergeStrength, s.convergeStrength, VF_DUMPTOCHAIR, "Viewmodel Tweaks: convergence strength (0..1)");
    REGISTER_CVAR2("vm_converge_max_angle", &s.convergeMaxAngle, s.convergeMaxAngle, VF_DUMPTOCHAIR, "Viewmodel Tweaks: convergence max angle in degrees");
    REGISTER_CVAR2("vm_converge_smooth", &s.convergeSmoothTime, s.convergeSmoothTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: convergence smoothing time constant in seconds");
    REGISTER_CVAR2("vm_converge_max_dist", &s.convergeMaxDist, s.convergeMaxDist, VF_DUMPTOCHAIR, "Viewmodel Tweaks: convergence raycast distance in meters (no hit = parallel)");
    REGISTER_CVAR2("vm_aim_wall_block", &s.aimWallBlockEnabled, s.aimWallBlockEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: prevent aiming while the weapon would poke into a wall (0/1)");
    REGISTER_CVAR2("vm_aim_block_reload", &s.aimBlockReload, s.aimBlockReload, VF_DUMPTOCHAIR, "Viewmodel Tweaks: no aiming down sights while the weapon reloads (0/1)");
    REGISTER_CVAR2("vm_aim_block_switch", &s.aimBlockSwitch, s.aimBlockSwitch, VF_DUMPTOCHAIR, "Viewmodel Tweaks: no aiming down sights while a weapon is holstered / drawn (0/1)");
    REGISTER_CVAR2("vm_aim_switch_delay", &s.aimSwitchDelay, s.aimSwitchDelay, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds after a weapon change before aiming is allowed (covers the raise animation)");
    REGISTER_CVAR2("vm_reload_fade", &s.reloadFadesOffsets, s.reloadFadesOffsets, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fade the hip offsets out while reloading so the support hand meets the weapon (0/1)");
    REGISTER_CVAR2("vm_reload_fade_time", &s.reloadFadeTime, s.reloadFadeTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reload fade in/out time in seconds");
    REGISTER_CVAR2("vm_aim_wall_block_scale", &s.aimWallBlockScale, s.aimWallBlockScale, VF_DUMPTOCHAIR, "Viewmodel Tweaks: tolerance multiplier on the aim-block distance");
    REGISTER_CVAR2("vm_wall_push", &s.wallPushEnabled, s.wallPushEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: pull the weapon back towards the camera near walls (0/1); amount is per weapon");
    REGISTER_CVAR2("vm_wall_push_start", &s.wallPushStartDist, s.wallPushStartDist, VF_DUMPTOCHAIR, "Viewmodel Tweaks: distance (m) at which the wall pull-back starts");
    REGISTER_CVAR2("vm_wall_push_full", &s.wallPushFullDist, s.wallPushFullDist, VF_DUMPTOCHAIR, "Viewmodel Tweaks: distance (m) at which the full per-weapon pull-back is reached");
    REGISTER_CVAR2("vm_wall_push_smooth", &s.wallPushSmoothTime, s.wallPushSmoothTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: wall pull-back smoothing time constant in seconds");
    REGISTER_CVAR2("vm_wall_push_aiming", &s.wallPushWhileAiming, s.wallPushWhileAiming, VF_DUMPTOCHAIR, "Viewmodel Tweaks: keep the wall pull-back while aiming (0/1)");
    REGISTER_CVAR2("vm_wall_pose", &s.wallPoseEnabled, s.wallPoseEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: blend each weapon's near-wall pose in as the pull-back builds up (0/1); the pose is per weapon");
    REGISTER_CVAR2("vm_wall_pose_start", &s.wallPoseStart, s.wallPoseStart, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fraction of the full pull-back (0..1) where the near-wall pose starts blending in");
    REGISTER_CVAR2("vm_wall_pose_full", &s.wallPoseFull, s.wallPoseFull, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fraction of the full pull-back (0..1) where the near-wall pose is fully applied");
    REGISTER_CVAR2("vm_wall_pose_converge_fade", &s.wallPoseConvergeFade, s.wallPoseConvergeFade, VF_DUMPTOCHAIR, "Viewmodel Tweaks: how much the hip convergence fades out as the near-wall pose comes in (0..1)");
    REGISTER_CVAR2("vm_wall_pose_pitch_start", &s.wallPosePitchStart, s.wallPosePitchStart, VF_DUMPTOCHAIR, "Viewmodel Tweaks: camera pitch (deg up or down) where the near-wall pose starts giving way to the plain pull-back");
    REGISTER_CVAR2("vm_wall_pose_pitch_full", &s.wallPosePitchFull, s.wallPosePitchFull, VF_DUMPTOCHAIR, "Viewmodel Tweaks: camera pitch (deg) where that fade is complete");
    REGISTER_CVAR2("vm_wall_pose_pitch_strength", &s.wallPosePitchStrength, s.wallPosePitchStrength, VF_DUMPTOCHAIR, "Viewmodel Tweaks: how much of the near-wall pose is removed at full pitch (0 = pitch fade off, 1 = all of it)");
    REGISTER_CVAR2("vm_reticle_mode", &s.reticleMode, s.reticleMode, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reticle position. 0 = game default, 1 = centered, 2 = custom (vm_reticle_y)");
    REGISTER_CVAR2("vm_reticle_y", &s.reticleY, s.reticleY, VF_DUMPTOCHAIR, "Viewmodel Tweaks: custom g_reticleYPercentage (0 = top, 1 = bottom)");
    REGISTER_CVAR2("vm_reticle_style", &s.reticleStyle, s.reticleStyle, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reticle style (hud_reticleSetting). 0 = game default, 1 = default reticle, 2 = simple dot, 3 = hidden");
    REGISTER_CVAR2("vm_aim_hide_reticle", &s.aimHideReticle, s.aimHideReticle, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hide the reticle while aiming down sights (0/1)");

    // interaction reach
    REGISTER_CVAR2("vm_interact", &s.interactEnabled, s.interactEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach out with the support hand when interacting (0/1)");
    REGISTER_CVAR2("vm_interact_defer", &s.interactDefer, s.interactDefer, VF_DUMPTOCHAIR, "Viewmodel Tweaks: delay the game's interaction until the hand is about to arrive (0/1)");
    REGISTER_CVAR2("vm_interact_fire_delay", &s.interactFireDelay, s.interactFireDelay, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds from the key press to the actual interaction when deferring");
    REGISTER_CVAR2("vm_interact_cancel_retarget", &s.interactCancelRetarget, s.interactCancelRetarget, VF_DUMPTOCHAIR, "Viewmodel Tweaks: drop the deferred interaction if the crosshair moved to another object (0/1)");
    REGISTER_CVAR2("vm_interact_target_mode", &s.interactTargetMode, s.interactTargetMode, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hand target. 0 = crosshair hit point on the object, 1 = object centre, 2 = fixed point ahead (vm_interact_test_*)");
    REGISTER_CVAR2("vm_interact_while_aiming", &s.interactWhileAiming, s.interactWhileAiming, VF_DUMPTOCHAIR, "Viewmodel Tweaks: animate interactions while aiming down sights too (0/1)");
    REGISTER_CVAR2("vm_interact_force_left_ik", &s.interactForceLeftIk, s.interactForceLeftIk, VF_DUMPTOCHAIR, "Viewmodel Tweaks: force the left arm's IK weight to 1 during the reach (0/1)");
    REGISTER_CVAR2("vm_interact_rotate", &s.interactRotate, s.interactRotate, VF_DUMPTOCHAIR, "Viewmodel Tweaks: push the style's hand rotation as well (0/1)");
    REGISTER_CVAR2("vm_interact_unarmed", &s.interactUnarmed, s.interactUnarmed, VF_DUMPTOCHAIR, "Viewmodel Tweaks: interaction reach with no weapon out too (0/1)");
    REGISTER_CVAR2("vm_interact_examination", &s.interactExamination, s.interactExamination, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach for the cursor when clicking on in-world screens / keypads (0/1)");
    REGISTER_CVAR2("vm_interact_exam_key", &s.interactExamKey, s.interactExamKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: EKeyId that counts as a screen click (256 = left mouse button)");
    REGISTER_CVAR2("vm_interact_exam_key2", &s.interactExamKey2, s.interactExamKey2, VF_DUMPTOCHAIR, "Viewmodel Tweaks: second EKeyId that counts as a screen click (17 = E)");
    RegisterPoseCVars(s.examCorr, "interact_exam_", "interaction hand correction on screens");
    REGISTER_CVAR2("vm_interact_exam_body_x", &s.interactExamBodyX, s.interactExamBodyX, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens, move the arms/torso right (m)");
    REGISTER_CVAR2("vm_interact_exam_body_y", &s.interactExamBodyY, s.interactExamBodyY, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens, move the arms/torso forward (m)");
    REGISTER_CVAR2("vm_interact_exam_body_z", &s.interactExamBodyZ, s.interactExamBodyZ, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens, move the arms/torso up (m)");
    REGISTER_CVAR2("vm_interact_exam_auto_body", &s.interactExamAutoBody, s.interactExamAutoBody, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens, bring the body forward automatically when the wrist is beyond the arm (0/1)");
    REGISTER_CVAR2("vm_interact_exam_arm_length", &s.interactExamArmLength, s.interactExamArmLength, VF_DUMPTOCHAIR, "Viewmodel Tweaks: shoulder-to-wrist distance the automatic body offset keeps (m)");
    REGISTER_CVAR2("vm_interact_exam_min_dist", &s.interactExamMinTargetDist, s.interactExamMinTargetDist, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens, no reach when the point is closer than this to the camera (m)");
    REGISTER_CVAR2("vm_interact_exam_fov_mode", &s.interactExamFovMode, s.interactExamFovMode, VF_DUMPTOCHAIR, "Viewmodel Tweaks: camera FOV on screens. 0 = the game's zoom, 1 = no zoom (cl_hfov), 2 = custom (vm_interact_exam_fov)");
    REGISTER_CVAR2("vm_interact_exam_fov", &s.interactExamFov, s.interactExamFov, VF_DUMPTOCHAIR, "Viewmodel Tweaks: custom horizontal FOV on screens (deg)");
    REGISTER_CVAR2("vm_interact_debug_marker", &s.interactDebugMarker, s.interactDebugMarker, VF_DUMPTOCHAIR, "Viewmodel Tweaks: draw the reach target / asked hand position and log screen clicks (0/1)");
    REGISTER_CVAR2("vm_interact_exam_rest", &s.interactExamRest, s.interactExamRest, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens keep the arms shown and the pointing hand resting in view between clicks (0/1)");
    REGISTER_CVAR2("vm_interact_rest_x", &s.interactRestX, s.interactRestX, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting spot, right (m)");
    REGISTER_CVAR2("vm_interact_rest_y", &s.interactRestY, s.interactRestY, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting spot, forward (m)");
    REGISTER_CVAR2("vm_interact_rest_z", &s.interactRestZ, s.interactRestZ, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting spot, up (m)");
    REGISTER_CVAR2("vm_interact_rest_exam_x", &s.interactRestExamX, s.interactRestExamX, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting spot on screens, right (m)");
    REGISTER_CVAR2("vm_interact_rest_exam_y", &s.interactRestExamY, s.interactRestExamY, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting spot on screens, forward (m)");
    REGISTER_CVAR2("vm_interact_rest_exam_z", &s.interactRestExamZ, s.interactRestExamZ, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting spot on screens, up (m)");
    REGISTER_CVAR2("vm_interact_hover_aiming", &s.interactHoverWhileAiming, s.interactHoverWhileAiming, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hover hand also while aiming down sights (0/1)");
    REGISTER_CVAR2("vm_interact_ik_weight_ramp", &s.interactIkWeightRamp, s.interactIkWeightRamp, VF_DUMPTOCHAIR, "Viewmodel Tweaks: blend the left arm's IK weight in with the reach instead of forcing 1 (0/1)");
    REGISTER_CVAR2("vm_interact_rest_blend", &s.interactRestBlendTime, s.interactRestBlendTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds to settle into / out of the resting spot");
    REGISTER_CVAR2("vm_interact_exam_leave_time", &s.interactExamLeaveTime, s.interactExamLeaveTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds over which the screen-specific pieces (body shift, corrections, limits) fade after leaving a screen");
    REGISTER_CVAR2("vm_interact_rest_sway_pos", &s.interactRestSwayPos, s.interactRestSwayPos, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting hand drift amplitude (m)");
    REGISTER_CVAR2("vm_interact_rest_sway_rot", &s.interactRestSwayRot, s.interactRestSwayRot, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting hand drift amplitude (degrees)");
    REGISTER_CVAR2("vm_interact_rest_sway_freq", &s.interactRestSwayFreq, s.interactRestSwayFreq, VF_DUMPTOCHAIR, "Viewmodel Tweaks: resting hand drift, Hz of the slow axis");
    REGISTER_CVAR2("vm_interact_hover", &s.interactHoverRest, s.interactHoverRest, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hold the resting hand up while looking at something usable within reach, outside screens (0/1)");
    REGISTER_CVAR2("vm_interact_hover_types", &s.interactHoverTypeMask, s.interactHoverTypeMask, VF_DUMPTOCHAIR, "Viewmodel Tweaks: bitmask of EArkInteractionType values the hand comes up for (see vm_interact_type_mask)");
    REGISTER_CVAR2("vm_interact_hover_dist", &s.interactHoverMaxDist, s.interactHoverMaxDist, VF_DUMPTOCHAIR, "Viewmodel Tweaks: the hand comes up when the usable thing is closer than this (m)");
    REGISTER_CVAR2("vm_interact_hover_towards", &s.interactHoverTowards, s.interactHoverTowards, VF_DUMPTOCHAIR, "Viewmodel Tweaks: 0 = resting spot, 1 = hover right at the thing");
    REGISTER_CVAR2("vm_interact_hover_unarmed", &s.interactHoverUnarmed, s.interactHoverUnarmed, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hover hand with no weapon out (0/1)");
    REGISTER_CVAR2("vm_interact_hover_weapon", &s.interactHoverWeapon, s.interactHoverWeapon, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hover hand with a weapon out (0/1)");
    REGISTER_CVAR2("vm_interact_nozoom_keypad", &s.interactNoZoomKeypad, s.interactNoZoomKeypad, VF_DUMPTOCHAIR, "Viewmodel Tweaks: use keypads from where you stand, no automatic zoom-in (writes ui_examine_keypad) (0/1)");
    REGISTER_CVAR2("vm_interact_nozoom_fabricator", &s.interactNoZoomFabricator, s.interactNoZoomFabricator, VF_DUMPTOCHAIR, "Viewmodel Tweaks: the same for fabricators (ui_examine_fabricator) (0/1)");
    REGISTER_CVAR2("vm_interact_nozoom_security", &s.interactNoZoomSecurity, s.interactNoZoomSecurity, VF_DUMPTOCHAIR, "Viewmodel Tweaks: the same for security stations (ui_examine_securitystation) (0/1)");
    REGISTER_CVAR2("vm_interact_nozoom_workstation", &s.interactNoZoomWorkstation, s.interactNoZoomWorkstation, VF_DUMPTOCHAIR, "Viewmodel Tweaks: the same for workstations (ui_examine_workstation) (0/1)");
    REGISTER_CVAR2("vm_interact_grab_apex", &s.interactGrabAtApex, s.interactGrabAtApex, VF_DUMPTOCHAIR, "Viewmodel Tweaks: grab-style interactions fire when the reach is at its apex instead of after vm_interact_fire_delay (0/1)");
    REGISTER_CVAR2("vm_interact_carry_apex", &s.interactCarryAtApex, s.interactCarryAtApex, VF_DUMPTOCHAIR, "Viewmodel Tweaks: carrying starts when the hand gets there - the game's carry delay is stretched to the reach (0/1)");
    REGISTER_CVAR2("vm_interact_exam_gentle", &s.interactExamGentle, s.interactExamGentle, VF_DUMPTOCHAIR, "Viewmodel Tweaks: on screens the press uses its own gentler style, vm_interact_press_exam_* (0/1)");
    REGISTER_CVAR2("vm_interact_show_arms", &s.interactShowArms, s.interactShowArms, VF_DUMPTOCHAIR, "Viewmodel Tweaks: show the arms for the reach where the game hides them - unarmed, screens (0/1)");
    REGISTER_CVAR2("vm_interact_correct", &s.interactCorrGain, s.interactCorrGain, VF_DUMPTOCHAIR, "Viewmodel Tweaks: closed-loop correction of the hand position per frame (0 = off, 0.5 default, 1 = full)");
    REGISTER_CVAR2("vm_interact_wrist_mode", &s.interactWristMode, s.interactWristMode, VF_DUMPTOCHAIR, "Viewmodel Tweaks: how a hand pose's wrist orientation is applied. 0 = IK target joint, 1 = hand joint relative to the forearm, 2 = both");
    REGISTER_CVAR2("vm_interact_ease_in", &s.interactEaseIn, s.interactEaseIn, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach easing. 0 linear, 1 smooth, 2 ease out, 3 ease in, 4 ease in-out");
    REGISTER_CVAR2("vm_interact_ease_out", &s.interactEaseOut, s.interactEaseOut, VF_DUMPTOCHAIR, "Viewmodel Tweaks: return easing (same list)");
    REGISTER_CVAR2("vm_interact_type_mask", &s.interactTypeMask, s.interactTypeMask, VF_DUMPTOCHAIR, "Viewmodel Tweaks: bitmask of EArkInteractionType values that animate (bit 1 scriptDefined, 3 codeDefined, 4 pickup, 5 consume, 6 carry, 7 hack, 8 repair, 9 fortify, 10 examine, 11 equip, 12 hoover)");
    REGISTER_CVAR2("vm_interact_remote", &s.interactRemoteMode, s.interactRemoteMode, VF_DUMPTOCHAIR, "Viewmodel Tweaks: animate the remote-manipulation (psi) mode too (0/1)");
    REGISTER_CVAR2("vm_interact_max_forward", &s.interactMaxForward, s.interactMaxForward, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach envelope - furthest forward (m, view space)");
    REGISTER_CVAR2("vm_interact_exam_max_forward", &s.interactExamMaxForward, s.interactExamMaxForward, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach envelope on screens (examination mode) - furthest forward the wrist goes (m)");
    REGISTER_CVAR2("vm_interact_min_forward", &s.interactMinForward, s.interactMinForward, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach envelope - nearest (m)");
    REGISTER_CVAR2("vm_interact_max_side", &s.interactMaxSide, s.interactMaxSide, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach envelope - left/right of the eye (m)");
    REGISTER_CVAR2("vm_interact_max_up", &s.interactMaxUp, s.interactMaxUp, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach envelope - above the eye (m)");
    REGISTER_CVAR2("vm_interact_max_down", &s.interactMaxDown, s.interactMaxDown, VF_DUMPTOCHAIR, "Viewmodel Tweaks: reach envelope - below the eye (m)");
    REGISTER_CVAR2("vm_interact_test_x", &s.interactTestX, s.interactTestX, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fixed test point, right (m)");
    REGISTER_CVAR2("vm_interact_test_y", &s.interactTestY, s.interactTestY, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fixed test point, forward (m)");
    REGISTER_CVAR2("vm_interact_test_z", &s.interactTestZ, s.interactTestZ, VF_DUMPTOCHAIR, "Viewmodel Tweaks: fixed test point, up (m)");
    RegisterReachStyleCVars(s.press, "press_", "press");
    RegisterReachStyleCVars(s.grab, "grab_", "grab");
    RegisterReachStyleCVars(s.pressExam, "press_exam_", "press on screens");
    RegisterReachStyleCVars(s.punch, "punch_", "punch (quick melee)");
    REGISTER_CVAR2("vm_melee", &s.meleeEnabled, s.meleeEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: quick melee on a key - the punch plays and a wrench hit lands at its apex (0/1)");
    REGISTER_CVAR2("vm_melee_key", &s.meleeKey, s.meleeKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: quick melee key (EKeyId, default 46 = V)");
    REGISTER_CVAR2("vm_melee_consume_key", &s.meleeConsumeKey, s.meleeConsumeKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: swallow the quick melee key (0/1)");
    REGISTER_CVAR2("vm_melee_damage", &s.meleeDamage, s.meleeDamage, VF_DUMPTOCHAIR, "Viewmodel Tweaks: quick melee damage relative to a wrench hit");
    REGISTER_CVAR2("vm_melee_cooldown", &s.meleeCooldown, s.meleeCooldown, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds between quick melee punches");
    REGISTER_CVAR2("vm_melee_cam_kick", &s.meleeCamKick, s.meleeCamKick, VF_DUMPTOCHAIR, "Viewmodel Tweaks: quick melee camera pitch kick (deg)");
    REGISTER_CVAR2("vm_melee_cam_kick_yaw", &s.meleeCamKickYaw, s.meleeCamKickYaw, VF_DUMPTOCHAIR, "Viewmodel Tweaks: quick melee camera yaw kick (deg)");
    REGISTER_CVAR2("vm_melee_cam_kick_time", &s.meleeCamKickTime, s.meleeCamKickTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds the camera kick takes");
    REGISTER_CVAR2("vm_melee_sound", &s.meleeSound, s.meleeSound, VF_DUMPTOCHAIR, "Viewmodel Tweaks: play a swing sound with the punch (0/1)");
    REGISTER_CVAR2("vm_melee_impulse_flip", &s.meleeImpulseFlip, s.meleeImpulseFlip, VF_DUMPTOCHAIR, "Viewmodel Tweaks: flip the physics impulse of the quick melee hit (0/1)");
    REGISTER_CVAR2("vm_melee_impulse_scale", &s.meleeImpulseScale, s.meleeImpulseScale, VF_DUMPTOCHAIR, "Viewmodel Tweaks: scale of the physics impulse of the quick melee hit");
    REGISTER_CVAR2("vm_melee_lower_time", &s.meleeLowerTime, s.meleeLowerTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds to lower the equipped weapon for the punch and to bring it back");
    RegisterPoseCVars(s.meleeLower, "melee_lower_", "quick melee - equipped weapon lowered while the punch plays");
    REGISTER_CVAR2("vm_melee_while_aiming", &s.meleeWhileAiming, s.meleeWhileAiming, VF_DUMPTOCHAIR, "Viewmodel Tweaks: quick melee while aiming down sights (0/1)");
    if (gEnv && gEnv->pConsole)
        gEnv->pConsole->RegisterString("vm_melee_sound_name", "Play_Player_Throw", VF_DUMPTOCHAIR, "Viewmodel Tweaks: audio trigger played when the punch starts (a wwise event name; Play_Player_Throw is the throw whoosh)");
    REGISTER_CVAR2("vm_interact_carry_hold", &s.interactCarryHoldTime, s.interactCarryHoldTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: carrying - seconds the key has to be held before the grab starts (a tap does nothing)");
    REGISTER_CVAR2("vm_interact_hidden_start", &s.interactHiddenStart, s.interactHiddenStart, VF_DUMPTOCHAIR, "Viewmodel Tweaks: with the support hand off the weapon (one-handed / none) the hand comes up from a fixed spot below the view (0/1)");
    REGISTER_CVAR2("vm_interact_start_x", &s.interactStartX, s.interactStartX, VF_DUMPTOCHAIR, "Viewmodel Tweaks: that spot, right (m)");
    REGISTER_CVAR2("vm_interact_start_y", &s.interactStartY, s.interactStartY, VF_DUMPTOCHAIR, "Viewmodel Tweaks: that spot, forward (m)");
    REGISTER_CVAR2("vm_interact_start_z", &s.interactStartZ, s.interactStartZ, VF_DUMPTOCHAIR, "Viewmodel Tweaks: that spot, up (m)");
    RegisterPoseCVars(s.interactStartShoulder, "interact_start_shoulder_", "hand off the weapon - shoulder moved while the hand is up");
    RegisterPoseCVars(s.interactStartElbow, "interact_start_elbow_", "hand off the weapon - elbow moved while the hand is up");
    REGISTER_CVAR2("vm_interact_no_context_fallback", &s.interactNoContextFallback, s.interactNoContextFallback, VF_DUMPTOCHAIR, "Viewmodel Tweaks: before any weapon was ever equipped, drive the hand with our own pose modifier (0/1)");
    REGISTER_CVAR2("vm_melee_lower_ease", &s.meleeLowerEase, s.meleeLowerEase, VF_DUMPTOCHAIR, "Viewmodel Tweaks: easing of the weapon lowering for the punch (0 linear, 1 smooth, 2 ease out, 3 ease in, 4 in-out)");

    REGISTER_CVAR2("vm_world_fov_enabled", &s.worldFovEnabled, s.worldFovEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: override the game's horizontal FOV, cl_hfov (0/1)");
    REGISTER_CVAR2("vm_world_fov", &s.worldFov, s.worldFov, VF_DUMPTOCHAIR, "Viewmodel Tweaks: horizontal FOV in degrees");
    REGISTER_CVAR2("vm_sprint_sens_enabled", &s.sprintSensEnabled, s.sprintSensEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: override the look-sensitivity scale while sprinting (0/1)");
    REGISTER_CVAR2("vm_sprint_sens_scale", &s.sprintSensScale, s.sprintSensScale, VF_DUMPTOCHAIR, "Viewmodel Tweaks: look-sensitivity multiplier while sprinting (1 = same as walking)");
    // feel: sprint
    REGISTER_CVAR2("vm_sprint_pose", &s.sprintPoseEnabled, s.sprintPoseEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: lower / tilt the weapon while sprinting (0/1)");
    RegisterPoseCVars(s.sprint, "sprint_", "sprint pose, added while sprinting");
    REGISTER_CVAR2("vm_sprint_blend_in", &s.sprintBlendIn, s.sprintBlendIn, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds to blend into the sprint pose");
    REGISTER_CVAR2("vm_sprint_blend_out", &s.sprintBlendOut, s.sprintBlendOut, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds to blend out of the sprint pose");
    REGISTER_CVAR2("vm_sprint_blocks_aim", &s.sprintBlocksAim, s.sprintBlocksAim, VF_DUMPTOCHAIR, "Viewmodel Tweaks: no aiming down sights while sprinting (0/1)");
    REGISTER_CVAR2("vm_sprint_zerog", &s.sprintInZeroG, s.sprintInZeroG, VF_DUMPTOCHAIR, "Viewmodel Tweaks: treat the zero-G thruster boost as sprinting (pose, sway, aim block) (0/1)");
    REGISTER_CVAR2("vm_sprint_sway", &s.sprintSwayEnabled, s.sprintSwayEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: extra weapon sway while sprinting (0/1)");
    REGISTER_CVAR2("vm_sprint_sway_pos", &s.sprintSwayPos, s.sprintSwayPos, VF_DUMPTOCHAIR, "Viewmodel Tweaks: sprint sway amplitude in meters");
    REGISTER_CVAR2("vm_sprint_sway_rot", &s.sprintSwayRot, s.sprintSwayRot, VF_DUMPTOCHAIR, "Viewmodel Tweaks: sprint sway roll amplitude in degrees");
    REGISTER_CVAR2("vm_sprint_sway_freq", &s.sprintSwayFreq, s.sprintSwayFreq, VF_DUMPTOCHAIR, "Viewmodel Tweaks: sprint sway frequency in Hz");
    // feel: aim sway
    REGISTER_CVAR2("vm_sway_enabled", &s.aimSwayEnabled, s.aimSwayEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: sway and settle while aiming (0/1)");
    REGISTER_CVAR2("vm_sway_pos", &s.aimSwayPos, s.aimSwayPos, VF_DUMPTOCHAIR, "Viewmodel Tweaks: aim sway position amplitude at rest, meters");
    REGISTER_CVAR2("vm_sway_rot", &s.aimSwayRot, s.aimSwayRot, VF_DUMPTOCHAIR, "Viewmodel Tweaks: aim sway rotation amplitude at rest, degrees");
    REGISTER_CVAR2("vm_sway_freq", &s.aimSwayFreq, s.aimSwayFreq, VF_DUMPTOCHAIR, "Viewmodel Tweaks: aim sway frequency in Hz");
    REGISTER_CVAR2("vm_sway_initial", &s.aimSwayInitial, s.aimSwayInitial, VF_DUMPTOCHAIR, "Viewmodel Tweaks: sway multiplier when the sights come up");
    REGISTER_CVAR2("vm_sway_settle", &s.aimSwaySettleTime, s.aimSwaySettleTime, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds for the sway to settle");
    REGISTER_CVAR2("vm_sway_move", &s.aimSwayMoveMult, s.aimSwayMoveMult, VF_DUMPTOCHAIR, "Viewmodel Tweaks: extra sway at walking speed");
    REGISTER_CVAR2("vm_sway_sprint_penalty", &s.aimSwaySprintPenalty, s.aimSwaySprintPenalty, VF_DUMPTOCHAIR, "Viewmodel Tweaks: extra sway right after sprinting");
    REGISTER_CVAR2("vm_sway_sprint_recover", &s.aimSwaySprintRecover, s.aimSwaySprintRecover, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds for the sprint penalty to fade");
    REGISTER_CVAR2("vm_steady_enabled", &s.steadyEnabled, s.steadyEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: hold-breath key steadies the sights (0/1)");
    REGISTER_CVAR2("vm_steady_key", &s.steadyKey, s.steadyKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: EKeyId of the steady key (0 = none)");
    REGISTER_CVAR2("vm_steady_reduce", &s.steadyReduce, s.steadyReduce, VF_DUMPTOCHAIR, "Viewmodel Tweaks: sway reduction while steady (0..1)");
    REGISTER_CVAR2("vm_steady_duration", &s.steadyDuration, s.steadyDuration, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds of breath");
    REGISTER_CVAR2("vm_steady_recover", &s.steadyRecover, s.steadyRecover, VF_DUMPTOCHAIR, "Viewmodel Tweaks: seconds to recover a full breath");
    REGISTER_CVAR2("vm_steady_consume_key", &s.steadyConsumeKey, s.steadyConsumeKey, VF_DUMPTOCHAIR, "Viewmodel Tweaks: swallow the steady key (0/1)");
    // feel: view drag
    REGISTER_CVAR2("vm_drag_enabled", &s.dragEnabled, s.dragEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: GoldenEye-style view drag (0/1)");
    REGISTER_CVAR2("vm_drag_lead", &s.dragLead, s.dragLead, VF_DUMPTOCHAIR, "Viewmodel Tweaks: 1 = weapon leads into the turn, 0 = lags behind");
    REGISTER_CVAR2("vm_drag_pos", &s.dragPos, s.dragPos, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag meters per rad/s");
    REGISTER_CVAR2("vm_drag_rot", &s.dragRot, s.dragRot, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag degrees per rad/s");
    REGISTER_CVAR2("vm_drag_stiffness", &s.dragStiffness, s.dragStiffness, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag spring stiffness");
    REGISTER_CVAR2("vm_drag_damping", &s.dragDamping, s.dragDamping, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag damping ratio");
    REGISTER_CVAR2("vm_drag_max_pos", &s.dragMaxPos, s.dragMaxPos, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag position clamp, meters");
    REGISTER_CVAR2("vm_drag_max_rot", &s.dragMaxRot, s.dragMaxRot, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag rotation clamp, degrees");
    REGISTER_CVAR2("vm_drag_aim_scale", &s.dragAimScale, s.dragAimScale, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag while aiming, 0..1 of the hip amount");
    REGISTER_CVAR2("vm_drag_pitch_scale", &s.dragPitchScale, s.dragPitchScale, VF_DUMPTOCHAIR, "Viewmodel Tweaks: view drag pitch (look up/down) relative to yaw");

    REGISTER_CVAR2("vm_fov_enabled", &s.fovEnabled, s.fovEnabled, VF_DUMPTOCHAIR, "Viewmodel Tweaks: enable weapon FOV override (0/1)");
    REGISTER_CVAR2("vm_fov", &s.fov, s.fov, VF_DUMPTOCHAIR, "Viewmodel Tweaks: weapon FOV in degrees (game default 55)");
    REGISTER_CVAR2("vm_gui_mouse", &s.guiMouse, s.guiMouse, VF_DUMPTOCHAIR, "Viewmodel Tweaks: show cursor and block look input while the settings window is open (0/1)");
    REGISTER_CVAR2("vm_show_window", &s.showWindow, s.showWindow, VF_DUMPTOCHAIR, "Viewmodel Tweaks: show the settings window in the Chairloader GUI (0/1)");
    REGISTER_CVAR2("vm_spread_debug", &s.spreadDebug, s.spreadDebug, VF_DUMPTOCHAIR, "Viewmodel Tweaks: log the full spread picture (cone, dispersion, aim offset) on every shotgun/pistol shot (0/1)");
    REGISTER_CVAR2("vm_show_advanced", &s.showAdvanced, s.showAdvanced, VF_DUMPTOCHAIR, "Viewmodel Tweaks: show diagnostics, self-tests and experimental features in the window (0/1)");
}

void ModMain::InitSystem(const ModInitInfo& initInfo, ModDllInfo& dllInfo)
{
    BaseClass::InitSystem(initInfo, dllInfo);
    RegisterCVars();
    SanitizeSettings();
    m_offsetHookActive = s_hookPwaUpdate.IsHooked() && s_hookPwaContextUpdate.IsHooked();
    m_cameraHookActive = s_hookUpdateView.IsHooked();
    CryLog("ViewmodelTweaks: initialized (weapon offset hooks {}, camera hook {})", m_offsetHookActive ? "installed" : "NOT installed",
        m_cameraHookActive ? "installed" : "NOT installed");
}

void ModMain::InitGame(bool isHotReloading)
{
    BaseClass::InitGame(isHotReloading);
    if (gEnv && gEnv->pInput && !m_inputListenerRegistered)
    {
        gEnv->pInput->AddEventListener(this);
        m_inputListenerRegistered = true;
    }
    LoadWeapons();
    LoadPoses();
}

//---------------------------------------------------------------------------------
// Mod Shutdown
//---------------------------------------------------------------------------------
void ModMain::ShutdownGame(bool isHotUnloading)
{
    if (m_weaponsDirty)
        SaveWeapons();
    if (m_posesDirty)
        SavePoses();
    if (gEnv && gEnv->pInput && m_inputListenerRegistered)
    {
        gEnv->pInput->RemoveEventListener(this);
        m_inputListenerRegistered = false;
    }
    m_aimKeyHeld = false;
    m_aimBlend = 0.0f;
    m_interact = InteractState(); // a stored Interact call must not survive the player it belongs to
    m_examZoomHandle = 0;
    UpdateReticle(); // un-hide if we hid it
    UpdateCameraZoom(false);
    UpdateMouseCapture(false);
    BaseClass::ShutdownGame(isHotUnloading);
}


//! Removes every console variable with the given prefix. The console keeps raw pointers to the names, help
//! strings and value storage we registered - all of which vanish with this DLL - and the engine touches them
//! again at shutdown (crash on exit) unless they are gone before the module is unloaded.
static void UnregisterCVarsWithPrefix(const char* prefix)
{
    if (!gEnv || !gEnv->pConsole)
        return;
    const int total = gEnv->pConsole->GetNumVars(false);
    if (total <= 0)
        return;
    std::vector<const char*> names((size_t)total + 1, nullptr);
    const size_t n = gEnv->pConsole->GetSortedVars(names.data(), names.size(), prefix);
    std::vector<std::string> copies;
    for (size_t i = 0; i < n && i < names.size(); i++)
        if (names[i])
            copies.emplace_back(names[i]);
    for (const std::string& name : copies)
        gEnv->pConsole->UnregisterVariable(name.c_str(), true);
    CryLog("Unregistered {} console variable(s) with prefix '{}'", copies.size(), prefix);
}

void ModMain::ShutdownSystem(bool isHotUnloading)
{
    // Restore the stock weapon FOV so unloading leaves the game as we found it.
    if (gEnv && gEnv->pRenderer && m_lastAppliedFov >= 0.0f)
    {
        float stock = PreyInternals::STOCK_WEAPON_FOV;
        gEnv->pRenderer->EF_Query(EFQ_SetDrawNearFov, stock);
    }
    UnregisterCVarsWithPrefix("vm_");
    BaseClass::ShutdownSystem(isHotUnloading);
}

//---------------------------------------------------------------------------------
// Main Update Loop
//---------------------------------------------------------------------------------
//! Debug markers for the interaction reach, drawn with ImGui's foreground draw list: ImGui has a live frame
//! during MainUpdate whether or not the Chairloader GUI is shown (mods' Draw() only runs with it shown), and
//! the aux-geom text path (IRenderAuxGeom::RenderText -> gRenDev->FlushTextMessages) draws nothing in Prey.
void ModMain::DrawInteractMarkers(float dt)
{
    InteractState& I = m_interact;
    if (!m_settings.interactDebugMarker || !gEnv || !gEnv->pSystem)
        return;
    I.markerTimer = max(I.markerTimer - clamp_tpl(dt, 0.0f, 0.1f), 0.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl)
        return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x < 8.0f || ds.y < 8.0f)
        return;
    const CCamera& cam = gEnv->pSystem->GetViewCamera();
    auto mark = [&](const Vec3& world, const char* label, ImU32 col) {
        Vec3 scr;
        if (!Finite(world) || !cam.Project(world, scr, Vec2i(0, 0), Vec2i((int)ds.x, (int)ds.y)))
            return;
        const ImVec2 p(scr.x, scr.y);
        dl->AddCircle(p, 9.0f, col, 16, 2.0f);
        dl->AddText(ImVec2(p.x + 11.0f, p.y - 8.0f), col, label);
    };
    ArkPlayer* pP = ArkPlayer::GetInstancePtr();
    IEntity* pEnt = pP ? pP->GetEntity() : nullptr;
    if (I.hasWorldTarget)
        mark(I.targetWorld, "target", IM_COL32(255, 60, 60, 255));
    if (pEnt && m_render.camValid && Finite(I.desiredView))
    {
        const Vec3 model = m_render.camModel * I.desiredView;
        mark(pEnt->GetWorldPos() + pEnt->GetWorldRotation() * model, "hand asked", IM_COL32(60, 255, 60, 255));
    }
    if (pEnt && m_render.camValid && I.handActualValid && (I.phase != InteractState::Idle || I.restBlend > 0.0f))
    {
        const Vec3 model = m_render.camModel * I.handActualView;
        mark(pEnt->GetWorldPos() + pEnt->GetWorldRotation() * model, "hand actual (wrist joint)", IM_COL32(60, 220, 255, 255));
    }
    // crosshair reference (screen space, no projection involved)
    dl->AddCircle(ImVec2(ds.x * 0.5f, ds.y * 0.5f), 4.0f, IM_COL32(255, 255, 255, 160), 12, 1.0f);
    // the shoulder (left upper-arm joint), so the reach geometry can be judged
    if (pEnt && m_render.camValid && I.shoulderValid)
    {
        const Vec3 model = m_render.camModel * I.shoulderView;
        mark(pEnt->GetWorldPos() + pEnt->GetWorldRotation() * model, "shoulder", IM_COL32(255, 160, 255, 255));
    }
    const float residual = (I.handActualValid && Finite(I.desiredView)) ? (I.desiredView - I.handActualView).GetLength() : -1.0f;
    const float shoulderDist = (I.shoulderValid && Finite(I.desiredView)) ? (I.desiredView - I.shoulderView).GetLength() : -1.0f;
    char buf[520];
    snprintf(buf, sizeof(buf), "[vm debug] %s%s%s auto body fwd %.2f | target (%.2f %.2f %.2f)%s  asked (%.2f %.2f %.2f)  actual (%.2f %.2f %.2f)  residual %.2f m%s  corr %.2f%s | shoulder (%.2f %.2f %.2f) -> asked %.2f m | anim hand (%.2f %.2f %.2f) curve %.2f rest %.2f | chain resets %d",
        I.examining ? "screen" : "not on a screen", I.unarmed ? ", no weapon" : "", I.examTooClose ? " TOO CLOSE - no reach" : "", I.autoBodyY,
        I.targetView.x, I.targetView.y, I.targetView.z, I.clamped ? " CLAMPED" : "", I.desiredView.x, I.desiredView.y, I.desiredView.z,
        I.handActualView.x, I.handActualView.y, I.handActualView.z, residual, residual > 0.06f ? " (IK NOT REACHING)" : "",
        I.corr.GetLength(), I.corr.GetLength() > 0.34f ? " SATURATED" : "",
        I.shoulderView.x, I.shoulderView.y, I.shoulderView.z, shoulderDist,
        I.handView.x, I.handView.y, I.handView.z, I.curve, I.restBlend, I.chainResets);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddRectFilled(ImVec2(ds.x * 0.5f - ts.x * 0.5f - 6.0f, ds.y - 60.0f), ImVec2(ds.x * 0.5f + ts.x * 0.5f + 6.0f, ds.y - 60.0f + ts.y + 6.0f), IM_COL32(0, 0, 0, 160));
    dl->AddText(ImVec2(ds.x * 0.5f - ts.x * 0.5f, ds.y - 57.0f), IM_COL32(255, 255, 255, 255), buf);
}

void ModMain::UpdateBeforeSystem(unsigned updateFlags)
{
    // A deferred interaction is made here, before the game's own update, i.e. in the same window the
    // input-driven call would normally happen in.
    FireDeferredInteract();
    // No weapon animation context yet (nothing ever equipped) -> our own pose modifier carries the hand.
    PushWithOwnQueue();
}

// CryCreateClassInstance(const char* className, std::shared_ptr<T>& out) - the factory the game's own procedural
// context uses for its operator queue ("AnimationPoseModifier_OperatorQueue", CProceduralWeaponAnimationContext::
// Initialize at 0x17D5B88).
static auto s_fnCryCreateClassInstance = PreyFunction<bool(const char* className, void* pSharedPtrOut)>(0x2C3530);
// IAnimationPoseModifier IID the context passes to the queue's QueryInterface (slot 2) before PushPoseModifier
// (0x17D4E95 -> .rdata 0x1CE6508).
static const unsigned char s_iidAnimationPoseModifier[16] = { 0x7f, 0x44, 0x42, 0x5e, 0x75, 0x47, 0xfe, 0x22, 0x49, 0xf4, 0x9a, 0xd3, 0x4e, 0x27, 0xb6, 0xba };

// A pointer that can be read and whose first word (the vtable) points into the game module. Under SEH.
static bool LooksLikeGameObject(const void* p, uintptr_t moduleBase, uintptr_t moduleEnd)
{
    if (!p || (uintptr_t)p < 0x10000 || (uintptr_t)p > 0x00007FFFFFFFFFFFull)
        return false;
    __try
    {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
        return vt >= moduleBase && vt < moduleEnd;
    }
    __except (1)
    {
        return false;
    }
}

static bool SafePushPoseModifier(void* pSkelAnim, size_t slot, unsigned layer, const void* pSharedPtr, const char* name)
{
    using Fn = void (*)(void*, unsigned, const void*, const char*);
    Fn fn = reinterpret_cast<Fn>((*reinterpret_cast<void***>(pSkelAnim))[slot]);
    __try
    {
        fn(pSkelAnim, layer, pSharedPtr, name);
        return true;
    }
    __except (1)
    {
        return false;
    }
}

void ModMain::PushWithOwnQueue()
{
    using namespace PreyInternals;
    InteractState& I = m_interact;
    const ViewmodelSettings& s = m_settings;
    I.ownQueueUsed = false;
    if (!s.interactNoContextFallback || m_ownQueueBroken || !Active() || !s.interactEnabled || !m_offsetHookActive || m_playerDead)
        return;
    // Only when the game's context did not run last frame: otherwise it carries our pushes (and a second modifier
    // would apply them twice).
    if (m_diag.ctxUpdatesLastFrame > 0)
        return;
    // Anything to push at all? (The reach, the resting hand, a pose being edited.)
    const bool wanted = I.phase != InteractState::Idle || I.restBlend > 0.0f || I.holdMode != 0 || I.restActive;
    if (!wanted)
        return;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
    ICharacterInstance* pChar = pEnt ? pEnt->GetCharacter(0) : nullptr;
    if (!pChar)
        return;
    const uintptr_t base = GetModuleBase();
    const uintptr_t end = base + 0x4000000; // PreyDll is ~48 MB; anything within 64 MB of the base is "in the module"
    if (!base)
        return;

    if (!m_ownQueue && !m_ownQueueTried)
    {
        m_ownQueueTried = true;
        struct { void* ptr; void* ctrl; } sp = { nullptr, nullptr };
        if (s_fnCryCreateClassInstance("AnimationPoseModifier_OperatorQueue", &sp) && sp.ptr && sp.ctrl)
        {
            if (!LooksLikeGameObject(sp.ptr, base, end))
            {
                CryLog("ViewmodelTweaks: own operator queue - the created object does not look like one ({}), fallback disabled", sp.ptr);
                m_ownQueueBroken = true;
                return;
            }
            void* pm = VCall<void*>(sp.ptr, 2, (const void*)s_iidAnimationPoseModifier); // ICryUnknown::QueryInterface
            if (!LooksLikeGameObject(pm, base, end))
            {
                CryLog("ViewmodelTweaks: own operator queue - QueryInterface gave {} for the pose-modifier interface (object {}), fallback disabled", pm, sp.ptr);
                m_ownQueueBroken = true;
                return;
            }
            m_ownQueue = sp.ptr;
            m_ownQueueCtrl = sp.ctrl; // kept for the life of the DLL (one small object)
            m_ownQueuePM = pm;
            CryLog("ViewmodelTweaks: created our own AnimationPoseModifier_OperatorQueue ({}), pose-modifier interface {} - carries the hand while the game's weapon animation context does not exist", m_ownQueue, m_ownQueuePM);
        }
        else
        {
            CryLog("ViewmodelTweaks: could not create an AnimationPoseModifier_OperatorQueue - no hand until a weapon has been equipped once");
            m_ownQueueBroken = true;
            return;
        }
    }
    if (!m_ownQueue || !m_ownQueuePM)
        return;

    // Same thing the game's context does every frame: hand the queue to the skeleton (layer 6, named), clear it,
    // then fill it.
    void* pSkelAnim = VCall<void*>(pChar, 0x28 / 8);
    void* pSkelPose = VCall<void*>(pChar, VT_ICharacterInstance_GetISkeletonPose);
    if (!pSkelAnim || !pSkelPose || !LooksLikeGameObject(pSkelAnim, base, end))
        return;
    struct { void* ptr; void* ctrl; } spPM = { m_ownQueuePM, m_ownQueueCtrl };
    if (!SafePushPoseModifier(pSkelAnim, 0x120 / 8, 6u, &spPM, "ProceduralWeapon"))
    {
        CryLog("ViewmodelTweaks: own operator queue - PushPoseModifier faulted, fallback disabled");
        m_ownQueueBroken = true;
        return;
    }
    VCall<void>(m_ownQueue, 0x70 / 8); // the queue's Clear() - what the context calls right after pushing the modifier

    // The context's joint ids are what the pushes address; without the context they come from the skeleton by name.
    if (m_lock.leftIkJoint < 0)
    {
        void* pSkel = VCall<void*>(pChar, VT_ICharacterInstance_GetIDefaultSkeleton);
        if (pSkel)
            m_lock.leftIkJoint = VCall<int>(pSkel, VT_IDefaultSkeleton_GetJointIDByName, "l_hand_spine_target");
    }
    QuatT camAbs(IDENTITY);
    if (!PredictCamera(pPlayer, camAbs))
        return;
    PushInteractReach(m_ownQueue, pSkelPose, camAbs);
    I.ownQueueUsed = true;
    I.ownQueuePushes++;
}

void ModMain::MainUpdate(unsigned updateFlags)
{
    m_frameIndex++;
    // Time step for the filters (blends, convergence, wall pull-back, feel). This must be a real,
    // per-update delta: an exponential smoother uses k = 1 - exp(-dt/tau), which is exactly 0 when dt is
    // 0 (the value never moves - the filter looks "dead") and 1 when dt is huge (the value snaps - the
    // filter looks "off"). So a wrong dt does not merely mistune the smoothing, it disables it.
    //
    // The game frame time (ITimer::GetFrameTime) is the game clock: it is scaled by the trainer's time
    // dilation and paused with the game, and reads oddly here. The async *seconds* accessor is a float32
    // of absolute time, which after a while cannot resolve a 16 ms step at all (huge value minus huge
    // value rounds to 0). The async clock as an int64 tick count (100000/s) is precise and monotonic, so
    // the step between two updates is taken from that. GetFrameTime is kept only for the on-screen readout.
    m_dtGame = (gEnv && gEnv->pTimer) ? gEnv->pTimer->GetFrameTime() : 0.0f;
    float dt = 1.0f / 60.0f; // sane default until the clock gives us two readings (never 0 -> never frozen)
    if (gEnv && gEnv->pTimer)
    {
        const int64 nowTicks = gEnv->pTimer->GetAsyncTime().GetValue();
        if (m_lastAsyncTicks >= 0 && nowTicks > m_lastAsyncTicks)
        {
            const float delta = (float)(nowTicks - m_lastAsyncTicks) * (1.0f / 100000.0f);
            if (Finite(delta) && delta > 0.0f)
            {
                dt = clamp_tpl(delta, 0.0f, 0.1f); // a longer gap is a load / alt-tab / pause, not a frame
                m_updateHz += (1.0f / clamp_tpl(delta, 1e-4f, 1.0f) - m_updateHz) * 0.05f;
            }
        }
        m_lastAsyncTicks = nowTicks;
    }
    m_dtUsed = dt;
    UpdateBlendStates(dt);
    UpdateFeel(dt, ArkPlayer::GetInstancePtr());
    SanitizeFeel();
    UpdateNudge(dt);
    UpdateConvergence(dt);
    UpdateInteract(dt);
    ApplyExamineCVars();
    DrawInteractMarkers(dt);
    RecordTrace();

    if (m_weaponsDirty)
    {
        m_weaponsSaveTimer += dt;
        if (m_weaponsSaveTimer > 2.0f)
        {
            SaveWeapons();
            m_weaponsSaveTimer = 0.0f;
        }
    }

    if (m_posesDirty)
    {
        m_posesSaveTimer += dt;
        if (m_posesSaveTimer > 2.0f)
        {
            SavePoses();
            m_posesSaveTimer = 0.0f;
        }
    }
    else
        m_posesSaveTimer = 0.0f;

    UpdateReticle();
    UpdateCameraInputOverrides();

    bool want = m_settings.guiMouse != 0 && m_settings.showWindow != 0 && gCL && gCL->gui && gCL->gui->IsEnabled();
    // Player gone (level unload / main menu) -> the input-mode handle is stale, release everything.
    if (m_mouseCaptured && !ArkPlayer::GetInstancePtr())
        want = false;
    UpdateMouseCapture(want);
}

void ModMain::UpdateMouseCapture(bool wantCapture)
{
    if (wantCapture == m_mouseCaptured)
        return;

    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();

    if (wantCapture)
    {
        // Only meaningful while in a level; in the main menu the cursor is already visible.
        if (!pPlayer)
            return;

        if (gEnv && gEnv->pHardwareMouse)
            gEnv->pHardwareMouse->IncrementCounter();

        // Switch to "menu" input mode so mouse movement drives the cursor instead of the camera.
        m_inputModeHandle = pPlayer->m_input.EnableInputMode(ArkPlayerInput::Mode::menu);
        m_mouseCaptured = true;
    }
    else
    {
        if (gEnv && gEnv->pHardwareMouse)
            gEnv->pHardwareMouse->DecrementCounter();

        if (pPlayer && m_inputModeHandle >= 0)
            pPlayer->m_input.DisableInputMode(m_inputModeHandle);

        m_inputModeHandle = -1;
        m_mouseCaptured = false;
    }
}

void ModMain::LateUpdate(unsigned updateFlags)
{
    // Runs after the game's ArkPlayerZoomManager::Update for this frame.
    EnforceWeaponFov();

    // Pipeline diagnostics: did the weapon offset code run this frame?
    PipelineDiag& d = m_diag;
    if (!m_currentWeaponClass.empty() && m_settings.enabled)
    {
        if (d.pwaUpdatesThisFrame == 0)
        {
            d.missingFrames++;
            if (Now() - d.lastMissingLog > 1.0f)
            {
                char buf[160];
                snprintf(buf, sizeof(buf), "no weapon-offset update this frame (ctx updates: %d, instanceCount %d, modifier %s)",
                    d.ctxUpdatesThisFrame, d.lastInstanceCount, d.lastModifierNull ? "NULL" : "ok");
                d.Add(Now(), buf);
                d.lastMissingLog = Now();
            }
        }
        else
        {
            if (d.missingFrames > 0)
            {
                char buf[96];
                snprintf(buf, sizeof(buf), "weapon-offset updates resumed after %d frame(s)", d.missingFrames);
                d.Add(Now(), buf);
            }
            d.missingFrames = 0;
        }
    }
    d.pwaUpdatesLastFrame = d.pwaUpdatesThisFrame;
    d.ctxUpdatesLastFrame = d.ctxUpdatesThisFrame;
    d.pwaUpdatesThisFrame = 0;
    d.ctxUpdatesThisFrame = 0;
}

//---------------------------------------------------------------------------------
// GUI
//---------------------------------------------------------------------------------
void ModMain::Draw()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Viewmodel Tweaks"))
        {
            bool show = m_settings.showWindow != 0;
            if (ImGui::MenuItem("Settings window", nullptr, &show))
                m_settings.showWindow = show ? 1 : 0;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (m_settings.showWindow)
        DrawWindow();
}

static bool SliderCm(const char* label, float& meters, float rangeCm, const char* tooltip)
{
    float cm = meters * 100.0f;
    bool changed = ImGui::SliderFloat(label, &cm, -rangeCm, rangeCm, "%.2f cm");
    if (ImGui::IsItemHovered() && tooltip)
        ImGui::SetTooltip("%s", tooltip);
    if (changed)
        meters = cm / 100.0f;
    return changed;
}

static bool SliderDeg(const char* label, float& deg, float range, const char* tooltip)
{
    bool changed = ImGui::SliderFloat(label, &deg, -range, range, "%.2f deg");
    if (ImGui::IsItemHovered() && tooltip)
        ImGui::SetTooltip("%s", tooltip);
    return changed;
}

static bool CheckboxInt(const char* label, int& value, const char* tooltip = nullptr)
{
    bool b = value != 0;
    bool changed = ImGui::Checkbox(label, &b);
    if (changed)
        value = b ? 1 : 0;
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return changed;
}

static void TextQuatT(const char* label, const QuatT& q)
{
    Ang3 a(q.q);
    ImGui::Text("%s: pos (%.3f, %.3f, %.3f) m  rot (p %.1f, r %.1f, y %.1f) deg", label,
        q.t.x, q.t.y, q.t.z, RAD2DEG(a.x), RAD2DEG(a.y), RAD2DEG(a.z));
}

static void DrawReticleControls(ViewmodelSettings& s, const char* id)
{
    ImGui::PushID(id);
    ImGui::Text("Reticle position");
    ImGui::SameLine();
    ImGui::RadioButton("Game default", &s.reticleMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Centered", &s.reticleMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Custom", &s.reticleMode, 2);
    if (s.reticleMode == 2)
        ImGui::SliderFloat("Vertical position", &s.reticleY, 0.2f, 0.8f, "%.3f of screen height");
    ImGui::Text("Reticle style   ");
    ImGui::SameLine();
    ImGui::RadioButton("Game default##style", &s.reticleStyle, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Weapon reticle", &s.reticleStyle, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Simple dot", &s.reticleStyle, 2);
    ImGui::SameLine();
    ImGui::RadioButton("Hidden", &s.reticleStyle, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("hud_reticleSetting: the game's own reticle options. While you use an in-world screen the reticle\ncomes back as the cursor and is hidden again afterwards.");
    CheckboxInt("Hide reticle while aiming", s.aimHideReticle, "Switches the reticle off once the weapon is more than halfway up to the sights, and back when you lower it.");
    ImGui::PopID();
}

bool ModMain::DrawPoseSliders(PoseOffset& pose, const char* id, float posRangeCm, float rotRangeDeg)
{
    bool ch = false;
    ImGui::PushID(id);
    ImGui::Text("Position (view space)");
    ch |= SliderCm("Right / Left", pose.posX, posRangeCm, "Positive moves the weapon to the right.");
    ch |= SliderCm("Forward / Back", pose.posY, posRangeCm, "Positive moves the weapon away from the camera.");
    ch |= SliderCm("Up / Down", pose.posZ, posRangeCm, "Positive moves the weapon up.");
    ImGui::Spacing();
    ImGui::Text("Rotation");
    ch |= SliderDeg("Pitch", pose.pitch, rotRangeDeg, "Positive tilts the muzzle up.");
    ch |= SliderDeg("Yaw", pose.yaw, rotRangeDeg, "Positive turns the muzzle to the left.");
    ch |= SliderDeg("Roll", pose.roll, rotRangeDeg, "Rolls the weapon around its forward axis.");
    if (ImGui::Button("Reset"))
    {
        pose.Reset();
        ch = true;
    }
    ImGui::PopID();
    return ch;
}

static void DrawReachStyle(ReachStyle& r, const char* id)
{
    ImGui::PushID(id);
    ImGui::Text("Timing");
    ImGui::SliderFloat("Reach", &r.reachTime, 0.05f, 1.0f, "%.2f s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seconds for the hand to get to the target.");
    ImGui::SliderFloat("Hold", &r.holdTime, 0.0f, 1.0f, "%.2f s");
    ImGui::SliderFloat("Return", &r.returnTime, 0.05f, 1.5f, "%.2f s");
    ImGui::SliderFloat("Amount", &r.amount, 0.0f, 1.5f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far along the way to the target the hand goes (1 = touches it, 0.5 = half way, >1 = past it).");
    ImGui::Spacing();
    ImGui::Text("Target offset (view space, from the object point)");
    SliderCm("Right / Left##off", r.offX, 80.0f, "Positive moves the hand target to the right.");
    SliderCm("Forward / Back##off", r.offY, 80.0f, "Negative stops the hand short of the surface (the fist has a size).");
    SliderCm("Up / Down##off", r.offZ, 80.0f, "Positive moves the hand target up.");
    ImGui::Spacing();
    ImGui::Text("Windup (a keyframe before the reach: the hand is pulled back first)");
    ImGui::SliderFloat("Windup time", &r.windupTime, 0.0f, 0.6f, "%.2f s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = no windup. Uses the reach easing; the pulled-back offset unwinds as the hand goes out.");
    if (r.windupTime > 0.0f)
    {
        SliderCm("Pulled right / left##wind", r.windX, 40.0f, "Relative to where the hand starts from.");
        SliderCm("Pulled forward / back##wind", r.windY, 40.0f, "Negative = back towards the body (loading the arm).");
        SliderCm("Pulled up / down##wind", r.windZ, 40.0f, nullptr);
        if (ImGui::TreeNode("Shoulder, elbow and forearm during the windup"))
        {
            SliderCm("Shoulder right / left##wsh", r.windShX, 30.0f, "The upper-arm joint moved (view space); the arm is re-solved from there.");
            SliderCm("Shoulder forward / back##wsh", r.windShY, 30.0f, nullptr);
            SliderCm("Shoulder up / down##wsh", r.windShZ, 30.0f, nullptr);
            SliderCm("Elbow right / left##wel", r.windElX, 30.0f, "The forearm joint moved; the arm IK may re-solve it, try and see.");
            SliderCm("Elbow forward / back##wel", r.windElY, 30.0f, nullptr);
            SliderCm("Elbow up / down##wel", r.windElZ, 30.0f, nullptr);
            SliderDeg("Forearm pitch##wfa", r.windFaPitch, 90.0f, "Forearm rotation about its own axes (the hand keeps its orientation).");
            SliderDeg("Forearm yaw##wfa", r.windFaYaw, 90.0f, nullptr);
            SliderDeg("Forearm roll##wfa", r.windFaRoll, 90.0f, nullptr);
            ImGui::SliderFloat("Kept through the strike", &r.windKeep, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = these unwind with the windup; 1 = they stay for the whole strike.");
            ImGui::TreePop();
        }
    }
    ImGui::Spacing();
    ImGui::Text("Path (sine bulge, peaks half-way)");
    SliderCm("Way out: sideways", r.arcX, 20.0f, "Bulge of the path towards the target, right (+) / left (-).");
    SliderCm("Way out: vertical", r.arcZ, 20.0f, "Bulge of the path towards the target, up (+) / down (-).");
    SliderCm("Way back: sideways", r.retArcX, 20.0f, "Bulge of the path back, right (+) / left (-). A grab sweeps in towards the body.");
    SliderCm("Way back: vertical", r.retArcZ, 20.0f, "Bulge of the path back, up (+) / down (-).");
    ImGui::Spacing();
    ImGui::Text("Hand rotation at the target");
    SliderDeg("Pitch##rot", r.pitch, 90.0f, "Turns the hand up (+) / down (-) as it gets there, on top of its pose.");
    SliderDeg("Yaw##rot", r.yaw, 90.0f, "Turns the hand left (+) / right (-).");
    SliderDeg("Roll##rot", r.roll, 90.0f, "Rolls the hand.");
    ImGui::Spacing();
    ImGui::Text("Reach");
    ImGui::SliderFloat("Envelope relaxed by", &r.envelopeScale, 0.5f, 3.0f, "x %.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The reach envelope's limits (forward, sideways, up, down) are multiplied by this while this animation plays - e.g. so a grab can go down to the floor without loosening the press.");
    SliderCm("Target moved into the object", r.along, 40.0f, "Along the line from the camera to the object: + puts the hand's target beyond the object's surface, - short of it.");
    if (ImGui::Button("Reset this style"))
        r = strcmp(id, "grab") == 0 ? ReachStyle::Grab() : (strcmp(id, "pressExam") == 0 ? ReachStyle::GentlePress() : (strcmp(id, "punch") == 0 ? ReachStyle::Punch() : ReachStyle()));
    ImGui::PopID();
}

void ModMain::DrawHandPoseEditor()
{
    InteractState& I = m_interact;
    RenderLockState& R = m_render;
    ViewmodelSettings& s = m_settings;

    ImGui::TextWrapped("A pose is absolute: the wrist's orientation in view space, its offset from the reach target, and every joint under "
                       "the hand as a rotation on the skeleton's bind pose. Whatever weapon is held only supplies the start of the blend; "
                       "the end pose is always this one. Saved to Vee.ViewmodelTweaks.poses.xml next to the weapons file.");
    if (!R.leftSubtree.empty() && !R.defaultPoseValid)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "The bind pose could not be read from this skeleton: finger sliders work from identity rotations instead (see the log).");

    // --- which pose each style uses ------------------------------------------------------------------
    auto poseCombo = [&](const char* label, std::string& sel) {
        const char* current = sel.empty() ? "(keep the animation)" : sel.c_str();
        if (ImGui::BeginCombo(label, current))
        {
            if (ImGui::Selectable("(keep the animation)", sel.empty())) { sel.clear(); m_posesDirty = true; }
            for (const HandPose& p : m_poses)
                if (ImGui::Selectable(p.name.c_str(), sel == p.name)) { sel = p.name; m_posesDirty = true; }
            ImGui::EndCombo();
        }
    };
    poseCombo("Press uses pose", m_stylePose[0]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("amount##press", &m_stylePoseAmount[0], 0.0f, 1.0f, "%.2f")) m_posesDirty = true;
    poseCombo("Grab uses pose", m_stylePose[1]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("amount##grab", &m_stylePoseAmount[1], 0.0f, 1.0f, "%.2f")) m_posesDirty = true;
    poseCombo("Punch uses pose", m_stylePose[3]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("amount##punch", &m_stylePoseAmount[3], 0.0f, 1.0f, "%.2f")) m_posesDirty = true;
    {
        const char* current = m_stylePose[2].empty() ? "(same as the press pose)" : m_stylePose[2].c_str();
        if (ImGui::BeginCombo("Resting hand uses pose", current))
        {
            if (ImGui::Selectable("(same as the press pose)", m_stylePose[2].empty())) { m_stylePose[2].clear(); m_posesDirty = true; }
            for (const HandPose& p : m_poses)
                if (ImGui::Selectable(p.name.c_str(), m_stylePose[2] == p.name)) { m_stylePose[2] = p.name; m_posesDirty = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The hand held up between clicks / while hovering. A press blends from this pose into the press pose and back.\nTo make one: pick the point pose under 'Edit pose', press Duplicate, then shape the copy.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderFloat("amount##rest", &m_stylePoseAmount[2], 0.0f, 1.0f, "%.2f")) m_posesDirty = true;
    }

    ImGui::Separator();
    // --- pose being edited ------------------------------------------------------------------------------
    if (m_poses.empty())
        SeedDefaultPoses(m_poses, nullptr);
    m_editPose = clamp_tpl(m_editPose, 0, (int)m_poses.size() - 1);
    HandPose& P = m_poses[(size_t)m_editPose];
    if (ImGui::BeginCombo("Edit pose", P.name.c_str()))
    {
        for (int i = 0; i < (int)m_poses.size(); i++)
            if (ImGui::Selectable(m_poses[(size_t)i].name.c_str(), i == m_editPose)) m_editPose = i;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##newpose", m_newPoseName, sizeof(m_newPoseName));
    ImGui::SameLine();
    if (ImGui::Button("Add") && m_newPoseName[0] && !FindPose(m_newPoseName))
    {
        HandPose p; p.name = m_newPoseName; m_poses.push_back(p); m_editPose = (int)m_poses.size() - 1; m_newPoseName[0] = 0; m_posesDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate"))
    {
        std::string nm = m_newPoseName[0] ? std::string(m_newPoseName) : P.name + " copy";
        for (int k = 2; FindPose(nm.c_str()) && k < 100; k++) nm = P.name + " copy " + std::to_string(k);
        if (!FindPose(nm.c_str()))
        {
            HandPose p = P; p.name = nm; m_poses.push_back(p); m_editPose = (int)m_poses.size() - 1; m_newPoseName[0] = 0; m_posesDirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && m_poses.size() > 1)
    {
        for (std::string& sp : m_stylePose) if (sp == P.name) sp.clear();
        m_poses.erase(m_poses.begin() + m_editPose);
        m_editPose = clamp_tpl(m_editPose, 0, (int)m_poses.size() - 1);
        m_posesDirty = true;
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type a name in the box to Add an empty pose; Duplicate copies the edited one (under that name, or '<name> copy').");

    // --- posing mode ------------------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Posing mode (hold the edited pose so you can shape it live)");
    ImGui::RadioButton("Off", &I.holdMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Hold the pose on the animated hand", &I.holdMode, 1); ImGui::SameLine();
    ImGui::RadioButton("Hold the pose + reach to the test point", &I.holdMode, 2);
    if (I.holdMode != 0 && m_currentWeaponClass.empty() && !s.interactUnarmed)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Needs a weapon out.");
    ImGui::TextDisabled("With every slider at 0 the hand is the skeleton's bind pose, wrist level with the camera: the same for every weapon.\n"
                        "The reach here goes to the test point (Test section) with the assigned style's offsets - identical to a real interaction with\n"
                        "'Hand goes to' = fixed point, or with the pose's Absolute position on.");

    // --- wrist --------------------------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Wrist");
    if (CheckboxInt("Absolute position (ignore the object, the hand always goes here)", P.absolutePos,
        "Off: the sliders below are an offset from the object point the hand reaches for (fingertip on the button, not the wrist).\n"
        "On: they are the hand's position in view space, the same for every interaction and every weapon."))
        m_posesDirty = true;
    const float range = P.absolutePos ? 100.0f : 30.0f;
    if (SliderCm(P.absolutePos ? "Position right / left" : "Offset right / left", P.posX, range, "View space, relative to the camera.")) m_posesDirty = true;
    if (SliderCm(P.absolutePos ? "Position forward" : "Offset forward / back", P.posY, range, P.absolutePos ? "Distance in front of the camera." : "Negative pulls the wrist back from the target.")) m_posesDirty = true;
    if (SliderCm(P.absolutePos ? "Position up / down" : "Offset up / down", P.posZ, range, nullptr)) m_posesDirty = true;
    ImGui::TextDisabled("The position is enforced on the hand joint itself (closed loop on last frame's pose), so it is the same whatever the weapon.");

    // --- per-weapon correction ----------------------------------------------------------------------------
    ImGui::Spacing();
    {
        ImGui::Text("Per-weapon correction: %s", m_currentWeaponClass.empty() ? "(no weapon)" : m_currentWeaponClass.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Added on top of every pose while this weapon is held (position in view space, wrist rotation), blended in with the reach.\n"
                              "Line the hand up with one weapon through the pose, then fix the others here. Saved with the weapon settings.");
        WeaponSettings& w = GetCurrentWeapon();
        bool ch = false;
        ImGui::PushID("interact_weapon");
        ch |= SliderCm("Hand right / left##w", w.interact.posX, 20.0f, "This weapon only: moves the hand's end position right (+) / left (-).");
        ch |= SliderCm("Hand forward / back##w", w.interact.posY, 20.0f, "This weapon only: moves the hand's end position forward (+) / back (-).");
        ch |= SliderCm("Hand up / down##w", w.interact.posZ, 20.0f, "This weapon only: moves the hand's end position up (+) / down (-).");
        ch |= SliderDeg("Wrist pitch##w", w.interact.pitch, 60.0f, "This weapon only: extra wrist pitch.");
        ch |= SliderDeg("Wrist yaw##w", w.interact.yaw, 60.0f, "This weapon only: extra wrist yaw.");
        ch |= SliderDeg("Wrist roll##w", w.interact.roll, 60.0f, "This weapon only: extra wrist roll.");
        ImGui::TextDisabled("Forearm (turns under the wrist; the hand keeps its orientation)");
        ch |= SliderDeg("Forearm pitch##w", w.interactForearm.pitch, 90.0f, "This weapon only: rotates the forearm about its own axes while the hand is posed / resting. The wrist is pushed absolutely, so it stays put - use this when the wrist looks over-twisted against the forearm.");
        ch |= SliderDeg("Forearm yaw##w", w.interactForearm.yaw, 90.0f, nullptr);
        ch |= SliderDeg("Forearm roll (twist)##w", w.interactForearm.roll, 90.0f, nullptr);
        if (ImGui::Button("Reset this weapon's correction")) { w.interact.Reset(); w.interactForearm.Reset(); ch = true; }
        ImGui::PopID();
        if (ch)
        {
            w.valid = true;
            m_weaponsDirty = true;
        }
    }
    if (SliderDeg("Hand pitch", P.handPitch, 180.0f, "Absolute, view space: + tilts the hand up (fingers up).")) m_posesDirty = true;
    if (SliderDeg("Hand yaw", P.handYaw, 180.0f, "Absolute, view space: + turns the hand left.")) m_posesDirty = true;
    if (SliderDeg("Hand roll", P.handRoll, 180.0f, "Absolute, view space: rolls around the forward axis.")) m_posesDirty = true;

    // --- fingers ------------------------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Joints under the hand (%d)", (int)max((int)R.leftSubtree.size() - 1, 0));
    ImGui::SameLine();
    if (ImGui::Button("Reset all joints"))
    {
        for (HandPoseJoint& j : P.joints) j.pitch = j.yaw = j.roll = 0.0f;
        m_posesDirty = true;
    }
    if (R.leftSubtree.empty())
        ImGui::TextDisabled("The hand skeleton has not been seen yet (wait a moment with a weapon out).");
    ImGui::BeginChild("pose_joints", ImVec2(0, 340), true);
    for (size_t i = 0; i < R.leftSubtree.size(); i++)
    {
        const int id = R.leftSubtree[i];
        if (id == R.leftHand) continue;
        const std::string& name = R.leftSubtreeNames[i];
        HandPoseJoint* pJ = P.Find(name.c_str());
        ImGui::PushID(id);
        ImGui::Text("%s%s", name.c_str(), (pJ && pJ->Adjusted()) ? " *" : "");
        // Sliders edit a joint entry, created on first touch.
        float pitch = pJ ? pJ->pitch : 0.0f, yaw = pJ ? pJ->yaw : 0.0f, roll = pJ ? pJ->roll : 0.0f;
        bool ch = false;
        ImGui::SetNextItemWidth(130); ch |= ImGui::SliderFloat("P", &pitch, -150.0f, 150.0f, "%.0f"); ImGui::SameLine();
        ImGui::SetNextItemWidth(130); ch |= ImGui::SliderFloat("Y", &yaw, -150.0f, 150.0f, "%.0f"); ImGui::SameLine();
        ImGui::SetNextItemWidth(130); ch |= ImGui::SliderFloat("R", &roll, -150.0f, 150.0f, "%.0f"); ImGui::SameLine();
        if (ImGui::SmallButton("0")) { pitch = yaw = roll = 0.0f; ch = true; }
        if (ch)
        {
            if (!pJ)
            {
                P.joints.push_back(HandPoseJoint());
                pJ = &P.joints.back();
                pJ->name = name;
            }
            pJ->pitch = pitch; pJ->yaw = yaw; pJ->roll = roll;
            m_posesDirty = true;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("P / Y / R: rotation on the bind pose, in the joint's parent frame (which axis curls a finger depends on the rig - try each). Ctrl+click to type.");
    if (ImGui::Button("Save poses now")) SavePoses();
    ImGui::SameLine();
    ImGui::TextDisabled(m_posesDirty ? "(unsaved changes - saved automatically in a moment)" : "(saved)");
    if (s.showAdvanced)
        ImGui::TextDisabled("pose this frame: %s, weight %.2f, %d joint overrides pushed, bind pose %s (slots %d/%d)", I.poseName.empty() ? "-" : I.poseName.c_str(),
            I.poseWeight, I.poseJointsPushed, R.defaultPoseValid ? "ok" : "missing", R.defaultAbsSlot, R.defaultRelSlot);
}

void ModMain::DrawInteractTab()
{
    ViewmodelSettings& s = m_settings;
    InteractState& I = m_interact;
    const char* typeNames[] = { "scriptDefined (buttons, doors, terminals, ...)", "codeDefined", "pickup", "consume", "carry", "hack", "repair", "fortify", "examine", "equip", "hoover (GLOO/Recycler charges)" };
    const int typeBits[] = { 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    auto typeMaskEditor = [&](int& mask, const char* id) {
        ImGui::PushID(id);
        for (int i = 0; i < 11; i++)
        {
            bool on = (mask >> typeBits[i]) & 1;
            if (ImGui::Checkbox(typeNames[i], &on))
                mask = on ? (mask | (1 << typeBits[i])) : (mask & ~(1 << typeBits[i]));
        }
        ImGui::PopID();
    };

    ImGui::TextWrapped("The support hand reaches out when you interact: a press for buttons, switches, terminals and hacking, a grab for "
                       "pickups, loot, consumables and things you carry - with any weapon, with none, and on in-world screens and keypads.");
    ImGui::Spacing();
    CheckboxInt("Enable interaction animation", s.interactEnabled);
    ImGui::SameLine();
    CheckboxInt("Also while aiming", s.interactWhileAiming, "By default the sights win: no reach while aiming down sights.");
    ImGui::SameLine();
    CheckboxInt("With no weapon out", s.interactUnarmed, "The game hides the arms with no weapon out; they are shown for the reach.");

    // --- the reach itself -----------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Reach", ImGuiTreeNodeFlags_DefaultOpen))
    {
        CheckboxInt("Defer the interaction until the hand arrives", s.interactDefer,
            "The game's side of the interaction (item vanishes, button clicks, door opens) is delayed so it happens when the hand gets there.\n"
            "The key press is answered immediately; only the effect waits. Carrying is never deferred: the game delays that itself.");
        if (s.interactDefer)
        {
            ImGui::Indent();
            ImGui::SliderFloat("Interaction fires after", &s.interactFireDelay, 0.0f, 0.6f, "%.2f s");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seconds from the key press to the actual interaction. Roughly the reach time of the style, or a bit less.");
            CheckboxInt("Cancel if the crosshair moved to another object", s.interactCancelRetarget,
                "Off: the interaction happens on whatever is under the crosshair when the delay ends (never loses an input).\n"
                "On: it is dropped when the target changed in between.");
            CheckboxInt("Grabs fire at the apex of the reach instead", s.interactGrabAtApex,
                "Pickups, loot and the like happen when the hand is closest to the object (the grab's reach time), not after the fixed delay above.");
            ImGui::Unindent();
        }
        CheckboxInt("Pick physics objects up when the hand gets there", s.interactCarryAtApex,
            "The grab starts once the key has been held (the game's own hold-to-lift time for heavy objects, or the time below), and the object leaves the "
            "ground at the apex of the grab. A tap does nothing, and letting go early calls the grab off.");
        if (s.interactCarryAtApex)
        {
            ImGui::Indent();
            ImGui::SliderFloat("Hold the key at least", &s.interactCarryHoldTime, 0.0f, 0.6f, "%.2f s");
            ImGui::TextDisabled("carries: %d called off by letting go%s", I.carryCancelled, I.carryPending ? " | waiting for the hold" : (I.carryAnimating ? " | grabbing" : ""));
            ImGui::Unindent();
        }
        const char* targetModes[] = { "Crosshair hit point on the object", "Object centre", "Fixed point ahead (test point)" };
        ImGui::Combo("Hand goes to", &s.interactTargetMode, targetModes, 3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the hand reaches. The hit point is where the crosshair ray meets the object (a button's face); falls back to the object centre.");
        const char* eases[] = { "Linear", "Smooth", "Ease out (fast start)", "Ease in (slow start)", "Ease in-out" };
        ImGui::Combo("Reach easing", &s.interactEaseIn, eases, 5);
        ImGui::Combo("Return easing", &s.interactEaseOut, eases, 5);
        if (ImGui::TreeNode("Press timing and path (buttons, switches, terminals, hack, repair)"))
        {
            DrawReachStyle(s.press, "press");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Grab timing and path (pickups, loot, consume, carry, equip, examine)"))
        {
            DrawReachStyle(s.grab, "grab");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Punch timing and path (quick melee - test button below for now)"))
        {
            ImGui::TextWrapped("Goes to the fixed test point ahead unless its pose has an absolute position (the default punch pose does: 75 cm ahead).");
            DrawReachStyle(s.punch, "punch");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("How far the hand may go (envelope)"))
        {
            ImGui::TextWrapped("The wrist is kept inside this box around the eye (view space); a farther target is approached along the line of sight.");
            SliderCm("Furthest forward", s.interactMaxForward, 100.0f, nullptr);
            SliderCm("Furthest forward on screens", s.interactExamMaxForward, 100.0f, "While a screen / keypad is up the arms are brought to the camera, so the arm has its full length.");
            SliderCm("Nearest", s.interactMinForward, 50.0f, "The hand never comes closer to the camera than this.");
            SliderCm("Left / right", s.interactMaxSide, 80.0f, "Sideways limit either way.");
            SliderCm("Above the eye", s.interactMaxUp, 80.0f, nullptr);
            SliderCm("Below the eye", s.interactMaxDown, 100.0f, nullptr);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Which interactions animate, and how"))
        {
            ImGui::TextWrapped("Built in: pickups, consumables, carrying, equipping, examining and the loot action grab; everything else presses. "
                               "The rules below override that by what the thing is (first match wins). 'Add from the last interaction' makes a rule "
                               "for whatever you just used - then pick press / grab / none for it.");
            ImGui::TextDisabled("Last interaction: %s / %s on %s", InteractionTypeName(I.lastType), InteractionModeName(I.lastMode), I.lastEntity.empty() ? "-" : I.lastEntity.c_str());
            if (ImGui::Button("Add a rule from the last interaction") && I.lastType >= 0)
            {
                InteractRule r;
                r.type = -1;
                r.mode = I.lastMode;
                r.classContains = I.lastClass;
                r.textContains = I.lastText;
                r.style = 0;
                r.note = "";
                m_rules.insert(m_rules.begin(), r);
                m_editRule = 0;
                m_posesDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset the rules to the defaults")) { m_rules.clear(); SeedDefaultRules(); m_posesDirty = true; }
            const char* styleNames[] = { "press", "grab", "none" };
            const char* modeNames[] = { "any mode", "use", "hold use", "loot", "special" };
            for (int i = 0; i < (int)m_rules.size(); i++)
            {
                InteractRule& r = m_rules[(size_t)i];
                ImGui::PushID(i);
                char label[200];
                snprintf(label, sizeof(label), "%d. %s -> %s%s%s", i + 1,
                    r.note.empty() ? (r.classContains.empty() && r.textContains.empty() ? "(any)" : (r.classContains + (r.textContains.empty() ? "" : " \"" + r.textContains + "\"")).c_str()) : r.note.c_str(),
                    styleNames[clamp_tpl(r.style, 0, 2)], r.hover == 0 && r.style != 2 ? " (no hover)" : "", r.mode >= 0 ? (std::string(" [") + modeNames[clamp_tpl(r.mode + 1, 0, 4)] + "]").c_str() : "");
                if (ImGui::TreeNode("rule", "%s", label))
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "%s", r.note.c_str()); ImGui::SetNextItemWidth(260);
                    if (ImGui::InputText("Note", buf, sizeof(buf))) { r.note = buf; m_posesDirty = true; }
                    snprintf(buf, sizeof(buf), "%s", r.classContains.c_str()); ImGui::SetNextItemWidth(260);
                    if (ImGui::InputText("Entity class contains", buf, sizeof(buf))) { r.classContains = buf; m_posesDirty = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Case-insensitive substring of the entity class name (e.g. Container, ArkHuman, ArkHarvestable, ArkWeapon). Empty = any.");
                    snprintf(buf, sizeof(buf), "%s", r.textContains.c_str()); ImGui::SetNextItemWidth(260);
                    if (ImGui::InputText("Prompt text contains", buf, sizeof(buf))) { r.textContains = buf; m_posesDirty = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Case-insensitive substring of the interaction's prompt text (e.g. talk). Empty = any.");
                    int modeSel = clamp_tpl(r.mode + 1, 0, 4);
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::Combo("Mode", &modeSel, modeNames, 5)) { r.mode = modeSel - 1; m_posesDirty = true; }
                    int typeSel = r.type < 0 ? 0 : r.type;
                    const char* typeSelNames[] = { "any type", "none", "scriptDefined", "unavailable", "codeDefined", "pickup", "consume", "carry", "hack", "repair", "fortify", "examine", "equip", "hoover" };
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::Combo("Type", &typeSel, typeSelNames, 14)) { r.type = typeSel == 0 ? -1 : typeSel; m_posesDirty = true; }
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::Combo("Animation", &r.style, styleNames, 3)) m_posesDirty = true;
                    if (r.style != 2)
                    {
                        bool hv = r.hover != 0;
                        if (ImGui::Checkbox("Hovering hand comes up for it", &hv)) { r.hover = hv ? 1 : 0; m_posesDirty = true; }
                    }
                    if (ImGui::Button("Up") && i > 0) { std::swap(m_rules[(size_t)i], m_rules[(size_t)i - 1]); m_posesDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::Button("Down") && i + 1 < (int)m_rules.size()) { std::swap(m_rules[(size_t)i], m_rules[(size_t)i + 1]); m_posesDirty = true; }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) { m_rules.erase(m_rules.begin() + i); m_posesDirty = true; ImGui::TreePop(); ImGui::PopID(); break; }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::Text("Interaction types that animate at all");
            typeMaskEditor(s.interactTypeMask, "types");
            CheckboxInt("Remote manipulation (psi) mode", s.interactRemoteMode);
            ImGui::TreePop();
        }
    }

    // --- quick melee ---------------------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Quick melee", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("A punch on its own key: the punch animation plays and, at its apex, the wrench's own hit lands (its range, force, reactions), scaled. "
                           "Needs a wrench in the inventory - equipped or not.");
        CheckboxInt("Enable quick melee", s.meleeEnabled);
        ImGui::Text("Key: %s", GetKeyName(s.meleeKey));
        ImGui::SameLine();
        if (!m_waitingForMeleeKey)
        {
            if (ImGui::SmallButton("Bind##melee")) m_waitingForMeleeKey = true;
        }
        else
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "press the key (Esc cancels)");
        ImGui::SameLine();
        CheckboxInt("swallow the key", s.meleeConsumeKey, "The game does not see the key press.");
        ImGui::SliderFloat("Damage (x wrench hit)", &s.meleeDamage, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Cooldown", &s.meleeCooldown, 0.0f, 3.0f, "%.2f s");
        CheckboxInt("Also while aiming down sights", s.meleeWhileAiming);
        CheckboxInt("Swing sound", s.meleeSound, "Plays the audio trigger named by vm_melee_sound_name when the punch starts (default Play_Player_Throw, the throw whoosh; set another wwise event name in the console).");
        CheckboxInt("Flip the hit's physics impulse", s.meleeImpulseFlip, "The wrench hit's impulse pulled objects toward the player when called from the punch; on, its direction is reversed. Log line with the raw direction when the debug overlay is on.");
        ImGui::SliderFloat("Impulse scale", &s.meleeImpulseScale, 0.0f, 3.0f, "x %.2f");
        if (ImGui::TreeNodeEx("Equipped weapon lowered while punching", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Added to the hip pose from the windup, taken back from the return.");
            SliderCm("Right / left##ml", s.meleeLower.posX, 30.0f, nullptr);
            SliderCm("Forward / back##ml", s.meleeLower.posY, 30.0f, nullptr);
            SliderCm("Up / down##ml", s.meleeLower.posZ, 30.0f, "Negative lowers the weapon.");
            SliderDeg("Pitch##ml", s.meleeLower.pitch, 60.0f, "Negative tips the muzzle down.");
            SliderDeg("Yaw##ml", s.meleeLower.yaw, 60.0f, nullptr);
            SliderDeg("Roll##ml", s.meleeLower.roll, 60.0f, nullptr);
            ImGui::SliderFloat("Blend time##ml", &s.meleeLowerTime, 0.02f, 0.5f, "%.2f s");
            const char* lowerEases[] = { "Linear", "Smooth", "Ease out (fast start)", "Ease in (slow start)", "Ease in-out" };
            ImGui::Combo("Easing##ml", &s.meleeLowerEase, lowerEases, 5);
            ImGui::TextDisabled("now %.2f", I.meleeLowerBlend);
            ImGui::TreePop();
        }
        ImGui::Text("Camera kick");
        ImGui::SliderFloat("Pitch##mk", &s.meleeCamKick, -5.0f, 5.0f, "%.1f deg");
        ImGui::SliderFloat("Yaw##mk", &s.meleeCamKickYaw, -5.0f, 5.0f, "%.1f deg");
        ImGui::SliderFloat("Time##mk", &s.meleeCamKickTime, 0.05f, 1.0f, "%.2f s");
        ImGui::TextDisabled("punches %d, hits %d, without a wrench %d%s | timing and path: Reach -> Punch; pose: Hand pose -> Punch uses pose",
            I.meleePunches, I.meleeHits, I.meleeNoWrench, I.meleeCooldownLeft > 0.0f ? " | cooling down" : "");
    }

    // --- screens and keypads ----------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Screens and keypads", ImGuiTreeNodeFlags_DefaultOpen))
    {
        CheckboxInt("Press where you click on a screen / keypad (zoomed-in view)", s.interactExamination,
            "While a screen is up the arms are brought to the zoomed-in camera and the hand presses where the centre of the view is when you click.");
        if (s.interactExamination)
        {
            ImGui::Indent();
            ImGui::Text("Click keys: %s and %s", GetKeyName(s.interactExamKey), GetKeyName(s.interactExamKey2));
            ImGui::SameLine();
            if (m_waitingForExamKey == 0)
            {
                if (ImGui::SmallButton("Bind 1st")) m_waitingForExamKey = 1;
                ImGui::SameLine();
                if (ImGui::SmallButton("Bind 2nd")) m_waitingForExamKey = 2;
            }
            else
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "press the key to use as a screen click (Esc cancels)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set these to the mouse button / key you click screens with (your use key, e.g. F).");
            CheckboxInt("Gentler press on screens", s.interactExamGentle,
                "The press while a screen is up uses its own timings (slower in and out, shorter way): you are close to it and the hand starts from its resting spot.");
            if (s.interactExamGentle && ImGui::TreeNode("Press on screens: timing and path"))
            {
                DrawReachStyle(s.pressExam, "pressExam");
                ImGui::TreePop();
            }
            ImGui::SliderFloat("Leaving a screen: fade-out time", &s.interactExamLeaveTime, 0.05f, 1.5f, "%.2f s");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything that only applies on screens (the body brought to the camera, the screen corrections, the longer reach) fades over this after you leave one, instead of snapping to the weapon.");
            const char* fovModes[] = { "The game's zoom", "No zoom (regular FOV)", "Custom" };
            ImGui::Combo("Camera FOV on screens", &s.interactExamFovMode, fovModes, 3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overrides the zoom the game applies while a screen / keypad is up. The camera still moves to the screen, and the screen itself keeps its size (it is drawn with the arms).");
            if (s.interactExamFovMode == 2)
                ImGui::SliderFloat("Screen FOV", &s.interactExamFov, 30.0f, 120.0f, "%.0f deg");
            if (ImGui::TreeNode("Fitting the arm to the screen (advanced)"))
            {
                ImGui::TextWrapped("Only while a screen is up. Turn the debug overlay on (Advanced) to see the target, the asked and the actual hand and the shoulder.");
                SliderCm("No reach when the point is closer than", s.interactExamMinTargetDist, 100.0f,
                    "Keypads put the camera 20-30 cm from the surface; a hand touching them would sit on the lens. Closer than this: the hand stays put.");
                CheckboxInt("Bring the body forward automatically when the wrist is beyond the arm", s.interactExamAutoBody,
                    "Slides the body toward the screen until shoulder-to-wrist equals the arm length below (monitors are farther than the arm is long).");
                if (s.interactExamAutoBody)
                {
                    SliderCm("Arm length (shoulder to wrist)", s.interactExamArmLength, 80.0f, nullptr);
                    ImGui::TextDisabled("automatic body forward right now: %.0f cm", I.autoBodyY * 100.0f);
                }
                ImGui::Text("Body offset (the whole arms / torso relative to the camera)");
                SliderCm("Body right / left##bd", s.interactExamBodyX, 100.0f, nullptr);
                SliderCm("Body forward / back##bd", s.interactExamBodyY, 100.0f, "Positive brings the shoulders closer to the screen.");
                SliderCm("Body up / down##bd", s.interactExamBodyZ, 100.0f, nullptr);
                ImGui::Text("Hand correction on screens (on top of the pose and the per-weapon correction)");
                SliderCm("Hand right / left##ex", s.examCorr.posX, 100.0f, nullptr);
                SliderCm("Hand forward / back##ex", s.examCorr.posY, 100.0f, "Positive = further from the camera.");
                SliderCm("Hand up / down##ex", s.examCorr.posZ, 100.0f, nullptr);
                SliderDeg("Wrist pitch##ex", s.examCorr.pitch, 180.0f, nullptr);
                SliderDeg("Wrist yaw##ex", s.examCorr.yaw, 180.0f, nullptr);
                SliderDeg("Wrist roll##ex", s.examCorr.roll, 180.0f, nullptr);
                if (ImGui::Button("Reset all of the above")) { s.examCorr.Reset(); s.interactExamBodyX = s.interactExamBodyY = s.interactExamBodyZ = 0.0f; I.autoBodyY = 0.0f; }
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }
        ImGui::Spacing();
        ImGui::Text("Use from where you stand (no automatic zoom-in)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The game zooms you into some screen types as soon as you press use on them. Off, the press goes straight to the button under the\n"
                              "crosshair - like kiosks and keycard readers already work - and the hand presses it from where you stand. The examine key still zooms in.");
        CheckboxInt("Keypads", s.interactNoZoomKeypad);
        ImGui::SameLine(); CheckboxInt("Fabricators", s.interactNoZoomFabricator);
        ImGui::SameLine(); CheckboxInt("Security stations", s.interactNoZoomSecurity);
        ImGui::SameLine(); CheckboxInt("Workstations", s.interactNoZoomWorkstation);
    }

    // --- resting hand -------------------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Resting hand", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("The pointing hand held up in view: between clicks on a screen, and - optionally - whenever something usable is in front of you. "
                           "Its pose is chosen under 'Hand pose' (\"Resting hand uses pose\").");
        CheckboxInt("On screens (between clicks)", s.interactExamRest,
            "While a screen / keypad is up the arms stay shown and the hand waits at the resting spot; each click reaches from there and back.");
        CheckboxInt("Hovering over usable things (outside screens)", s.interactHoverRest,
            "While the game shows its use prompt for something within the distance below - a button, a keypad, an item, a container - the hand comes up and waits, then presses / grabs from there.");
        if (s.interactHoverRest)
        {
            ImGui::Indent();
            CheckboxInt("with a weapon out (the support hand leaves the grip)", s.interactHoverWeapon);
            if (s.interactHoverWeapon)
            {
                ImGui::SameLine();
                CheckboxInt("also while aiming", s.interactHoverWhileAiming, "Off: while aiming down sights the support hand stays on the gun.");
            }
            CheckboxInt("with no weapon out", s.interactHoverUnarmed);
            SliderCm("Only when closer than", s.interactHoverMaxDist, 300.0f, "Camera to the thing's point.");
            ImGui::SliderFloat("Hover towards the thing", &s.interactHoverTowards, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = the hand waits at the plain resting spot, 1 = it hovers right at the thing (fingertip on it).");
            if (ImGui::TreeNode("Which things"))
            {
                typeMaskEditor(s.interactHoverTypeMask, "hovertypes");
                ImGui::TreePop();
            }
            ImGui::TextDisabled("now: %s%s (type %s, %.2f m)", I.hoverActive ? "hovering" : "not hovering", I.hoverType >= 0 && !I.hoverActive ? " - usable thing seen but not taken" : "",
                InteractionTypeName(I.hoverType), I.hoverDist);
            ImGui::Unindent();
        }
        ImGui::SliderFloat("Settle time", &s.interactRestBlendTime, 0.05f, 1.5f, "%.2f s");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seconds for the hand to come up to the spot and to go back.");
        CheckboxInt("Hand comes up from below the view when it is not on the weapon", s.interactHiddenStart,
            "One-handed weapons and no weapon: the animated support hand is somewhere off screen, and a blend from there crosses into view in a couple of frames - the 'pop'. "
            "On, the hand rises from the spot below instead (and the arm's IK is on from the first frame).");
        if (s.interactHiddenStart && ImGui::TreeNode("Where it comes up from"))
        {
            SliderCm("Right / left##hs", s.interactStartX, 60.0f, "View space; keep it below the view's bottom edge.");
            SliderCm("Forward##hs", s.interactStartY, 80.0f, nullptr);
            SliderCm("Up / down##hs", s.interactStartZ, 100.0f, nullptr);
            WeaponSettings& w = GetCurrentWeapon();
            bool ch = false;
            ImGui::PushID("interact_start_weapon");
            ImGui::TextDisabled("or, for %s only (all zero = use the spot above):", m_currentWeaponClass.empty() ? "no weapon" : m_currentWeaponClass.c_str());
            ch |= SliderCm("Right / left##sw", w.interactStart.posX, 60.0f, nullptr);
            ch |= SliderCm("Forward##sw", w.interactStart.posY, 80.0f, nullptr);
            ch |= SliderCm("Up / down##sw", w.interactStart.posZ, 100.0f, nullptr);
            if (ImGui::Button("Reset this weapon's spot##sw")) { w.interactStart.Reset(); ch = true; }
            ImGui::TextDisabled("Arm while the hand is up (the animated arm hangs elsewhere - move the shoulder to where it would be)");
            SliderCm("Shoulder right / left##hsh", s.interactStartShoulder.posX, 40.0f, "The upper-arm joint moved (view space) while the hand is up, hand off the weapon only. The arm is re-solved from there.");
            SliderCm("Shoulder forward / back##hsh", s.interactStartShoulder.posY, 40.0f, nullptr);
            SliderCm("Shoulder up / down##hsh", s.interactStartShoulder.posZ, 40.0f, nullptr);
            SliderCm("Elbow right / left##hel", s.interactStartElbow.posX, 40.0f, "The forearm joint moved; the arm IK may re-solve it.");
            SliderCm("Elbow forward / back##hel", s.interactStartElbow.posY, 40.0f, nullptr);
            SliderCm("Elbow up / down##hel", s.interactStartElbow.posZ, 40.0f, nullptr);
            ImGui::TextDisabled("plus, for %s only:", m_currentWeaponClass.empty() ? "no weapon" : m_currentWeaponClass.c_str());
            ch |= SliderCm("Shoulder right / left##wsh2", w.interactStartShoulder.posX, 40.0f, nullptr);
            ch |= SliderCm("Shoulder forward / back##wsh2", w.interactStartShoulder.posY, 40.0f, nullptr);
            ch |= SliderCm("Shoulder up / down##wsh2", w.interactStartShoulder.posZ, 40.0f, nullptr);
            ch |= SliderCm("Elbow right / left##wel2", w.interactStartElbow.posX, 40.0f, nullptr);
            ch |= SliderCm("Elbow forward / back##wel2", w.interactStartElbow.posY, 40.0f, nullptr);
            ch |= SliderCm("Elbow up / down##wel2", w.interactStartElbow.posZ, 40.0f, nullptr);
            if (ImGui::Button("Reset this weapon's arm##sw2")) { w.interactStartShoulder.Reset(); w.interactStartElbow.Reset(); ch = true; }
            const char* handOff[] = { "on the weapon (blend from the animated hand)", "off the weapon (come up from the spot)", "auto (IK weight / where the animated hand is)" };
            ImGui::SetNextItemWidth(320);
            if (ImGui::Combo("Support hand for this weapon", &w.interactHandOff, handOff, 3)) ch = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("One-handed weapons (wrench, grenades) animate the support hand out of view: the hand should come up from the spot. Built-in per weapon; auto looks at the animated IK weight and hand position.");
            ImGui::PopID();
            if (ch) { w.valid = true; m_weaponsDirty = true; }
            ImGui::TextDisabled("now: %s | animated hand in view space (%.2f %.2f %.2f), IK weight %.2f%s", I.hiddenStartUsed ? "coming up from the spot" : "from the animated hand",
                I.handView.x, I.handView.y, I.handView.z, I.animIkWeight, I.animIkWeightValid ? "" : " (not captured)");
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Resting spot outside screens", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SliderCm("Right / left##rn", s.interactRestX, 50.0f, "View space, relative to the camera.");
            SliderCm("Forward##rn", s.interactRestY, 80.0f, nullptr);
            SliderCm("Up / down##rn", s.interactRestZ, 50.0f, nullptr);
            WeaponSettings& w = GetCurrentWeapon();
            bool ch = false;
            ImGui::PushID("interact_rest_weapon");
            ImGui::TextDisabled("plus, for %s only:", m_currentWeaponClass.empty() ? "no weapon" : m_currentWeaponClass.c_str());
            ch |= SliderCm("Right / left##rw", w.interactRest.posX, 30.0f, "This weapon only: where its resting hand waits, relative to the spot above (e.g. clear of a one-handed weapon). Saved with the weapon.");
            ch |= SliderCm("Forward##rw", w.interactRest.posY, 30.0f, nullptr);
            ch |= SliderCm("Up / down##rw", w.interactRest.posZ, 30.0f, nullptr);
            if (ImGui::Button("Reset this weapon's spot")) { w.interactRest.Reset(); ch = true; }
            ImGui::PopID();
            if (ch) { w.valid = true; m_weaponsDirty = true; }
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Resting spot on screens", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SliderCm("Right / left##re", s.interactRestExamX, 50.0f, "View space, relative to the zoomed-in camera.");
            SliderCm("Forward##re", s.interactRestExamY, 80.0f, nullptr);
            SliderCm("Up / down##re", s.interactRestExamZ, 50.0f, nullptr);
            if (ImGui::Button("Copy the outside spot here")) { s.interactRestExamX = s.interactRestX; s.interactRestExamY = s.interactRestY; s.interactRestExamZ = s.interactRestZ; }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Drift (slow figure-eight while resting; fades out during a press)"))
        {
            SliderCm("Drift amount", s.interactRestSwayPos, 5.0f, "Side to side; the vertical part is 60 %% of it, forward 30 %%.");
            ImGui::SliderFloat("Drift rotation", &s.interactRestSwayRot, 0.0f, 10.0f, "%.1f deg");
            ImGui::SliderFloat("Drift speed", &s.interactRestSwayFreq, 0.05f, 1.5f, "%.2f Hz");
            ImGui::TreePop();
        }
    }

    // --- hand pose --------------------------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Hand pose (wrist and fingers)", ImGuiTreeNodeFlags_DefaultOpen))
        DrawHandPoseEditor();

    // --- test --------------------------------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Test the animation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Plays the animation without interacting with anything. 'Ahead' uses the fixed test point below; 'on the crosshair' uses whatever the crosshair ray hits (walls included).");
        const bool canTest = Active() && (!m_currentWeaponClass.empty() || s.interactUnarmed);
        if (!canTest)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Needs the mod enabled and a weapon out (or 'With no weapon out').");
        auto crosshairPoint = [&](Vec3& out) -> bool {
            if (!gEnv || !gEnv->pSystem || !gEnv->pPhysicalWorld) return false;
            ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
            IPhysicalEntity* pSkip = (pPlayer && pPlayer->GetEntity()) ? pPlayer->GetEntity()->GetPhysics() : nullptr;
            const Matrix34 cam = gEnv->pSystem->GetViewCamera().GetMatrix();
            PaddedRayHit hit;
            const int n = gEnv->pPhysicalWorld->RayWorldIntersection(cam.GetTranslation(), cam.GetColumn1().GetNormalized() * 3.0f, ent_all,
                rwi_stop_at_pierceable | rwi_colltype_any(geom_colltype_ray | geom_colltype0 | geom_colltype_player), &hit, 1, pSkip);
            if (n > 0 && Finite(hit.pt)) { out = hit.pt; return true; }
            return false;
        };
        if (ImGui::Button("Press ahead") && canTest) StartReach(0, nullptr);
        ImGui::SameLine();
        if (ImGui::Button("Grab ahead") && canTest) StartReach(1, nullptr);
        ImGui::SameLine();
        if (ImGui::Button("Punch") && canTest) StartReach(2, nullptr);
        ImGui::SameLine();
        if (ImGui::Button("Press on the crosshair") && canTest) { Vec3 p; if (crosshairPoint(p)) StartReach(0, &p); }
        ImGui::SameLine();
        if (ImGui::Button("Grab on the crosshair") && canTest) { Vec3 p; if (crosshairPoint(p)) StartReach(1, &p); }
        SliderCm("Test point: right / left", s.interactTestX, 50.0f, "Fixed test point, view space.");
        SliderCm("Test point: forward", s.interactTestY, 100.0f, nullptr);
        SliderCm("Test point: up / down", s.interactTestZ, 50.0f, nullptr);
        const char* phaseNames[] = { "idle", "reach", "hold", "return", "windup" };
        ImGui::Text("%s  t=%.2f s  progress %.2f  style %s", phaseNames[(int)I.phase], I.time, I.curve, I.style == 1 ? "grab" : (I.style == 2 ? "punch" : "press"));
        ImGui::ProgressBar(I.curve, ImVec2(-1, 0), I.phase == InteractState::Idle ? "idle" : "reaching");
        if (I.pending)
            ImGui::Text("Deferred %s interaction fires in %.2f s", InteractionModeName(I.mode), max(I.fireIn, 0.0f));
        ImGui::Text("Last interaction: %s / %s on %s", InteractionTypeName(I.lastType), InteractionModeName(I.lastMode), I.lastEntity.empty() ? "-" : I.lastEntity.c_str());
        if (!I.skipReason.empty())
            ImGui::TextDisabled("Last one not animated: %s", I.skipReason.c_str());
    }

    // --- advanced -----------------------------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Advanced (rig, arms, diagnostics)"))
    {
        ImGui::TextWrapped("Nothing here needs touching for normal use. The defaults are what the rig was found to want.");
        CheckboxInt("Debug overlay + click log", s.interactDebugMarker,
            "Drawn every frame while on: red = reach target, green = where the wrist is asked to be, cyan = where the wrist joint really is, magenta = shoulder;\n"
            "a line of numbers at the bottom (residual, correction, chain resets). Clicks on screens are logged to Game.log.");
        ImGui::Text("Rig");
        CheckboxInt("Drive the left arm's IK during the reach", s.interactForceLeftIk,
            "Pushes the left arm's animation-driven IK weight while the hand is out, like the game does for the right arm. Off, one-handed stances ignore the target.");
        ImGui::SameLine();
        ImGui::TextDisabled(I.weightJoint >= 0 ? "(weight joint: %s)" : "(weight joint not found - option does nothing)", I.weightJointName.c_str());
        if (s.interactForceLeftIk)
        {
            ImGui::Indent();
            CheckboxInt("Blend the IK weight in with the hand instead of switching it on", s.interactIkWeightRamp,
                "One-handed weapons (wrench, grenades) animate the support arm's IK weight at 0 with the hand off screen; switching it to 1 snaps the hand to its target in one frame.");
            if (I.weightJoint >= 0)
                ImGui::TextDisabled("IK weight now: pushed %.2f, animated %.2f%s", I.ikWeightPushed, I.animIkWeight, I.animIkWeightValid ? "" : " (not captured)");
            ImGui::Unindent();
        }
        CheckboxInt("Push the style's hand rotation too", s.interactRotate, "The press / grab styles' pitch / yaw / roll, additive on the IK target (not when a pose owns the wrist).");
        const char* wristModes[] = { "Through the IK target joint", "Through the hand joint (relative to the forearm)", "Both" };
        ImGui::Combo("A pose's wrist orientation applied", &s.interactWristMode, wristModes, 3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The arm IK runs after our overrides; 'both' is what was found to work. Change only if the wrist sliders of a pose do nothing.");
        ImGui::SliderFloat("Hand position correction gain", &s.interactCorrGain, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Closed loop on the hand joint: how much of last frame's hand error is fed back per frame (0 = off).");
        CheckboxInt("Show the arms for the reach where the game hides them", s.interactShowArms, "Sets the arms' render flag while the reach / resting hand is active (no weapon, screens) and gives it back afterwards.");
        if (ImGui::Button("Log the arm / hand joint names"))
            LogHandJoints();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes the first-person arms' hand / arm / IK joint names to the game log.");
        ImGui::Text("State");
        ImGui::TextDisabled("%s%s%s, arms slot flags 0x%X%s, game context ran last frame: %s%s", I.unarmed ? "no weapon" : "weapon out",
            I.examining ? ", on a screen" : "", I.bodyShiftActive ? " (body brought to the camera)" : "", I.slotFlagsNow, I.armsForced ? " (arms shown by us)" : "",
            m_diag.ctxUpdatesLastFrame > 0 ? "yes" : "no", I.ownQueueUsed ? " -> our own pose modifier" : (m_ownQueueBroken ? " (own pose modifier disabled after a failed check)" : ""));
        CheckboxInt("Before any weapon was equipped: drive the hand with our own pose modifier", s.interactNoContextFallback,
            "The game's weapon animation context, which carries our pushes, is only created when a weapon is first equipped. Until then this fills in.");
        if (I.bodyShiftActive)
            ImGui::TextDisabled("camera is %.2f m from the head (%.2f %.2f %.2f)", I.bodyShift.GetLength(), I.bodyShift.x, I.bodyShift.y, I.bodyShift.z);
        if (I.examCursorValid)
            ImGui::TextDisabled("last screen click at world (%.2f %.2f %.2f)", I.examCursorWorld.x, I.examCursorWorld.y, I.examCursorWorld.z);
        ImGui::TextDisabled("started %d, deferred %d, fired %d, dropped %d, skipped %d", I.started, I.deferred, I.fired, I.dropped, I.skipped);
        ImGui::TextDisabled("target view (%.2f %.2f %.2f)%s  hand (%.2f %.2f %.2f)  desired (%.2f %.2f %.2f)",
            I.targetView.x, I.targetView.y, I.targetView.z, I.clamped ? " [clamped]" : "", I.handView.x, I.handView.y, I.handView.z,
            I.desiredView.x, I.desiredView.y, I.desiredView.z);
        ImGui::TextDisabled("left IK joint %d, weight joint %d, pushes %d, chain resets %d, add |t| %.3f m, hand correction (%.3f %.3f %.3f) err %.3f m",
            m_lock.leftIkJoint, I.weightJoint, I.pushes, I.chainResets, I.lastAdd.t.GetLength(), I.corr.x, I.corr.y, I.corr.z, I.corrError);
    }
}

void ModMain::DrawWindow()
{
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Viewmodel Tweaks", &open))
    {
        ViewmodelSettings& s = m_settings;

        ImGui::TextDisabled("New here? Start with Quick Settings. Aim with %s. Per-weapon poses live in the Weapon tab. Ctrl+click a slider to type a value.", GetKeyName(s.aimKey));
        CheckboxInt("Enable position / rotation offsets", s.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("| %s%s%s | %s", m_isCrouching ? "crouching" : "standing", m_isAiming ? " | aiming" : "", m_feel.sprinting ? " | sprinting" : "",
            m_currentWeaponClass.empty() ? "no weapon" : m_currentWeaponClass.c_str());
        {
            bool vanilla = s.bypass != 0;
            if (ImGui::Checkbox("Vanilla viewmodel (bypass the whole mod)", &vanilla))
                s.bypass = vanilla ? 1 : 0;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Switches off every viewmodel feature at once - offsets, ironsights, feel, weapon FOV, convergence, wall pull-back,\n"
                                  "spread changes - for before/after comparisons. The reticle, world FOV and sprint sensitivity settings stay as they are.\n"
                                  "Your settings are kept; untick to get everything back.");
            if (s.bypass)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "VANILLA - the mod is bypassed");
            }
        }
        if (!m_offsetHookActive || !m_cameraHookActive)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Warning: some game hooks could not be installed (%s%s). This game build is not the one Chairloader supports; the mod is partly disabled.",
                m_offsetHookActive ? "" : "weapon offsets ", m_cameraHookActive ? "" : "camera/ironsights");
        if (m_nanRecoveries > 0 && s.showAdvanced)
            ImGui::TextDisabled("Numeric recoveries this session: %d (frames where a bad value from the game or the mod was caught and reset)", m_nanRecoveries);

        ImGui::Separator();

        if (ImGui::BeginTabBar("vm_tabs"))
        {
            //------------------------------------------------------------------ Quick Settings
            if (ImGui::BeginTabItem("Quick Settings"))
            {
                ImGui::TextWrapped("The essentials. Everything else (per-weapon poses, crouch offsets, nudge keys, fine tuning) is in the other tabs.");
                ImGui::Spacing();

                CheckboxInt("Enable weapon offsets", s.enabled, "Master switch for all position / rotation offsets, convergence and wall pull-back.");
                ImGui::SameLine();
                CheckboxInt("Vanilla viewmodel##q", s.bypass, "Bypass every viewmodel feature (everything except reticle, world FOV and sprint sensitivity) for before/after comparisons.");
                ImGui::Indent();
                CheckboxInt("Override weapon FOV", s.fovEnabled, "Vertical FOV used to render the weapon and arms. Game default is 55. Higher = smaller weapon.");
                ImGui::BeginDisabled(!s.fovEnabled);
                ImGui::SliderFloat("Weapon FOV##q", &s.fov, 20.0f, 120.0f, "%.1f deg");
                ImGui::EndDisabled();
                ImGui::Unindent();
                ImGui::Spacing();

                CheckboxInt("Override field of view", s.worldFovEnabled, "The game's horizontal FOV (cl_hfov), same as the options menu slider but kept where you set it.");
                ImGui::BeginDisabled(!s.worldFovEnabled);
                ImGui::SliderFloat("Field of view##q", &s.worldFov, 60.0f, 130.0f, "%.1f deg (horizontal)");
                ImGui::EndDisabled();
                if (!s.worldFovEnabled && gEnv && gEnv->pConsole)
                    if (ICVar* pVar = gEnv->pConsole->GetCVar("cl_hfov"))
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("use current"))
                            s.worldFov = pVar->GetFVal();
                    }
                CheckboxInt("Sensitivity sprint scale", s.sprintSensEnabled,
                    "The game turns the look sensitivity down while sprinting. 1.0 = same as walking.");
                ImGui::BeginDisabled(!s.sprintSensEnabled);
                ImGui::SliderFloat("Sprint sensitivity##q", &s.sprintSensScale, 0.1f, 2.0f, "x%.2f");
                ImGui::EndDisabled();
                if (m_gameSprintSensScale >= 0.0f)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(game: x%.2f)", m_gameSprintSensScale);
                }
                ImGui::Spacing();

                CheckboxInt("Enable ironsights", s.aimEnabled, "Hold (or toggle) the aim key to bring the weapon up to the sights. Poses are per weapon (Weapon tab).");
                ImGui::BeginDisabled(!s.aimEnabled);
                ImGui::Indent();
                ImGui::Text("Aim key: %s", GetKeyName(s.aimKey));
                ImGui::SameLine();
                if (m_waitingForAimKey)
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a key or mouse button... (Esc cancels)");
                else if (ImGui::Button("Bind##q"))
                    m_waitingForAimKey = true;
                ImGui::SameLine();
                if (ImGui::Button("RMB##q"))
                    s.aimKey = (int)eKI_Mouse2;
                CheckboxInt("Toggle instead of hold##q", s.aimToggle);
                CheckboxInt("Block the game's own action on this key##q", s.aimConsumeKey,
                    "Swallows the key so whatever the game has bound to it (e.g. RMB) does nothing while this mod is enabled.");
                CheckboxInt("Separate weapon FOV while aiming##q", s.aimFovEnabled, "Blends the weapon FOV towards this value while aiming. Lower = bigger weapon.");
                ImGui::BeginDisabled(!s.aimFovEnabled);
                ImGui::SliderFloat("Ironsights FOV##q", &s.aimFov, 20.0f, 120.0f, "%.1f deg");
                ImGui::EndDisabled();
                CheckboxInt("Zoom camera while aiming##q", s.aimCameraZoom, "Lowers the world FOV while aiming, like ADS in modern shooters.");
                ImGui::BeginDisabled(!s.aimCameraZoom);
                ImGui::SliderFloat("Camera zoom##q", &s.aimCameraZoomFactor, 0.2f, 1.5f, "x%.2f FOV");
                ImGui::EndDisabled();
                const char* sensModesQ[] = { "Game default (weapon's zoomed multiplier)", "Match camera zoom factor", "Custom multiplier" };
                ImGui::Combo("ADS look sensitivity##q", &s.aimSensMode, sensModesQ, 3);
                ImGui::BeginDisabled(s.aimSensMode != 2);
                ImGui::SliderFloat("Sensitivity while aiming##q", &s.aimSensScale, 0.05f, 3.0f, "x%.2f");
                ImGui::EndDisabled();
                ImGui::Unindent();
                ImGui::EndDisabled();
                ImGui::Spacing();

                CheckboxInt("Enable weapon convergence", s.convergeEnabled,
                    "Hip fire: rotates the weapon so the barrel points at what the crosshair is over. Tuning in the Global tab.");
                CheckboxInt("Enable wall pull-back", s.wallPushEnabled,
                    "Slides the weapon back towards the camera near walls. Distances in the Global tab, amount per weapon in the Weapon tab.");
                ImGui::Spacing();
                ImGui::Text("Feel");
                ImGui::SameLine();
                CheckboxInt("Sprint pose##q", s.sprintPoseEnabled, "Lowers and tilts the weapon while sprinting, with a bit more sway. Tuning in the Feel tab.");
                ImGui::SameLine();
                CheckboxInt("Sway while aiming##q", s.aimSwayEnabled, "The sights drift in a slow figure-eight: large when they come up, after sprinting or while moving, calm when you hold still. Tuning in the Feel tab.");
                ImGui::SameLine();
                CheckboxInt("View drag##q", s.dragEnabled, "GoldenEye-style: the weapon follows your turns on a spring. Tuning in the Feel tab.");
                ImGui::Spacing();
                DrawReticleControls(s, "qret");
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Global
            if (ImGui::BeginTabItem("Global"))
            {
                ImGui::BeginDisabled(!s.enabled);
                ImGui::TextWrapped("Offsets applied to ALL weapons. Per-weapon offsets (next tab) are added on top.");
                if (ImGui::CollapsingHeader("Standing offset", ImGuiTreeNodeFlags_DefaultOpen))
                    DrawPoseSliders(s.base, "base", 30.0f, 45.0f);
                if (ImGui::CollapsingHeader("Crouch offset", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    CheckboxInt("Enable crouch offset", s.crouchEnabled, "Blends this extra offset in while sneaking (crouched).");
                    ImGui::BeginDisabled(!s.crouchEnabled);
                    ImGui::SliderFloat("Transition time", &s.crouchTime, 0.0f, 1.5f, "%.2f s");
                    ImGui::ProgressBar(SmoothStep01(m_crouchBlend), ImVec2(-1, 0), m_isCrouching ? "crouched" : "standing");
                    DrawPoseSliders(s.crouch, "crouch", 30.0f, 45.0f);
                    ImGui::EndDisabled();
                }
                if (ImGui::CollapsingHeader("Reloading", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    CheckboxInt("Fade the offsets out while reloading", s.reloadFadesOffsets,
                        "Reload animations move the support hand to where the weapon is in the STOCK pose (shells, magazines), so a\n"
                        "shifted weapon leaves a gap between hand and shell. With this on, every hip-side change (pose, convergence,\n"
                        "wall pull-back, sprint pose, drag) eases out when a reload starts and back in when it ends.");
                    ImGui::BeginDisabled(!s.reloadFadesOffsets);
                    ImGui::SliderFloat("Fade time", &s.reloadFadeTime, 0.05f, 0.6f, "%.2f s");
                    ImGui::ProgressBar(SmoothStep01(m_reloadFade), ImVec2(-1, 0), m_wsReloading ? "reloading" : "not reloading");
                    ImGui::EndDisabled();
                }
                if (ImGui::CollapsingHeader("Weapon convergence (hip fire)"))
                {
                    CheckboxInt("Point the weapon at the crosshair's impact point", s.convergeEnabled,
                        "Raycasts along the view every frame and rotates the weapon in place so the barrel\nconverges on what the crosshair is over. Noticeable at close range, invisible far away.\nFades out while aiming down sights.");
                    ImGui::BeginDisabled(!s.convergeEnabled);
                    ImGui::SliderFloat("Strength", &s.convergeStrength, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Max angle", &s.convergeMaxAngle, 0.0f, 30.0f, "%.1f deg");
                    ImGui::SliderFloat("Smoothing", &s.convergeSmoothTime, 0.0f, 0.5f, "%.2f s");
                    ImGui::SliderFloat("Max distance", &s.convergeMaxDist, 2.0f, 100.0f, "%.0f m");
                    ImGui::TextDisabled("Hit: %s at %.2f m | yaw %.2f (target %.2f) pitch %.2f (target %.2f) deg | weapon offset from eye (%.1f, %.1f, %.1f) cm",
                        m_convergeHit ? "yes" : "no", m_convergeDist, m_convergeYaw, m_convergeTargetYaw, m_convergePitch, m_convergeTargetPitch,
                        m_render.hipRelCam.t.x * 100, m_render.hipRelCam.t.y * 100, m_render.hipRelCam.t.z * 100);
                    {
                        const float tau = max(s.convergeSmoothTime, 0.0f);
                        const float k = (tau > 0.0005f) ? (1.0f - expf(-m_dtUsed / tau)) : 1.0f;
                        ImGui::TextDisabled("Filter step %.2f ms (game frame time %.2f ms) | %.0f updates/s | %.1f %% of the gap closed per update",
                            m_dtUsed * 1000.0f, m_dtGame * 1000.0f, m_updateHz, k * 100.0f);
                    }
                    if (ImGui::Button("Save trace CSV"))
                        SaveTrace("manual");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Writes the last ~30 s of per-update values (filter output vs. the weapon's measured orientation) to\n"
                            "Mods/config/Vee.ViewmodelTweaks.trace.csv. Also written automatically when the weapon jumps while the filter did not.");
                    ImGui::SameLine();
                    ImGui::Checkbox("auto on pop", &m_traceAuto);
                    if (!m_traceStatus.empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", m_traceStatus.c_str());
                    }
                    ImGui::EndDisabled();
                    ImGui::Spacing();
                    CheckboxInt("Prevent aiming while colliding with a wall", s.aimWallBlockEnabled,
                        "No ironsights while the crosshair's impact point is closer than the weapon's reach\n"
                        "(its distance from the eye + its wall pull-back, so longer weapons block sooner). Uses the same view raycast.");
                    ImGui::BeginDisabled(!s.aimWallBlockEnabled);
                    ImGui::SliderFloat("Tolerance", &s.aimWallBlockScale, 0.25f, 4.0f, "x%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Multiplier on the blocking distance. Higher = aiming is blocked further away from the wall.");
                    ImGui::TextDisabled("Blocks when the wall is closer than %.2f m | wall at %.2f m | %s", m_aimBlockDist, m_convergeDist,
                        m_aimBlockedByWall ? "BLOCKED" : "free");
                    ImGui::EndDisabled();
                }
                if (ImGui::CollapsingHeader("Wall pull-back"))
                {
                    CheckboxInt("Pull the weapon back near walls", s.wallPushEnabled,
                        "Uses the same view raycast: the closer the crosshair's impact point, the further the weapon slides\n"
                        "back towards the camera, as if you were keeping it from hitting the wall.\n"
                        "How far each weapon moves is set per weapon (Weapon tab): a little for the pistol, more for the shotgun / GLOO gun.");
                    ImGui::BeginDisabled(!s.wallPushEnabled);
                    ImGui::SliderFloat("Starts at", &s.wallPushStartDist, 0.3f, 3.0f, "%.2f m");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Distance from the camera to the wall where the pull-back begins.");
                    ImGui::SliderFloat("Full at", &s.wallPushFullDist, 0.0f, 2.0f, "%.2f m");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Distance where the weapon has moved back by its full per-weapon amount. Must be less than 'Starts at'.");
                    ImGui::SliderFloat("Smoothing##wall", &s.wallPushSmoothTime, 0.0f, 0.5f, "%.2f s");
                    CheckboxInt("Also while aiming", s.wallPushWhileAiming,
                        "The pull-back is along the view axis, so the sights stay on the crosshair; the weapon just gets a bit closer/bigger.");
                    {
                        const WeaponSettings* pW = FindCurrentWeapon();
                        ImGui::TextDisabled("Now: %.1f cm back (target %.1f cm) | this weapon's max: %.1f cm | wall at %.2f m",
                            m_wallPush * 100, m_wallPushTarget * 100, (pW ? pW->wallPush : WeaponSettings().wallPush) * 100, m_convergeDist);
                    }
                    ImGui::Separator();
                    CheckboxInt("Near-wall pose", s.wallPoseEnabled,
                        "Each weapon has a second hip pose for when it is against a wall (Weapon tab > Near-wall pose; the shotgun\n"
                        "goes muzzle-up by default). It blends in with the pull-back: 0 while the weapon is free, 1 when it has\n"
                        "used its full pull-back, i.e. is pressed against the wall.");
                    ImGui::BeginDisabled(!s.wallPoseEnabled);
                    {
                        float pct = s.wallPoseStart * 100.0f;
                        if (ImGui::SliderFloat("Pose starts at", &pct, 0.0f, 95.0f, "%.0f%% of the pull-back")) s.wallPoseStart = pct / 100.0f;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Bias: fraction of the weapon's full pull-back at which the pose starts blending in.");
                    {
                        float pct = s.wallPoseFull * 100.0f;
                        if (ImGui::SliderFloat("Pose fully applied at", &pct, 5.0f, 100.0f, "%.0f%% of the pull-back")) s.wallPoseFull = pct / 100.0f;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Bias: fraction of the full pull-back at which the pose is complete. Must be above 'starts at'.");
                    if (s.wallPoseFull <= s.wallPoseStart + 0.01f) s.wallPoseFull = min(s.wallPoseStart + 0.01f, 1.0f);
                    ImGui::SliderFloat("Convergence yields to the pose", &s.wallPoseConvergeFade, 0.0f, 1.0f, "x%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The pose points the barrel away from the wall on purpose; this fades the hip convergence out in proportion.");
                    ImGui::Separator();
                    ImGui::TextDisabled("Looking up / down");
                    ImGui::SliderFloat("Pose fades from", &s.wallPosePitchStart, 0.0f, 85.0f, "%.0f deg");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Camera pitch (up or down) at which the near-wall pose starts giving way to the plain pull-back.\n"
                                          "Looking level the pose is fully there; steeply up or down the wall is no longer where the muzzle would go.");
                    ImGui::SliderFloat("Fully faded at", &s.wallPosePitchFull, 1.0f, 90.0f, "%.0f deg");
                    if (s.wallPosePitchFull <= s.wallPosePitchStart + 0.5f) s.wallPosePitchFull = min(s.wallPosePitchStart + 0.5f, 90.0f);
                    ImGui::SliderFloat("Fade strength", &s.wallPosePitchStrength, 0.0f, 1.0f, "x%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("1 = at full pitch only the plain pull-back remains; lower values keep part of the pose; 0 turns the pitch fade off.");
                    ImGui::TextDisabled("Now: pose blend %.0f%% | camera pitch %.0f deg -> pitch factor x%.2f", m_wallBlend * 100.0f, m_camPitchDeg, m_wallPitchFade);
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Feel
            if (ImGui::BeginTabItem("Feel"))
            {
                ImGui::BeginDisabled(!s.enabled);
                const FeelState& f = m_feel;
                if (ImGui::CollapsingHeader("Sprint pose", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    CheckboxInt("Lower the weapon while sprinting", s.sprintPoseEnabled);
                    ImGui::BeginDisabled(!s.sprintPoseEnabled);
                    ImGui::TextDisabled("%s%s | blend %.2f | speed %.1f m/s", f.sprinting ? "sprinting" : "not sprinting", f.zeroG ? " | zero-G" : "", f.sprintBlend, f.speed);
                    DrawPoseSliders(s.sprint, "sprintpose", 20.0f, 30.0f);
                    ImGui::SliderFloat("Blend in", &s.sprintBlendIn, 0.05f, 1.0f, "%.2f s");
                    ImGui::SliderFloat("Blend out", &s.sprintBlendOut, 0.05f, 1.0f, "%.2f s");
                    CheckboxInt("No aiming while sprinting", s.sprintBlocksAim, "The aim key is ignored while sprinting; the sights come up as soon as you stop.");
                    CheckboxInt("Also in zero-G", s.sprintInZeroG,
                        "The thruster boost in zero-G counts as sprinting for the game. Off (default): no sprint pose, sway or aim block while\n"
                        "floating - there is no running body to lower the weapon for. On: boosting behaves like sprinting.");
                    CheckboxInt("Extra sway while sprinting", s.sprintSwayEnabled, "A side-to-side / up-down figure on top of the game's own sprint animation.");
                    ImGui::BeginDisabled(!s.sprintSwayEnabled);
                    ImGui::SliderFloat("Sway position", &s.sprintSwayPos, 0.0f, 0.05f, "%.3f m");
                    ImGui::SliderFloat("Sway roll", &s.sprintSwayRot, 0.0f, 6.0f, "%.1f deg");
                    ImGui::SliderFloat("Sway frequency", &s.sprintSwayFreq, 0.5f, 5.0f, "%.2f Hz");
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                }
                if (ImGui::CollapsingHeader("Sway and settle while aiming", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    CheckboxInt("Sway while aiming", s.aimSwayEnabled,
                        "A slow figure-eight of the sights. The rotation part moves the sights off the crosshair, so with the reticle hidden\n"
                        "while aiming (FOV tab) it is real inaccuracy you have to time your shots around; with the reticle visible it is cosmetic.");
                    ImGui::BeginDisabled(!s.aimSwayEnabled);
                    ImGui::TextDisabled("amplitude x%.2f | aiming for %.1f s | %.1f s since sprint | breath %.1f s%s", f.swayAmplitude, f.aimTime,
                        min(f.timeSinceSprint, 999.0f), f.steadyLeft, f.steadyActive ? " (steady)" : "");
                    ImGui::SliderFloat("Position at rest", &s.aimSwayPos, 0.0f, 0.02f, "%.4f m");
                    ImGui::SliderFloat("Rotation at rest", &s.aimSwayRot, 0.0f, 3.0f, "%.2f deg");
                    ImGui::SliderFloat("Frequency", &s.aimSwayFreq, 0.05f, 2.0f, "%.2f Hz");
                    ImGui::SliderFloat("When the sights come up", &s.aimSwayInitial, 1.0f, 8.0f, "x%.1f");
                    ImGui::SliderFloat("Settle time", &s.aimSwaySettleTime, 0.1f, 4.0f, "%.2f s");
                    ImGui::SliderFloat("Moving", &s.aimSwayMoveMult, 0.0f, 8.0f, "+x%.1f at walking speed");
                    ImGui::SliderFloat("After sprinting", &s.aimSwaySprintPenalty, 0.0f, 8.0f, "+x%.1f");
                    ImGui::SliderFloat("Sprint recovery", &s.aimSwaySprintRecover, 0.1f, 5.0f, "%.2f s");
                    ImGui::Separator();
                    CheckboxInt("Hold breath to steady", s.steadyEnabled);
                    ImGui::BeginDisabled(!s.steadyEnabled);
                    ImGui::Text("Steady key: %s", GetKeyName(s.steadyKey));
                    ImGui::SameLine();
                    if (m_waitingForSteadyKey)
                        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a key... (Esc cancels)");
                    else if (ImGui::Button("Bind##steady"))
                        m_waitingForSteadyKey = true;
                    ImGui::SameLine();
                    if (ImGui::Button("None##steady"))
                        s.steadyKey = 0;
                    CheckboxInt("Swallow the steady key", s.steadyConsumeKey, "The game does not see the key while it is bound here.");
                    {
                        float pct = s.steadyReduce * 100.0f;
                        if (ImGui::SliderFloat("Sway reduction", &pct, 0.0f, 100.0f, "%.0f %%"))
                            s.steadyReduce = pct / 100.0f;
                    }
                    ImGui::SliderFloat("Breath", &s.steadyDuration, 0.5f, 15.0f, "%.1f s");
                    ImGui::SliderFloat("Recovery", &s.steadyRecover, 0.5f, 15.0f, "%.1f s");
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                }
                if (ImGui::CollapsingHeader("View drag (GoldenEye)", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    CheckboxInt("Weapon follows your turns on a spring", s.dragEnabled,
                        "Turning drags the weapon along (or behind), then it springs back. Uses this frame's turn rate, so there is no input lag on the camera itself.");
                    ImGui::BeginDisabled(!s.dragEnabled);
                    ImGui::TextDisabled("turn rate %.2f / %.2f rad/s | spring %.2f / %.2f", f.yawRate, f.pitchRate, f.dragX, f.dragY);
                    ImGui::Text("Direction");
                    ImGui::SameLine();
                    ImGui::RadioButton("Leads into the turn (GoldenEye)", &s.dragLead, 1);
                    ImGui::SameLine();
                    ImGui::RadioButton("Lags behind", &s.dragLead, 0);
                    ImGui::SliderFloat("Position amount", &s.dragPos, 0.0f, 0.05f, "%.3f m per rad/s");
                    ImGui::SliderFloat("Rotation amount", &s.dragRot, 0.0f, 10.0f, "%.1f deg per rad/s");
                    ImGui::SliderFloat("Stiffness", &s.dragStiffness, 1.0f, 30.0f, "%.1f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Higher = reacts and returns faster.");
                    ImGui::SliderFloat("Damping", &s.dragDamping, 0.2f, 1.5f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("1 = settles without overshoot; lower values wobble a little at the end of a turn.");
                    ImGui::SliderFloat("Max position", &s.dragMaxPos, 0.0f, 0.15f, "%.3f m");
                    ImGui::SliderFloat("Max rotation", &s.dragMaxRot, 0.0f, 20.0f, "%.1f deg");
                    ImGui::SliderFloat("Up / down relative to sideways", &s.dragPitchScale, 0.0f, 1.5f, "x%.2f");
                    ImGui::SliderFloat("While aiming", &s.dragAimScale, 0.0f, 1.0f, "x%.2f");
                    ImGui::EndDisabled();
                }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Weapon
            if (ImGui::BeginTabItem("Weapon"))
            {
                ImGui::BeginDisabled(!s.enabled);
                if (m_currentWeaponClass.empty())
                {
                    ImGui::TextWrapped("Equip a weapon to edit its offsets.");
                }
                else
                {
                    WeaponSettings& w = GetCurrentWeapon();
                    ImGui::Text("Current weapon: %s%s", m_currentWeaponClass.c_str(), w.valid ? "  (customized)" : "  (built-in defaults)");
                    if (w.valid)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("Reset to built-in"))
                        {
                            if (const WeaponSettings* pB = WeaponSettings::BuiltIn(m_currentWeaponClass.c_str()))
                                w = *pB;
                            else
                            {
                                w = WeaponSettings();
                                w.aim = WeaponSettings::DefaultAim();
                            }
                            m_weaponsDirty = true;
                        }
                    }
                    bool ch = false;
                    bool allow = w.aimAllowed;
                    if (ImGui::Checkbox("Allow aiming with this weapon", &allow))
                    {
                        w.aimAllowed = allow;
                        ch = true;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Turn off for wrench, grenades, etc. The aim key then does nothing while this weapon is equipped.");
                    ch |= SliderCm("Wall pull-back", w.wallPush, 30.0f, "How far this weapon slides back towards the camera when the crosshair is right against a wall\n(Global tab > Wall pull-back sets the distances). Longer weapons want more: pistol ~4 cm, shotgun / GLOO gun ~12 cm.");
                    if (w.wallPush < 0.0f) { w.wallPush = 0.0f; }

                    if (ImGui::CollapsingHeader("Hip offset (not aiming)", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::TextWrapped("Added on top of the global standing/crouch offsets while this weapon is equipped.");
                        ch |= DrawPoseSliders(w.hip, "whip", 30.0f, 45.0f);
                    }
                    if (ImGui::CollapsingHeader("Near-wall pose"))
                    {
                        ImGui::TextWrapped("Blended in on top of the hip offset as this weapon's wall pull-back builds up (0 = free, 1 = pressed "
                                           "against the wall; Global tab > Wall pull-back sets the biases). Pitch + = muzzle up.");
                        ch |= DrawPoseSliders(w.wall, "wwall", 30.0f, 90.0f);
                        float amt = w.wallPoseAmount;
                        if (ImGui::SliderFloat("Amount##wallpose", &amt, 0.0f, 1.0f, "x%.2f"))
                        {
                            w.wallPoseAmount = amt;
                            ch = true;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Multiplier on the blend for this weapon; 0 disables the pose for it.");
                        ImGui::TextDisabled("Now: %.0f%%", m_wallBlend * 100.0f);
                    }
                    if (ImGui::CollapsingHeader("Aim pose (ironsights)", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::BeginDisabled(!w.aimAllowed);
                        ImGui::TextWrapped("Weapon bone relative to the camera while aiming. Centered = Right/Left 0 and no rotation; "
                                           "tune Forward and Up/Down until the sights sit on the crosshair, then small rotations.");
                        ImGui::PushID("waim");
                        ch |= SliderCm("Right / Left", w.aim.posX, 20.0f, "0 = centered on the crosshair.");
                        ch |= SliderCm("Forward", w.aim.posY, 60.0f, "Distance of the weapon bone from the camera.");
                        ch |= SliderCm("Up / Down", w.aim.posZ, 20.0f, "Raise until the sights meet the crosshair.");
                        ch |= SliderDeg("Pitch", w.aim.pitch, 90.0f, "If the weapon bone's forward axis is not the barrel, fix it here.");
                        ch |= SliderDeg("Yaw", w.aim.yaw, 90.0f, nullptr);
                        ch |= SliderDeg("Roll", w.aim.roll, 90.0f, nullptr);
                        if (ImGui::Button("Start from current hip pose"))
                        {
                            w.aim.FromQuatT(m_render.hipRelCam);
                            ch = true;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Copies the weapon's current (hip) camera-relative transform into the aim pose\n(measured exactly at render time). Do this while NOT aiming, then Center and raise it.");
                        ImGui::SameLine();
                        if (ImGui::Button("Center (x=0, no rotation)"))
                        {
                            w.aim.posX = 0.0f; w.aim.pitch = w.aim.yaw = w.aim.roll = 0.0f;
                            ch = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Reset##aim"))
                        {
                            w.aim = WeaponSettings::DefaultAim();
                            ch = true;
                        }
                        ImGui::Spacing();
                        ImGui::Text("Firing feel while aiming");
                        ch |= ImGui::SliderFloat("Coupling after a shot", &w.fireCoupling, 0.0f, 1.0f, "%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Head-bob coupling used right after each shot (decays to the Aim tab's value). Lets the fire kick move the\nweapon relative to the sights instead of the weapon being glued to the camera.");
                        ch |= ImGui::SliderFloat("... for", &w.fireCouplingTime, 0.05f, 1.0f, "%.2f s");
                        ch |= ImGui::SliderFloat("Recoil motion multiplier", &w.aimRecoilScale, 0.0f, 3.0f, "x%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Per-weapon multiplier on the game's procedural recoil offset while aiming (times the Aim tab's global one).\nThe shotgun's kick is mostly this.");
                        ch |= ImGui::SliderFloat("Fire animation kick", &w.aimKickScale, 0.0f, 3.0f, "x%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("How much of the fire animation's hand motion shows while aiming (the pistol's kick is animated, not procedural).\nMeasured against the pose just before the shot and active for the coupling time above. 0 = still sights.");
                        ImGui::Spacing();
                        ImGui::Text("Bullet spread (pistol / shotgun only)");
                        ch |= ImGui::SliderFloat("Spread while aiming##w", &w.aimSpreadMult, 0.0f, 2.0f, "x%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Multiplier on the spread cone while aiming. Pistol: the random bullet cone. Shotgun: the pellet cone and its wobble.\n0 = every shot exactly on the crosshair.");
                        ch |= ImGui::SliderFloat("Spread while not aiming##w", &w.hipSpreadMult, 0.0f, 3.0f, "x%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Above 1 makes hip fire less accurate than the stock game, to reward aiming.");
                        ImGui::PopID();
                        ImGui::EndDisabled();
                    }
                    if (ch)
                    {
                        w.valid = true;
                        m_weaponsDirty = true;
                    }

                    if (ImGui::TreeNode("All weapons"))
                    {
                        for (auto& kv : m_weapons)
                        {
                            if (!kv.second.valid)
                                continue;
                            ImGui::Text("%s  aim %s  hip(%.1f,%.1f,%.1f cm)  aim(%.1f,%.1f,%.1f cm)", kv.first.c_str(),
                                kv.second.aimAllowed ? "on " : "off",
                                kv.second.hip.posX * 100, kv.second.hip.posY * 100, kv.second.hip.posZ * 100,
                                kv.second.aim.posX * 100, kv.second.aim.posY * 100, kv.second.aim.posZ * 100);
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Aim
            if (ImGui::BeginTabItem("Aim"))
            {
                ImGui::BeginDisabled(!s.enabled);
                CheckboxInt("Enable aiming", s.aimEnabled,
                    "Hold (or toggle) the aim key to bring the weapon up to the sights,\noptionally zooming the camera like ADS in modern shooters.");
                ImGui::BeginDisabled(!s.aimEnabled);

                ImGui::Text("Aim key: %s", GetKeyName(s.aimKey));
                ImGui::SameLine();
                if (m_waitingForAimKey)
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a key or mouse button... (Esc cancels)");
                else if (ImGui::Button("Bind"))
                    m_waitingForAimKey = true;
                ImGui::SameLine();
                if (ImGui::Button("RMB"))
                    s.aimKey = (int)eKI_Mouse2;
                CheckboxInt("Toggle instead of hold", s.aimToggle);
                ImGui::SameLine();
                CheckboxInt("Block the game's own action on this key", s.aimConsumeKey,
                    "Swallows the key so whatever the game has bound to it (e.g. RMB) does nothing while this mod is enabled.");

                ImGui::SliderFloat("Transition time", &s.aimTime, 0.0f, 1.0f, "%.2f s");
                ImGui::ProgressBar(SmoothStep01(m_aimBlend), ImVec2(-1, 0), m_isAiming ? "aiming" : "hip");
                CheckboxInt("Ignore crouch offset while aiming", s.aimIgnoresCrouch,
                    "Fades the crouch offset out as the aim pose blends in, so the sights line up in any stance.");
                CheckboxInt("No aiming while reloading", s.aimBlockReload,
                    "Reload animations are authored for the stock pose; locking the weapon to the sights during one distorts the hands.\n"
                    "The sights drop when a reload starts and come back when it ends (the aim key can stay held).");
                CheckboxInt("No aiming while switching weapons", s.aimBlockSwitch,
                    "Same for the holster / draw animations: aiming waits until the new weapon is ready.");
                ImGui::BeginDisabled(!s.aimBlockSwitch);
                ImGui::SliderFloat("Draw time", &s.aimSwitchDelay, 0.0f, 2.0f, "%.2f s");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How long after a weapon change aiming stays blocked. The game flags the weapon ready before its raise\n"
                        "animation is over, so this covers the rest of it; too short and the sights can capture a hand that is still moving.");
                ImGui::EndDisabled();
                ImGui::TextDisabled("Weapon: %s%s%s%s%s", m_wsReloading ? "reloading " : "", m_wsUnequipping ? "holstering " : "",
                    m_wsDrawing ? "drawing " : "", m_wsReady ? "" : "(busy) ",
                    (!m_wsReloading && !m_wsUnequipping && !m_wsDrawing) ? "idle" : "");

                ImGui::Spacing();
                CheckboxInt("Zoom camera while aiming", s.aimCameraZoom, "Lowers the world FOV while aiming (the weapon gets bigger with it, like real ADS).");
                ImGui::BeginDisabled(!s.aimCameraZoom);
                ImGui::SliderFloat("Camera zoom", &s.aimCameraZoomFactor, 0.2f, 1.5f, "x%.2f FOV");
                ImGui::EndDisabled();
                CheckboxInt("Separate weapon FOV while aiming", s.aimFovEnabled,
                    "Blends the weapon FOV towards this value while aiming (on top of the camera zoom). Lower = bigger weapon.");
                ImGui::BeginDisabled(!s.aimFovEnabled);
                ImGui::SliderFloat("Aim weapon FOV", &s.aimFov, 20.0f, 120.0f, "%.1f deg");
                ImGui::EndDisabled();

                ImGui::Separator();
                ImGui::Text("Ironsight motion");
                {
                    if (s.showAdvanced)
                    {
                        CheckboxInt("Exact render-time placement", s.aimRenderLock,
                            "After the game has built this frame's camera (from the evaluated skeleton) the weapon's attachment\n"
                            "transform is set to exactly camera * aim pose before rendering. No prediction, no correction loop:\n"
                            "lean, landing, crouching, bob cannot move the sights off the crosshair.\n"
                            "Off = skeleton-only lock (the target has to be predicted a frame ahead; small errors show).");
                        ImGui::BeginDisabled(!s.aimRenderLock);
                        ImGui::Indent();
                        CheckboxInt("Move hands with the weapon", s.aimHandsFollow,
                            "Moves the final hand poses (weapon hand and, when it is on the weapon, the support hand) by the same\n"
                            "delta so they stay on the grip. The delta is normally a few millimetres.");
                        ImGui::SameLine();
                        CheckboxInt("incl. support hand", s.aimLeftHandFollow);
                        ImGui::Unindent();
                        ImGui::EndDisabled();
                    }

                    ImGui::SliderFloat("Head-bob coupling", &s.aimBobAmount, -0.5f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0 = the weapon is glued to the camera (perfectly still sights).\n"
                                          "Positive = the weapon lags behind the head bob by this fraction (0.05-0.15 feels natural),\n"
                                          "negative = it leads. Derived from the camera's own motion this frame, so it never jitters.");
                    ImGui::SliderFloat("Bob separation", &s.aimBobTau, 0.05f, 1.0f, "%.2f s");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Camera motion faster than this counts as bob; slower motion (crouching, leaning, eye height) does not.");
                    ImGui::SliderFloat("Recoil / bump motion", &s.aimAnimRecoil, 0.0f, 3.0f, "x%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Multiplier on the game's procedural recoil kick and landing bump while locked (times the per-weapon multiplier in the Weapon tab).");
                    ImGui::SliderFloat("Keep look / strafe sway", &s.aimAnimSway, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Re-applies the game's look/strafe sway on top of the lock. Any amount moves the sights off the crosshair while turning.");

                    ImGui::Separator();
                    ImGui::TextDisabled("Bullet spread multipliers are per weapon (Weapon tab).");
                    if (s.showAdvanced)
                        ImGui::TextDisabled("spread hook calls: %d | shots seen: %d | fire coupling: %.2f s", m_spreadHookCalls, m_shotsSeen, m_fireTimer);
                    ImGui::Separator();
                    const char* sensModes[] = { "Game default (weapon's zoomed multiplier)", "Match camera zoom factor", "Custom multiplier" };
                    ImGui::Combo("ADS look sensitivity", &s.aimSensMode, sensModes, 3);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The game treats the aim camera zoom like its own weapon zoom and blends the look speed towards the\nweapon's zoomed camera-speed stat, which is very aggressive. Override what it blends towards instead.");
                    if (s.aimSensMode == 2)
                        ImGui::SliderFloat("Sensitivity while aiming", &s.aimSensScale, 0.05f, 3.0f, "x%.2f");
                }

                if (s.showAdvanced && ImGui::TreeNode("Lock debug"))
                {
                    const RenderLockState& R = m_render;
                    if (!m_cameraHookActive)
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Camera hook NOT installed - render-time placement unavailable.");
                    ImGui::Text("Camera updates last frame: %d (expect 1) | position was %s", R.callsLastFrame, R.positionWasWorld ? "WORLD (converted)" : "entity-relative");
                    ImGui::Text("Weapon attachment: %s | joint %d '%s' | weapon hand '%s' (%d joints) | support hand '%s' (%d joints, %s, %.1f cm from weapon)",
                        R.attachValid ? "found" : "none", R.attachJoint, R.attachJointName.c_str(),
                        R.rightHandName.c_str(), (int)R.rightSubtree.size(), R.leftHandName.c_str(), (int)R.leftSubtree.size(),
                        R.leftOnWeapon ? "on weapon" : "free", R.leftHandDist * 100);
                    TextQuatT("Camera (model, exact)", R.camModel);
                    TextQuatT("Weapon rel. camera (game)", R.weaponRelCam);
                    TextQuatT("Hip weapon rel. camera", R.hipRelCam);
                    ImGui::Text("Render-time residual (how far the skeleton-side push was off): %.2f cm, %.2f deg | max 1s: %.2f cm  <- cosmetic only, the weapon is placed exactly regardless",
                        R.residualPos * 100, RAD2DEG(R.residualRot), R.residualPosMax1s * 100);
                    ImGui::Text("Head bob (cam space): (%.2f, %.2f, %.2f) cm", R.bobCam.x * 100, R.bobCam.y * 100, R.bobCam.z * 100);
                    ImGui::Separator();
                    ImGui::Text("Skeleton side: active %s | captured %s | skeleton matches player %s | joints: right IK %d, left IK %d, weapon target %d",
                        m_lock.active ? "yes" : "no", m_lock.captured ? "yes" : "no", m_lock.charMatches ? "yes" : "NO",
                        m_lock.rightIkJoint, m_lock.leftIkJoint, m_lock.weaponJoint);
                    TextQuatT("Predicted camera (model)", m_lock.camAbs);
                    TextQuatT("Right IK joint", m_lock.ikAbs);
                    TextQuatT("Weapon bone", m_lock.weaponAbs);
                    TextQuatT("Last override", m_lock.lastTarget);
                    ImGui::Text("IK joint vs pushed target: %.2f cm (one frame of animation velocity is expected) | sensitivity hook calls: %d", m_lock.ikErr * 100, m_sensHookCalls);
                    ImGui::Text("Fire animation deviation: %.2f cm, %.1f deg | kick applied: %.2f cm | pushes not applied by the skeleton: %d | numeric recoveries: %d",
                        m_lock.kickPos * 100, RAD2DEG(m_lock.kickRot), m_lock.kick.t.GetLength() * 100, m_lock.pushesNotApplied, m_nanRecoveries);
                    TextQuatT("Game recoil", m_gameOffsets[2]);
                    TextQuatT("Game bump", m_gameOffsets[3]);
                    ImGui::Separator();
                    ImGui::TextWrapped("Self-test (works while not aiming too): raise the weapon at render time. If the weapon moves, the attachment write works; "
                                       "if the hands move with it, the joint write works.");
                    ImGui::SliderFloat("Test: raise weapon", &s.testOffsetUp, 0.0f, 0.2f, "%.2f m");
                    ImGui::SameLine();
                    CheckboxInt("hands too", s.testHands);
                    ImGui::SameLine();
                    if (ImGui::Button("Reset##test"))
                        s.testOffsetUp = 0.0f;
                    ImGui::TreePop();
                }
                if (s.showAdvanced && ImGui::TreeNode("Input debug"))
                {
                    ImGui::Text("Aim key held/toggled: %s", m_aimKeyHeld ? "yes" : "no");
                    ImGui::Text("Aiming: %s", m_isAiming ? "yes" : "no");
                    ImGui::Text("Camera zoom handle: %d", m_cameraZoomHandle);
                    ImGui::Text("Input listener: %s", m_inputListenerRegistered ? "registered" : "NOT registered");
                    ImGui::TreePop();
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Interact
            if (ImGui::BeginTabItem("Interact"))
            {
                DrawInteractTab();
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ FOV
            if (ImGui::BeginTabItem("FOV"))
            {
                CheckboxInt("Override weapon FOV", s.fovEnabled);
                ImGui::BeginDisabled(!s.fovEnabled);
                ImGui::SliderFloat("Weapon FOV", &s.fov, 20.0f, 120.0f, "%.1f deg");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Vertical FOV used to render the weapon and arms.\nGame default is 55. Higher = smaller weapon.");
                ImGui::SameLine();
                if (ImGui::Button("55##fov"))
                    s.fov = PreyInternals::STOCK_WEAPON_FOV;
                ImGui::EndDisabled();
                ImGui::TextDisabled("The game's own zoom (and the aim camera zoom) still scales this proportionally.");
                ImGui::Separator();
                DrawReticleControls(s, "fret");
                if (gEnv && gEnv->pConsole)
                {
                    ICVar* pY = gEnv->pConsole->GetCVar("g_reticleYPercentage");
                    ICVar* pS = gEnv->pConsole->GetCVar("hud_reticleSetting");
                    ImGui::TextDisabled("g_reticleYPercentage %.3f | hud_reticleSetting %d", pY ? pY->GetFVal() : -1.0f, pS ? pS->GetIVal() : -1);
                }
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Nudge keys
            if (ImGui::BeginTabItem("Nudge keys"))
            {
                CheckboxInt("Enable nudge keys", s.nudgeKeys,
                    "Adjust the currently active pose without opening this window. The keys are blocked from the game while enabled.");
                const char* layouts[] = { "Numpad", "IJKL" };
                ImGui::Combo("Layout", &s.nudgeLayout, layouts, 2);
                const char* targets[] = { "Global standing / crouch (by stance)", "This weapon's hip offset" };
                ImGui::Combo("Not aiming edits", &s.nudgeTarget, targets, 2);
                const char* activeName = "";
                GetActivePose(&activeName);
                ImGui::Text("Active pose right now: %s", activeName);
                if (s.nudgeLayout == 0)
                    ImGui::TextWrapped(
                        "Numpad 4 / 6 : left / right\n"
                        "Numpad 8 / 2 : up / down\n"
                        "Numpad 7 / 1 : forward / back\n"
                        "Hold Numpad 0 : the same keys rotate instead (4/6 yaw, 8/2 pitch, 7/1 roll)\n"
                        "Hold Numpad . : slow (x0.2)");
                else
                    ImGui::TextWrapped(
                        "J / L : left / right\n"
                        "I / K : up / down\n"
                        "U / O : forward / back\n"
                        "Hold H : the same keys rotate instead (J/L yaw, I/K pitch, U/O roll)\n"
                        "Hold N : slow (x0.2)");
                ImGui::TextWrapped("While aiming (use Toggle aim) the keys edit the aim pose of the current weapon.");
                ImGui::SliderFloat("Position speed", &s.nudgePosSpeed, 0.005f, 0.3f, "%.3f m/s");
                ImGui::SliderFloat("Rotation speed", &s.nudgeRotSpeed, 1.0f, 90.0f, "%.0f deg/s");
                ImGui::EndTabItem();
            }

            //------------------------------------------------------------------ Options
            if (ImGui::BeginTabItem("Options"))
            {
                CheckboxInt("Capture mouse while this window is open", s.guiMouse,
                    "Shows the cursor and stops the camera from turning while you drag sliders.\nThe game keeps running. Close this window or press F1 to give the mouse back.");
                CheckboxInt("Show advanced and experimental features", s.showAdvanced,
                    "Diagnostics, self-tests and the experimental Death tab. Not needed for normal use.");
                if (ImGui::Button("Save weapon settings now"))
                    SaveWeapons();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", GetWeaponsPath().u8string().c_str());
                ImGui::TextWrapped("Console cvars: vm_pos_*, vm_rot_*, vm_crouch_*, vm_aim_*, vm_fov, vm_enabled, vm_nudge_*.");
                ImGui::TextDisabled("Global settings are saved by Chairloader; weapon settings a couple of seconds after a change.");

                if (s.showAdvanced && ImGui::CollapsingHeader("Pipeline diagnostics"))
                {
                    const PipelineDiag& d = m_diag;
                    ImGui::Text("Last frame: %d context update(s), %d weapon-offset update(s), instanceCount %d%s, modifier %s",
                        d.ctxUpdatesLastFrame, d.pwaUpdatesLastFrame, d.lastInstanceCount, d.lastForced ? " (forced alive)" : "",
                        d.lastModifierNull ? "NULL" : "ok");
                    ImGui::TextWrapped("Reproduce the problem (e.g. land from a jump with the pistol), then read the events below. Newest last.");
                    if (ImGui::Button("Clear events"))
                        m_diag.events.clear();
                    ImGui::BeginChild("diagevents", ImVec2(0, 160), true);
                    for (const auto& e : d.events)
                        ImGui::Text("[%8.2f] %s", e.time, e.text.c_str());
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::EndChild();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (!open)
        m_settings.showWindow = 0;
}

//---------------------------------------------------------------------------------
// Exported Functions
//---------------------------------------------------------------------------------
extern "C" DLL_EXPORT IChairloaderMod* ClMod_Initialize()
{
    CRY_ASSERT(!gMod);
    gMod = new ModMain();
    return gMod;
}

extern "C" DLL_EXPORT void ClMod_Shutdown()
{
    CRY_ASSERT(gMod);
    delete gMod;
    gMod = nullptr;
}

// Validate that declarations haven't changed
static_assert(std::is_same_v<decltype(ClMod_Initialize), IChairloaderMod::ProcInitialize>);
static_assert(std::is_same_v<decltype(ClMod_Shutdown), IChairloaderMod::ProcShutdown>);
