#pragma once
#include <Chairloader/ModSDK/ChairloaderModBase.h>
#include <Prey/CryInput/IInput.h>
#include <map>
#include <deque>
#include <string>
#include <vector>

class ArkPlayerCamera;
class CArkItem;
struct IEntity;
struct SViewParams;
class CArkWeapon;

//! A position (meters, view space: X right, Y forward, Z up) + rotation (degrees) offset.
struct PoseOffset
{
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float pitch = 0.0f; //!< Around the right axis (+ = muzzle up).
    float yaw = 0.0f;   //!< Around the up axis (+ = muzzle left).
    float roll = 0.0f;  //!< Around the forward axis.

    bool IsZero() const { return posX == 0 && posY == 0 && posZ == 0 && pitch == 0 && yaw == 0 && roll == 0; }
    void Reset() { *this = PoseOffset(); }
    Vec3 Pos() const { return Vec3(posX, posY, posZ); }
    Quat Rot() const { return Quat::CreateRotationXYZ(Ang3(DEG2RAD(pitch), DEG2RAD(roll), DEG2RAD(yaw))); }
    QuatT AsQuatT() const { return QuatT(Rot(), Pos()); }
    float& Axis(int i) { float* p[6] = { &posX, &posY, &posZ, &pitch, &yaw, &roll }; return *p[i]; }
    void AddScaled(const PoseOffset& o, float k)
    {
        posX += o.posX * k; posY += o.posY * k; posZ += o.posZ * k;
        pitch += o.pitch * k; yaw += o.yaw * k; roll += o.roll * k;
    }
    //! From a transform; a bad quaternion (not a number, not near unit length) leaves the pose unchanged.
    void FromQuatT(const QuatT& qt)
    {
        const float n2 = qt.q.w * qt.q.w + qt.q.v.x * qt.q.v.x + qt.q.v.y * qt.q.v.y + qt.q.v.z * qt.q.v.z;
        if (!(n2 > 0.25f && n2 < 4.0f) || !(fabsf(qt.t.x) < 10.0f && fabsf(qt.t.y) < 10.0f && fabsf(qt.t.z) < 10.0f))
            return;
        const Quat q = qt.q.GetNormalized();
        // Same convention as Ang3(Quat) (XYZ), with the asin argument clamped so float error cannot make a NaN.
        const float sy = clamp_tpl(-(q.v.x * q.v.z - q.w * q.v.y) * 2.0f, -1.0f, 1.0f);
        Ang3 a;
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
        posX = qt.t.x; posY = qt.t.y; posZ = qt.t.z;
        pitch = RAD2DEG(a.x); roll = RAD2DEG(a.y); yaw = RAD2DEG(a.z);
    }
    //! Replaces every non-finite field with the given fallback. Returns true if anything was fixed.
    bool Sanitize(const PoseOffset& fallback)
    {
        bool fixed = false;
        for (int i = 0; i < 6; i++)
        {
            float& v = Axis(i);
            if (!(fabsf(v) < 1e30f)) { v = const_cast<PoseOffset&>(fallback).Axis(i); fixed = true; }
        }
        return fixed;
    }
};

//! One procedural reach animation of the support hand (a "press" or a "grab"), all tunable.
//! Distances are meters in view space (X right, Y forward, Z up), times in seconds, angles in degrees.
struct ReachStyle
{
    // Defaults are the press as tuned in play (3.10.3).
    float reachTime = 0.20f;    //!< hand travels to the target
    float holdTime = 0.06f;     //!< stays there
    float returnTime = 0.30f;   //!< travels back to the animated pose
    float amount = 1.0f;        //!< fraction of the way to the target (1 = touch it, more overshoots)
    float offX = -0.0067f, offY = -0.0028f, offZ = -0.0013f;  //!< target offset (e.g. stop a little short of the surface)
    float arcX = -0.0027f, arcZ = 0.0378f;          //!< sideways / vertical bulge of the way out (sine, peaks half-way)
    float retArcX = 0.0f, retArcZ = 0.0f;           //!< the same for the way back
    float pitch = -2.07f, yaw = -2.07f, roll = -1.7f; //!< hand rotation at the target (additive, degrees)
    float envelopeScale = 1.0f; //!< the reach envelope's limits times this while this style plays (relax it for grabs down to the floor)
    float along = 0.0f;         //!< moves the target along the camera -> target line (m; + = beyond the object, into it)
    float windupTime = 0.0f;    //!< a keyframe before the reach: the hand is pulled to the windup offset first (0 = none)
    float windX = 0.0f, windY = 0.0f, windZ = 0.0f; //!< where it is pulled to (m, view space, relative to where it starts from)

    float Duration() const { return windupTime + reachTime + holdTime + returnTime; }
    float ApexTime() const { return max(windupTime, 0.0f) + max(reachTime, 0.0f); } //!< from the start to the hand at the target
    //! Quick melee: fast, straight ahead, snaps back.
    static ReachStyle Punch()
    {
        ReachStyle r;
        r.reachTime = 0.12f; r.holdTime = 0.04f; r.returnTime = 0.28f; r.amount = 1.0f;
        r.offX = r.offY = r.offZ = 0.0f; r.arcX = 0.0f; r.arcZ = -0.02f; r.retArcX = 0.0f; r.retArcZ = -0.04f;
        r.pitch = r.yaw = r.roll = 0.0f;
        r.windupTime = 0.14f; r.windX = 0.06f; r.windY = -0.14f; r.windZ = 0.03f; // the arm loads up: back, a little out and up
        return r;
    }
    //! The grab: a little slower, overshoots, sweeps in from the side and drops on the way back.
    static ReachStyle Grab()
    {
        ReachStyle r;
        r.reachTime = 0.25f; r.holdTime = 0.06f; r.returnTime = 0.30f; r.amount = 1.5f;
        r.offX = 0.0012f; r.offY = 0.0006f; r.offZ = -0.0006f;
        r.arcX = -0.0324f; r.arcZ = -0.03f; r.retArcX = 0.0f; r.retArcZ = -0.06f;
        r.pitch = -15.0f; r.yaw = 0.0f; r.roll = 20.0f;
        return r;
    }
    //! Softer variant for screens: slower in and out, a shorter way (the hand starts from the resting spot).
    static ReachStyle GentlePress()
    {
        ReachStyle r;
        r.reachTime = 0.31f; r.holdTime = 0.05f; r.returnTime = 0.29f;
        r.offX = 0.0f; r.offY = -0.03f; r.offZ = 0.0f; r.arcX = r.arcZ = 0.0f; r.pitch = r.yaw = r.roll = 0.0f;
        return r;
    }
};

//! Which animation an interaction gets, by what it is. First matching rule wins; no match = the built-in choice
//! (grab for pickups / consumables / carry / equip / examine and the loot mode, press otherwise).
struct InteractRule
{
    int type = -1;              //!< EArkInteractionType, -1 = any
    int mode = -1;              //!< EArkInteractionMode, -1 = any
    std::string classContains;  //!< entity class name contains this (case-insensitive), empty = any
    std::string textContains;   //!< the prompt text (m_displayText) contains this (case-insensitive), empty = any
    int style = 0;              //!< 0 press, 1 grab, 2 none (no animation, no hovering hand)
    int hover = 1;              //!< 0 = the hovering hand does not come up for this either
    std::string note;           //!< what it is for (shown in the list)
};

//! All user-tunable global settings. Backed by CVars flagged VF_DUMPTOCHAIR so Chairloader
//! persists them between sessions (Mods/config/Chairloader_CVars.xml) and they can
//! also be changed from the in-game console (vm_*).
struct ViewmodelSettings
{
    int enabled = 1;            //!< Master switch for position/rotation offsets.
    int bypass = 0;             //!< Vanilla viewmodel: switches off every viewmodel feature (offsets, ironsights, feel, weapon FOV,
                                //!< convergence, wall pull-back, spread) but keeps the reticle, world FOV and sprint sensitivity.

    PoseOffset base;            //!< Global standing offset, applied to all weapons.

    int   crouchEnabled = 1;    //!< Blend in the crouch pose while sneaking.
    PoseOffset crouch;          //!< Global crouch offset, added on top while crouched (all weapons).
    float crouchTime = 0.30f;   //!< Seconds for the stand<->crouch pose transition.

    int   aimEnabled = 1;       //!< Aim key / aim pose.
    int   aimKey = 0x101;       //!< EKeyId of the aim key (default eKI_Mouse2 = right mouse button).
    int   aimToggle = 0;        //!< 0 = hold to aim, 1 = press to toggle.
    int   aimConsumeKey = 1;    //!< Swallow the aim key so the game's own binding on it does nothing (vanilla RMB = select psi power).
    int   aimRenderLock = 1;    //!< 1 = place the weapon exactly at render time (after the camera is final), 0 = skeleton-only (approximate).
    int   aimHandsFollow = 1;   //!< Move the hand joints with the render-time placement so they stay on the grip.
    int   aimLeftHandFollow = 1;//!< Also move the support hand (when it is on the weapon).
    float aimBobAmount = 0.03f; //!< Head-bob coupling while locked (0 = perfectly still sights, 1 = weapon lags the head fully).
    float aimBobTau = 0.05f;    //!< Seconds; separates bob (fast) from posture/lean/crouch (slow).
    float aimAnimRecoil = 1.0f; //!< How much of the game's procedural recoil/bump offsets to keep while locked (0..1).
    float aimAnimSway = 0.0f;   //!< How much of the game's look/strafe sway to keep while locked (0..1).
    int   aimSensMode = 1;      //!< 0 = leave the game's zoom sensitivity scaling alone, 1 = match FOV zoom, 2 = custom multiplier.
    float aimSensScale = 0.85f; //!< Custom ADS look-sensitivity multiplier (mode 2).
    float aimTime = 0.18f;      //!< Seconds for the hip<->aim transition.
    int   aimIgnoresCrouch = 1; //!< Fade the crouch pose out while aiming.
    int   aimFovEnabled = 0;    //!< Use a different weapon FOV while aiming.
    float aimFov = 55.0f;       //!< Weapon FOV while aiming (before the game's zoom scaling).
    int   aimCameraZoom = 1;    //!< Zoom the camera (world FOV) while aiming, like ADS in modern shooters.
    float aimCameraZoomFactor = 0.85f; //!< Camera HFOV multiplier while aiming (1 = no zoom).

    int   nudgeKeys = 0;        //!< Nudge keys edit the currently active pose.
    int   nudgeLayout = 1;      //!< 0 = numpad, 1 = IJKL.
    int   nudgeTarget = 0;      //!< When not aiming: 0 = global standing/crouch (by stance), 1 = this weapon's hip offset.
    float nudgePosSpeed = 0.05f;//!< m/s
    float nudgeRotSpeed = 15.0f;//!< deg/s

    int   reticleMode = 1;      //!< Reticle position: 0 = leave the game alone, 1 = centered, 2 = custom (reticleY).
    float reticleY = 0.5f;      //!< g_reticleYPercentage for mode 2 (0 = top, 1 = bottom).
    int   reticleStyle = 0;     //!< hud_reticleSetting: 0 = leave the game alone, 1 = default reticle, 2 = simple dot, 3 = hidden.
    int   aimHideReticle = 1;   //!< Hide the reticle while aiming down sights.

    int   convergeEnabled = 1;      //!< Hip fire: rotate the weapon so the barrel converges on the crosshair's impact point.
    float convergeStrength = 0.75f; //!< 0..1
    float convergeMaxAngle = 30.0f; //!< degrees
    float convergeSmoothTime = 0.25f; //!< seconds (exponential smoothing time constant)
    float convergeMaxDist = 100.0f; //!< meters; no hit within this = parallel

    int   aimWallBlockEnabled = 1;  //!< Do not allow aiming while the weapon would be poking into a wall.
    int   aimBlockReload = 1;       //!< No aiming while the weapon reloads (the reload animation is not authored for the aim pose).
    int   aimBlockSwitch = 1;       //!< No aiming while a weapon is being holstered / drawn (select action in progress).
    float aimSwitchDelay = 0.75f;   //!< seconds after a weapon change before aiming is allowed again (the game reports the
                                    //!< weapon ready before its raise animation has finished).
    int   reloadFadesOffsets = 1;   //!< Fade the hip offsets (pose, convergence, pull-back, feel) out while reloading: the support
                                    //!< hand is animated in place during reloads and would otherwise miss a shifted weapon.
    float reloadFadeTime = 0.2f;    //!< seconds
    float aimWallBlockScale = 1.75f; //!< Tolerance multiplier on the blocking distance (higher = blocks further from the wall).

    int   wallPushEnabled = 1;      //!< Pull the weapon back towards the camera when the view ray hits something close.
    float wallPushStartDist = 1.2f; //!< meters from the camera where the pull-back starts
    float wallPushFullDist = 0.45f; //!< meters where the full per-weapon pull-back is reached
    float wallPushSmoothTime = 0.15f; //!< seconds (exponential smoothing)
    int   wallPushWhileAiming = 0;  //!< keep pulling back while aiming (the sights stay on the view axis)
    int   wallPoseEnabled = 1;      //!< blend each weapon's "near wall" pose in as the pull-back builds up
    float wallPoseStart = 0.10f;    //!< fraction of the weapon's full pull-back where the pose starts blending in
    float wallPoseFull = 0.85f;     //!< fraction where the pose is fully applied
    float wallPoseConvergeFade = 1.0f; //!< how much the hip convergence fades out as the wall pose comes in (0 none, 1 fully)
    float wallPosePitchStart = 20.0f;  //!< camera pitch (deg up or down) where the pose starts giving way to the plain pull-back
    float wallPosePitchFull = 50.0f;   //!< pitch where the fade is complete
    float wallPosePitchStrength = 1.0f; //!< how much of the pose is removed at full pitch (1 = all, 0 = the pitch fade is off)


    // --- Interaction animation (procedural left-hand reach) ---------------------------------------
    int   interactEnabled = 1;      //!< Reach out with the support hand when interacting (buttons, pickups, ...).
    int   interactDefer = 1;        //!< Delay the game's interaction until the hand is about to arrive.
    float interactFireDelay = 0.15f;//!< seconds from the key press to the actual interaction (when deferring)
    int   interactCancelRetarget = 0; //!< Drop the deferred interaction if the crosshair moved to another object meanwhile.
    int   interactTargetMode = 0;   //!< 0 = the point the crosshair ray hits on the object (fallback: object centre), 1 = object centre, 2 = fixed point ahead
    int   interactWhileAiming = 1;  //!< Also animate while aiming down sights (default: the sights win).
    int   interactForceLeftIk = 1;  //!< Force the left arm's IK weight to 1 during the reach (one-handed stances otherwise ignore the target).
    int   interactRotate = 1;       //!< Push the style's hand rotation too (the rig may or may not honour it).
    float interactCorrGain = 0.5f;  //!< closed-loop hand position correction per frame (0 = off)
    int   interactUnarmed = 1;      //!< Also animate with no weapon out (the arms are shown for the reach; the game hides them).
    int   interactExamination = 1;  //!< Also animate clicks on in-world screens / keypads (examination mode), towards the cursor.
    int   interactExamKey = 0x100;  //!< EKeyId that counts as a click on a screen (default eKI_Mouse1)
    int   interactExamKey2 = 0x11;  //!< second one (default eKI_E)
    PoseOffset examCorr;            //!< extra hand correction applied only while on a screen (m, deg) - big ranges, for tuning
    float interactExamBodyX = 0.0f, interactExamBodyY = 0.0f, interactExamBodyZ = 0.0f; //!< on screens: move the whole arms/torso relative to the camera (view space, m) - brings the shoulder within reach
    int   interactExamAutoBody = 0; //!< on screens: bring the body forward automatically when the wrist would be beyond the arm's length
    float interactExamArmLength = 0.34f; //!< shoulder-to-wrist distance the auto body offset keeps (m)
    float interactExamMinTargetDist = 0.0f; //!< on screens: targets closer than this to the camera (keypads you are nose-to-nose with) are not reached for
    int   interactExamFovMode = 0;  //!< camera FOV while on a screen: 0 = the game's zoom, 1 = no zoom (regular cl_hfov), 2 = custom (interactExamFov)
    float interactExamFov = 60.0f;  //!< custom horizontal FOV on screens (deg)
    int   interactDebugMarker = 0;  //!< draw the reach target (red) and the asked hand position (green) in the world
    int   interactExamRest = 1;     //!< On screens: keep the arms shown and hold the pointing hand at a resting spot between clicks.
    float interactRestX = -0.0585f, interactRestY = 0.0433f, interactRestZ = -0.0698f; //!< resting spot outside screens (view space, m)
    float interactRestExamX = -0.0697f, interactRestExamY = 0.0757f, interactRestExamZ = -0.0901f; //!< resting spot on screens (view space, m)
    int   interactHoverWhileAiming = 0;  //!< hover hand also while aiming down sights (off: the support hand stays on the gun)
    int   interactIkWeightRamp = 1;      //!< blend the left arm's IK weight in with the reach / rest instead of forcing 1 (one-handed weapons animate it at 0: forcing snaps the hand)
    float interactRestBlendTime = 0.20f; //!< seconds to settle into / out of the resting spot
    float interactExamLeaveTime = 0.4f;  //!< seconds over which everything screen-specific (body shift, screen corrections, limits) fades when leaving a screen
    float interactRestSwayPos = 0.0034f;  //!< resting hand: slow drift amplitude (m; vertical 60 % of it, forward 30 %)
    float interactRestSwayRot = 1.6f;    //!< resting hand: slow drift amplitude (degrees)
    float interactRestSwayFreq = 0.15f;  //!< Hz of the slow axis
    int   interactHoverRest = 1;         //!< Outside screens too: hold the resting hand up while looking at something usable within reach
    int   interactHoverTypeMask = 0x1FFA;//!< which interaction types (EArkInteractionType bits) bring the hand up
    float interactHoverMaxDist = 2.0f;   //!< only when the usable thing is closer than this (m, from the camera)
    float interactHoverTowards = 0.20f;  //!< 0 = plain resting spot, 1 = the hand hovers right at the target (pose offset included)
    int   interactHoverUnarmed = 1;      //!< also with no weapon out (the arms are shown for it)
    int   interactHoverWeapon = 1;       //!< also with a weapon out (the support hand leaves the grip)
    int   interactNoZoomKeypad = 1;      //!< use keypads from where you stand: no automatic zoom-in (the game's ui_examine_keypad)
    int   interactNoZoomFabricator = 1;  //!< the same for fabricators (ui_examine_fabricator)
    int   interactNoZoomSecurity = 1;    //!< ... security stations (ui_examine_securitystation)
    int   interactNoZoomWorkstation = 1; //!< ... workstations (ui_examine_workstation)
    int   interactExamGentle = 1;        //!< on screens the press uses its own, gentler style (pressExam)
    int   interactShowArms = 1;     //!< Un-hide the arms while a reach plays in a state where the game hides them (unarmed, screens).
    int   interactWristMode = 2;    //!< how a pose's wrist orientation is applied: 0 = on the IK target joint, 1 = on the hand joint (relative to the forearm), 2 = both
    int   interactEaseIn = 4;       //!< reach easing: 0 linear, 1 smooth, 2 ease out, 3 ease in, 4 ease in-out
    int   interactEaseOut = 2;      //!< return easing (same list)
    int   interactTypeMask = 0x1FFF & ~((1 << 0) | (1 << 2) | (1 << 12)); //!< EArkInteractionType bits that animate (default: all but none/unavailable/hoover)
    int   interactRemoteMode = 0;   //!< Animate the remote-manipulation (psi) mode too.
    float interactMaxForward = 0.75f; //!< reach envelope, view space (m): furthest the wrist goes forward
    float interactExamMaxForward = 1.0f; //!< the same on in-world screens (the arms are moved to the examination camera, the arm has its full length)
    float interactMinForward = 0.12f; //!< ... nearest
    float interactMaxSide = 0.40f;    //!< ... left / right of the eye
    float interactMaxUp = 0.30f;      //!< ... above the eye
    float interactMaxDown = 0.80f;    //!< ... below the eye
    float interactTestX = 0.0f, interactTestY = 0.45f, interactTestZ = -0.08f; //!< fixed test point (view space, m)
    ReachStyle press;               //!< buttons, switches, terminals, hacking, repairs (scriptDefined / codeDefined / hack / repair / fortify)
    ReachStyle grab = ReachStyle::Grab(); //!< pickups, loot, consumables, carry, equip, examine
    int   interactGrabAtApex = 1;   //!< grab-style interactions (and carrying) fire at the end of the reach instead of after interactFireDelay
    int   interactCarryAtApex = 1;  //!< carrying starts when the hand gets there (the game's own carry delay is lengthened to the reach)
    ReachStyle pressExam = ReachStyle::GentlePress(); //!< the press while a screen / keypad is up (close to it, from the resting hand)
    ReachStyle punch = ReachStyle::Punch(); //!< quick melee (style 2)
    int   meleeEnabled = 1;         //!< quick melee on a key: the punch plays and a wrench hit lands at its apex
    int   meleeKey = 0x2E;          //!< EKeyId (default eKI_V)
    int   meleeConsumeKey = 1;      //!< swallow the key so the game's own binding on it does nothing
    float meleeDamage = 0.5f;       //!< damage scale relative to a wrench hit
    float meleeCooldown = 1.0f;     //!< seconds between punches
    float meleeCamKick = 1.5f;      //!< camera pitch kick (degrees, down then back) with the punch
    float meleeCamKickYaw = 0.6f;   //!< ... and yaw (degrees, to the right then back)
    float meleeCamKickTime = 0.28f; //!< seconds for the kick to come and go
    int   meleeSound = 1;           //!< play a swing sound (vm_melee_sound_name) when the punch starts
    int   meleeWhileAiming = 0;     //!< allow while aiming down sights
    float interactCarryHoldTime = 0.15f; //!< carrying: the key has to be held this long before the grab starts (a tap does nothing)
    int   interactHiddenStart = 1;  //!< when the support hand is not on the weapon (one-handed weapons, no weapon), the hand comes up from a fixed spot below the view instead of from wherever the animation has it
    float interactStartX = -0.15f, interactStartY = 0.35f, interactStartZ = -0.55f; //!< that spot (view space, m)

    int   worldFovEnabled = 0;  //!< Override the game's horizontal FOV (cl_hfov).
    float worldFov = 85.0f;     //!< Horizontal FOV in degrees when worldFovEnabled.
    int   sprintSensEnabled = 0;//!< Override the look-sensitivity scale the game applies while sprinting.
    float sprintSensScale = 1.0f; //!< 1 = same sensitivity as walking.

    // --- Feel: sprint pose --------------------------------------------------------------------
    int   sprintPoseEnabled = 1;    //!< Lower / tilt the weapon while sprinting.
    PoseOffset sprint;              //!< Sprint offset (added to standing / crouch, all weapons). Set in ctor below.
    float sprintBlendIn = 0.29f;    //!< seconds to reach the sprint pose
    float sprintBlendOut = 0.20f;   //!< seconds to return from it
    int   sprintBlocksAim = 1;      //!< No aiming down sights while sprinting (the aim resumes when you stop).
    int   sprintInZeroG = 0;        //!< Treat the zero-G thruster boost as sprinting too (pose, sway, aim block). Off: nothing happens in zero-G.
    int   sprintSwayEnabled = 1;    //!< Extra procedural sway while sprinting (on top of the game's own).
    float sprintSwayPos = 0.012f;   //!< meters (side to side; the vertical part is 60 % of it, twice the rate)
    float sprintSwayRot = 1.5f;     //!< degrees of roll (pitch is 40 % of it)
    float sprintSwayFreq = 2.3f;    //!< Hz (one full side-to-side cycle = two steps)

    // --- Feel: sway and settle while aiming ------------------------------------------------------
    int   aimSwayEnabled = 1;       //!< Slow figure-eight sway of the sights while aiming.
    float aimSwayPos = 0.0005f;     //!< meters at rest
    float aimSwayRot = 0.05f;       //!< degrees at rest (this is what moves the sights off the crosshair)
    float aimSwayFreq = 0.14f;      //!< Hz of the slow axis
    float aimSwayInitial = 1.5f;    //!< amplitude multiplier at the moment the sights come up ...
    float aimSwaySettleTime = 0.1f; //!< ... decaying to 1 with this time constant (seconds)
    float aimSwayMoveMult = 2.0f;   //!< extra amplitude at walking speed (added: 1 + mult * speed/walk)
    float aimSwaySprintPenalty = 2.0f; //!< extra amplitude right after sprinting ...
    float aimSwaySprintRecover = 1.25f; //!< ... decaying with this time constant (seconds)
    int   steadyEnabled = 0;        //!< Hold a key to steady the sights.
    int   steadyKey = 41;           //!< EKeyId (0 = none); 41 = left Alt
    float steadyReduce = 0.85f;     //!< sway reduction while steady (0..1)
    float steadyDuration = 4.0f;    //!< seconds you can hold your breath
    float steadyRecover = 3.0f;     //!< seconds to recover a full breath
    int   steadyConsumeKey = 0;

    // --- Feel: view drag (GoldenEye style) --------------------------------------------------------
    int   dragEnabled = 1;          //!< The weapon follows the camera with a spring: turning drags it along / behind.
    int   dragLead = 0;             //!< 1 = the weapon leads into the turn (GoldenEye), 0 = it lags behind.
    float dragPos = 0.007f;         //!< meters of offset per rad/s of turn rate
    float dragRot = 1.5f;           //!< degrees of rotation per rad/s of turn rate
    float dragStiffness = 9.0f;     //!< spring stiffness (how quickly it reacts and returns)
    float dragDamping = 0.85f;      //!< damping ratio (1 = no overshoot, lower = a little wobble)
    float dragMaxPos = 0.035f;      //!< meters
    float dragMaxRot = 5.0f;        //!< degrees
    float dragAimScale = 0.05f;     //!< how much of it survives while aiming (0..1)
    float dragPitchScale = 0.7f;    //!< pitch (look up/down) relative to yaw

    int   fovEnabled = 1;       //!< Master switch for the weapon (near-scene) FOV override.
    float fov = 55.0f;          //!< Weapon FOV in degrees. Stock game uses 55.

    int   showWindow = 1;       //!< Whether the ImGui window is open when the Chairloader GUI is visible.
    int   spreadDebug = 0;      //!< Log the full spread picture (cone, dispersion, aim offset) on every shotgun/pistol shot.
    int   showAdvanced = 0;     //!< Show diagnostics, self-tests and experimental features (Death tab).
    int   guiMouse = 1;         //!< Show the cursor / block player look input while the window is open (game keeps running).

    // Not persisted: render-time placement self-test (moves the weapon/hands by this much, always).
    float testOffsetUp = 0.0f;  //!< meters
    int   testHands = 1;

    ViewmodelSettings()
    {
        sprint.posX = 0.02f; sprint.posY = -0.05f; sprint.posZ = -0.0671f;
        sprint.pitch = -6.76f; sprint.yaw = 9.57f; sprint.roll = 10.0f;
        // A grab comes in a touch slower, from slightly below and outside, and sweeps back towards the body.
        grab.reachTime = 0.26f; grab.holdTime = 0.06f; grab.returnTime = 0.34f;
        grab.offY = -0.03f; grab.offZ = 0.01f;
        grab.arcX = -0.04f; grab.arcZ = -0.03f;
        grab.retArcX = -0.05f; grab.retArcZ = -0.06f;
        grab.pitch = -15.0f; grab.roll = 20.0f;
        // A press pokes straight in with the fist turned a little.
        press.offY = -0.06f;
        press.arcZ = 0.02f;
        press.pitch = -10.0f;
    }
};

//! One joint of a hand pose: the joint's rotation relative to its parent, as an adjustment (degrees) on top of the
//! skeleton's bind pose. The bind pose is the same whatever weapon is held, so the pose is absolute.
struct HandPoseJoint
{
    std::string name;
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
    Quat Adjust() const { return Quat::CreateRotationXYZ(Ang3(DEG2RAD(pitch), DEG2RAD(roll), DEG2RAD(yaw))); }
    bool Adjusted() const { return pitch != 0.0f || yaw != 0.0f || roll != 0.0f; }
};

//! A full support-hand pose: wrist offset (view space) and orientation (absolute, view space) plus every joint
//! under the hand. Stored in Vee.ViewmodelTweaks.poses.xml.
struct HandPose
{
    std::string name;
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;        //!< wrist offset from the reach target (m, view space) - or the wrist position itself when absolutePos
    int absolutePos = 0;                                 //!< 1 = the hand goes to pos (view space) regardless of the object; 0 = pos is an offset from the object point
    float handPitch = 0.0f, handYaw = 0.0f, handRoll = 0.0f; //!< wrist orientation in view space (degrees)
    std::vector<HandPoseJoint> joints;

    HandPoseJoint* Find(const char* jointName)
    {
        for (HandPoseJoint& j : joints)
            if (j.name == jointName) return &j;
        return nullptr;
    }
    const HandPoseJoint* Find(const char* jointName) const
    {
        for (const HandPoseJoint& j : joints)
            if (j.name == jointName) return &j;
        return nullptr;
    }
    Quat HandRotView() const { return Quat::CreateRotationXYZ(Ang3(DEG2RAD(handPitch), DEG2RAD(handRoll), DEG2RAD(handYaw))); }
    Vec3 Pos() const { return Vec3(posX, posY, posZ); }
};

//! Runtime state of the interaction reach (support hand), see ModMain::OnInteract / PushInteractReach.
struct InteractState
{
    enum EPhase { Idle, Reach, Hold, Return, Windup };
    EPhase phase = Idle;
    float time = 0.0f;              //!< seconds into the animation
    int style = 0;                  //!< 0 press, 1 grab
    float curve = 0.0f;             //!< 0..1 progress towards the target this frame (after easing)
    float arc = 0.0f;               //!< sine bulge weight this frame (0..1), way out or way back
    bool returning = false;
    bool hasWorldTarget = false;
    Vec3 targetWorld = Vec3(ZERO);  //!< the point the hand goes to (world), when hasWorldTarget
    Vec3 targetView = Vec3(ZERO);   //!< ... in view space this frame (debug / fixed target)
    Vec3 handView = Vec3(ZERO);     //!< animated support hand this frame (view space, debug)
    Vec3 desiredView = Vec3(ZERO);  //!< where we put it (debug)
    bool clamped = false;           //!< the target was outside the reach envelope

    // Additive chain on the left IK target joint (same bookkeeping as the aim lock)
    bool addValid = false;
    QuatT lastAdd = QuatT(IDENTITY);
    QuatT animIk = QuatT(IDENTITY);
    Vec3 finalPrev = Vec3(ZERO);    //!< last frame's final IK target joint as read back (detects a skeleton that did not update)
    bool finalPrevValid = false;
    bool chainJustReset = false;    //!< last frame reset the chain: this frame's reconstruction is the new baseline
    int pushesNotApplied = 0;       //!< debug: frames where the read-back did not fit "animation + what we pushed" -> chain reset
    int chainResets = 0;            //!< debug: those resets (one frame without the reach each)
    int pushes = 0;
    int weightJoint = -1;           //!< "l_hand_spine_blend" (ADIK weight of the left arm), -1 = not found
    int weightJointParent = -1;
    float animIkWeight = 1.0f;      //!< the animated IK weight (the weight joint's relative x), captured on the first frame we push
    bool animIkWeightValid = false;
    float ikWeightPushed = 0.0f;    //!< what we pushed last frame (debug)
    std::string weaponSeen;         //!< weapon class the capture belongs to
    std::string weightJointName;

    // Deferred game interaction
    bool pending = false;
    void* pInteraction = nullptr;   //!< ArkPlayerInteraction the call belongs to
    int mode = 0;
    unsigned entityId = 0;
    float fireIn = 0.0f;            //!< seconds until the original call is made
    bool fireNow = false;

    // Hand pose (wrist orientation + finger overrides on the left hand subtree)
    int holdMode = 0;               //!< posing mode: 0 off, 1 hold the pose on the animated hand, 2 hold the pose and the reach to the test point
    bool poseApplied = false;       //!< overrides were pushed last frame (the final pose is then ours, keep the captured animation)
    std::vector<Quat> animRel;      //!< animated parent-relative rotations of the subtree joints, captured when the pose started blending in
    Quat animHandAbs = Quat(IDENTITY); //!< animated hand orientation (model space) at the same moment
    Quat animIkQ = Quat(IDENTITY);  //!< animated orientation of the left IK target joint at the same moment
    bool animValid = false;
    Vec3 poseOffsetView = Vec3(ZERO); //!< wrist offset of the active pose this frame, already weighted (added to the reach target)
    bool poseAbsolutePos = false;   //!< the active pose dictates the hand position (view space) instead of the object point
    Vec3 poseAbsView = Vec3(ZERO);
    // Closed-loop position correction: the IK target is pushed, but what must land on the point is the hand joint
    // (the IK effector may sit elsewhere on the hand, the IK weight may be partial): the error measured on last
    // frame's final pose is integrated into the push, so the hand ends exactly where asked whatever the weapon.
    Vec3 corr = Vec3(ZERO);         //!< accumulated correction (view space, m)
    Vec3 desiredPrev = Vec3(ZERO);  //!< where the hand was asked to be last frame (view space)
    bool desiredPrevValid = false;
    QuatT camReachPrev = QuatT(IDENTITY); //!< the camera (model space) that push was computed in; the read-back is measured in it
    float corrError = 0.0f;         //!< debug: last measured error (m)
    Vec3 handActualView = Vec3(ZERO); //!< debug: where the hand joint really ended up last frame (view space, real camera)
    bool handActualValid = false;
    Vec3 shoulderView = Vec3(ZERO);   //!< debug: the left upper-arm joint (shoulder) as seen (view space, real camera)
    bool shoulderValid = false;
    float autoBodyY = 0.0f;         //!< automatic body-forward offset on screens (m), smoothed
    bool examTooClose = false;      //!< the screen point is closer than the minimum: no reach
    int poseJointsPushed = 0;       //!< debug
    std::string poseName;           //!< pose used this frame (debug)
    float poseWeight = 0.0f;

    // Arms in states where the game hides them (no weapon, in-world screens)
    bool armsForced = false;        //!< we set the arms slot's render flag
    unsigned savedSlotFlags = 0;    //!< what it was before
    int armsForcedWhile = 0;        //!< 1 = the arms were hidden by a screen (examination) when we forced them on, 2 = by being unarmed
    bool examining = false;         //!< in-world UI examination mode (screens, keypads) this frame
    bool unarmed = false;           //!< no weapon out this frame
    Vec3 examCursorWorld = Vec3(ZERO); //!< where the last screen click's centre ray hit (debug)
    bool examCursorValid = false;
    float markerTimer = 0.0f;       //!< seconds left to draw the debug markers after a click
    unsigned slotFlagsNow = 0;      //!< debug
    // Examination mode moves the camera away from the head while the arms stay with the body: the body is
    // brought along with a root-joint additive (bodyShift, see PushInteractReach).
    Vec3 bodyShift = Vec3(ZERO);    //!< this frame's root push (model space), for the read-outs
    bool bodyShiftActive = false;
    float restBlend = 0.0f;         //!< 0..1: the hand is held at the resting spot (in-world screens, or hovering over something usable)
    bool restActive = false;
    bool hoverActive = false;       //!< the resting hand is up because of something usable in front of us (not a screen)
    bool hoverTargetValid = false;
    Vec3 hoverWorld = Vec3(ZERO);   //!< the usable thing's point (world), refreshed every frame while hovering
    int hoverType = -1;             //!< its interaction type (debug)
    float hoverDist = 0.0f;         //!< camera to it (m, debug)
    Vec3 restPointView = Vec3(ZERO);//!< where the resting hand is being held (view space), smoothed so screen <-> hover hand-overs do not pop
    bool restPointValid = false;
    float examBlend = 0.0f;         //!< 0..1: the screen-specific pieces (body shift, corrections, limits); fades out over interactExamLeaveTime
    float swayTime = 0.0f;          //!< seconds, drives the resting hand's drift
    float frameDt = 0.0f;           //!< this frame's dt (for the smoothing done at push time)
    Vec3 swayPos = Vec3(ZERO);      //!< this frame's drift (view space, m)
    Quat swayRot = Quat(IDENTITY);  //!< this frame's drift (view space)
    Vec3 lastReach = Vec3(ZERO);    //!< last frame's reach part of the additive (without the body shift), model space

    // Debug
    // Carrying (see OnPerformCarry): the grab starts once the key has been held, the game's carry delay ends at its apex
    bool hiddenStartUsed = false;   //!< this frame the hand comes up from the fixed spot (support hand not on the weapon)
    float wind = 0.0f;              //!< 0..1: the windup keyframe (hand pulled to the style's windup offset); fades out during the reach
    // Quick melee
    bool meleePending = false;      //!< a punch is playing and its hit has not landed yet
    float meleeCooldownLeft = 0.0f;
    float meleeKickTime = -1.0f;    //!< seconds into the camera kick, < 0 = none
    int meleePunches = 0, meleeHits = 0, meleeNoWrench = 0; //!< debug
    bool meleeSoundWarned = false;
    bool carryPending = false;      //!< waiting for the hold time before the grab starts
    float carryStartIn = 0.0f;      //!< seconds until it does
    unsigned carryEntityId = 0;     //!< what is being picked up (looked up again when the grab starts)
    int carryStyle = 1;             //!< the style the rules gave it
    bool carryAnimating = false;    //!< the grab is playing for a carry
    bool carryStarted = false;      //!< StartCarrying happened (our hook)
    int carryCancelled = 0;         //!< debug: carries the key was released on
    int lastType = -1, lastMode = -1;
    std::string lastEntity;         //!< class 'name' of the last interaction's entity (debug / rule editor)
    std::string lastClass, lastText;//!< its class and prompt text, for "add a rule from the last interaction"
    int started = 0, deferred = 0, fired = 0, dropped = 0, skipped = 0;
    std::string skipReason;
};

//! Per-frame state of the procedural "feel" layer (sprint pose, aim sway, view drag).
struct FeelState
{
    bool sprinting = false;         //!< sprinting for our purposes (already excludes zero-G unless allowed)
    bool zeroG = false;             //!< player is in zero-G / grav-shaft movement
    float sprintBlend = 0.0f;       //!< 0..1
    float timeSinceSprint = 1e9f;   //!< seconds since sprinting stopped
    float sprintPhase = 0.0f;       //!< radians
    float speed = 0.0f;             //!< horizontal speed (m/s)
    float speedNorm = 0.0f;         //!< speed / walking speed, clamped 0..1.5

    float aimTime = 0.0f;           //!< seconds since the sights started coming up (0 while not aiming)
    float swayPhase = 0.0f;         //!< radians
    float swayAmplitude = 0.0f;     //!< debug: current multiplier on the rest amplitude
    bool steadyHeld = false;
    float steadyLeft = 0.0f;        //!< seconds of breath left (init from settings)
    bool steadyActive = false;

    // View drag: spring state, x = yaw axis, y = pitch axis (units: rad/s of "felt" turn rate)
    float dragX = 0.0f, dragY = 0.0f, dragVX = 0.0f, dragVY = 0.0f;
    float lastYaw = 0.0f, lastPitch = 0.0f; bool lastLookValid = false;
    float yawRate = 0.0f, pitchRate = 0.0f; //!< debug (rad/s)

    // Outputs of this frame (view space: x right, y forward, z up; degrees)
    PoseOffset sprintOut;           //!< sprint pose + sprint sway, already scaled by the sprint blend
    PoseOffset dragHipOut;          //!< view drag while not aiming
    PoseOffset dragAimOut;          //!< view drag while aiming (scaled)
    PoseOffset swayOut;             //!< aim sway (unscaled by the aim blend; the blend happens in the pipeline)
};

//! Per-weapon settings (keyed by entity class name, stored in Vee.ViewmodelTweaks.weapons.xml).
struct WeaponSettings
{
    PoseOffset hip;         //!< Additive offset while not aiming, on top of the global standing/crouch offsets.
    PoseOffset aim;         //!< Ironsight pose: weapon (attachment) relative to the camera while aiming.
    bool aimAllowed = true; //!< Whether the aim key does anything with this weapon.
    float wallPush = 0.06f; //!< How far (m) this weapon moves back towards the camera against a wall (longer weapon = more).
    PoseOffset wall;        //!< "Near wall" pose, blended in on top of the hip offset as the pull-back builds up (e.g. muzzle up).
    float wallPoseAmount = 1.0f;    //!< Multiplier on the wall pose blend for this weapon (0 = never).
    float fireCoupling = 0.35f;     //!< Head-bob coupling right after a shot (lets the fire kick show while aiming).
    float fireCouplingTime = 0.30f; //!< Seconds the fire coupling decays over.
    float aimRecoilScale = 1.0f;    //!< Multiplier on the game's procedural recoil/bump offsets while aiming.
    float aimKickScale = 0.3f;      //!< Multiplier on the fire *animation's* weapon kick while aiming (the hand animation, not the procedural offset).
    float aimSpreadMult = 1.0f;     //!< Per-weapon spread multiplier while aiming (times the global one).
    float hipSpreadMult = 1.0f;     //!< Per-weapon spread multiplier while not aiming (times the global one).
    PoseOffset interact;            //!< Interaction reach correction for this weapon: hand position offset (m, view space) and wrist rotation (deg), on top of the pose.
    PoseOffset interactRest;        //!< Where this weapon's resting / hovering hand waits, relative to the resting spot (position, m, view space; rotation unused).
    PoseOffset interactForearm;     //!< Extra rotation of the left forearm (about its own axes, degrees) while the hand is posed; the hand keeps its own orientation. Position unused.
    PoseOffset interactStart;       //!< This weapon's own "hand comes up from here" spot (view space, m), replaces the global one when non-zero. Rotation unused.
    int interactHandOff = 2;        //!< Is the support hand off the weapon (animated out of view)? 0 no, 1 yes, 2 auto (from the left IK weight and where the animated hand is). Decides whether the hand comes up from the hidden spot.
    bool valid = false;     //!< Has been touched by the user (only valid entries are saved).

    static PoseOffset DefaultAim()
    {
        PoseOffset p;
        p.posX = 0.0f; p.posY = 0.25f; p.posZ = -0.06f;
        return p;
    }

    //! Built-in tuned settings for the stock weapons (used until the user changes a weapon).
    static const WeaponSettings* BuiltIn(const char* weaponClass);
};

//! Runtime state of the skeleton-side ironsight lock (pushed before the animation is evaluated).
struct AimLockState
{
    bool active = false;            //!< We are (or are blending) overriding the IK joint this frame.
    bool captured = false;          //!< hipRel / weaponRel captured for this aim session.
    QuatT hipRel = QuatT(IDENTITY);      //!< IK joint relative to camera at the moment aiming started.
    QuatT weaponRel = QuatT(IDENTITY);   //!< Weapon bone relative to the IK joint (assumed rigid).
    QuatT lastTarget = QuatT(IDENTITY);  //!< Last override we pushed (debug).
    std::string weaponClass;        //!< Weapon class the capture belongs to.

    // Debug / measurements (model space)
    QuatT camAbs = QuatT(IDENTITY);      //!< Predicted camera used for this frame's push.
    QuatT ikAbs = QuatT(IDENTITY);
    QuatT weaponAbs = QuatT(IDENTITY);
    bool charMatches = false;
    int rightIkJoint = -1;
    int leftIkJoint = -1;
    int weaponJoint = -1;
    float ikErr = 0.0f;             //!< |IK joint (prev frame) - target pushed prev frame| (m)

    // Animation pass-through. The IK joint is driven additively: final = animated (+) add. Knowing what we
    // added last frame gives the animated (pre-modifier) hand of last frame, and its deviation from a slow
    // reference is the fire animation's kick, which the lock would otherwise swallow.
    bool addValid = false;
    QuatT lastAdd = QuatT(IDENTITY);        //!< additive pushed last frame (model space: t added, q pre-multiplied)
    QuatT animIk = QuatT(IDENTITY);         //!< reconstructed animated IK joint of last frame (model space)
    QuatT animRelCam = QuatT(IDENTITY);     //!< the same, relative to that frame's camera (live hip reference for the blend)
    bool animRelCamValid = false;
    bool animRestValid = false;
    QuatT animRestRelCam = QuatT(IDENTITY); //!< slow reference of the animated hand relative to the camera
    QuatT kick = QuatT(IDENTITY);           //!< this frame's kick (hand-local), already scaled and gated
    float kickPos = 0.0f, kickRot = 0.0f;   //!< debug: magnitude of the raw deviation
    int pushesNotApplied = 0;               //!< debug: frames where the skeleton did not apply our additive
};

//! Runtime state of the render-time placement (runs after ArkPlayerCamera::UpdateView, when the
//! camera of this frame is final and the skeleton has been evaluated).
struct RenderLockState
{
    int callsThisFrame = 0;
    int callsLastFrame = 0;
    int frameOfLastCall = -1;
    bool positionWasWorld = false;  //!< SViewParams::position came as world position instead of entity-relative.

    // Exact camera of the last UpdateView, used by the skeleton-side push next frame.
    bool camValid = false;
    Quat camWorldRot = Quat(IDENTITY);
    Vec3 camModelPos = Vec3(ZERO);
    Quat freshRotAtCam = Quat(IDENTITY); //!< ArkPlayerCamera::m_rotation when camWorldRot was captured.
    QuatT camModel = QuatT(IDENTITY);   //!< camera in model (entity) space, this frame

    // Weapon attachment
    bool attachValid = false;
    int attachJoint = -1;
    std::string attachJointName;
    QuatT weaponModelGame = QuatT(IDENTITY);  //!< attachment transform as the game computed it (model space)
    QuatT weaponBoneGame = QuatT(IDENTITY);   //!< weapon bone (final pose, before our hand edits) this frame
    QuatT ikGame = QuatT(IDENTITY);           //!< right IK joint (final pose, before our hand edits) this frame
    bool ikGameValid = false;
    QuatT attOffset = QuatT(IDENTITY);        //!< weapon bone -> attachment (constant, from the game's values)
    bool attOffsetValid = false;
    QuatT weaponRelCam = QuatT(IDENTITY);     //!< ... relative to the exact camera
    QuatT hipRelCam = QuatT(IDENTITY);        //!< last weaponRelCam measured while not aiming (exact hip pose)
    bool hipValid = false;
    QuatT desiredModel = QuatT(IDENTITY);

    // Residual the render-time step had to fix (how good the skeleton-side prediction was; cosmetic only).
    float residualPos = 0.0f;       //!< m
    float residualRot = 0.0f;       //!< rad
    float residualPosMax1s = 0.0f, residualPosAcc = 0.0f, residualTimer = 0.0f;

    // Head bob (camera motion minus its slow component), camera frame.
    bool bobValid = false;
    Vec3 camLowPass = Vec3(ZERO);
    Vec3 bobCam = Vec3(ZERO);

    // Skeleton hierarchy cache
    void* skeleton = nullptr;
    int jointCount = 0;
    int rightHand = -1, leftHand = -1;
    std::string rightHandName, leftHandName;
    std::vector<int> rightSubtree, leftSubtree;
    std::vector<std::string> leftSubtreeNames;  //!< parallel to leftSubtree
    std::vector<int> leftSubtreeParent;         //!< parallel to leftSubtree (parent joint id)
    std::vector<Quat> leftSubtreeDefaultRel;    //!< parallel to leftSubtree: bind-pose rotation relative to the parent
    int leftUpperArm = -1;                      //!< the left hand's grandparent (the joint at the shoulder)
    bool defaultPoseValid = false;              //!< the bind pose accessors were found and verified on this skeleton
    int defaultRelSlot = -1, defaultAbsSlot = -1;
    bool leftOnWeapon = false;
    float leftHandDist = 0.0f;
};

//! Rolling diagnostics of the weapon-offset pipeline (to chase animation states that drop the offset).
struct PipelineDiag
{
    struct Event { float time; std::string text; };
    std::deque<Event> events;
    void* lastContext = nullptr;
    int lastInstanceCount = 0;
    bool lastForced = false;
    bool lastModifierNull = false;
    int lastRightIk = -2;
    int pwaUpdatesThisFrame = 0;
    int ctxUpdatesThisFrame = 0;
    int pwaUpdatesLastFrame = 0;
    int ctxUpdatesLastFrame = 0;
    int missingFrames = 0;      //!< consecutive frames without a PWA update while a weapon is equipped
    float lastMissingLog = -100.0f;

    void Add(float time, const std::string& text)
    {
        events.push_back({ time, text });
        while (events.size() > 40)
            events.pop_front();
    }
};

//! One per-update record of the hip-fire pipeline, for the pop tracer (see MainUpdate / SaveTrace).
struct TraceSample
{
    float t = 0.0f;            //!< async seconds
    float dt = 0.0f;           //!< filter step
    float cYaw = 0.0f, cYawT = 0.0f, cPitch = 0.0f, cPitchT = 0.0f; //!< convergence smoothed / target (deg)
    float wall = 0.0f, wallT = 0.0f;                                //!< wall pull-back smoothed / target (m)
    float dist = 0.0f;         //!< ray hit distance
    float mYaw = 0.0f, mPitch = 0.0f, mRoll = 0.0f;                 //!< measured weapon orientation rel. camera (deg)
    float mX = 0.0f, mY = 0.0f, mZ = 0.0f;                          //!< measured weapon position rel. camera (m)
    float lookYaw = 0.0f;      //!< the game's look offset yaw (deg)
    float aimBlend = 0.0f, sprintBlend = 0.0f, dragYaw = 0.0f;
    int pwa = 0, ctx = 0, cam = 0; //!< pipeline calls in the previous frame
    bool attachValid = false;
};

class ModMain final : public ChairloaderModBase, public IInputEventListener
{
public:
    using BaseClass = ChairloaderModBase;

    //---------------------------------------------------------------------------------
    // Mod Initialization
    //---------------------------------------------------------------------------------
    virtual void FillModInfo(ModDllInfoEx& info) override;
    virtual void InitHooks() override;
    virtual void InitSystem(const ModInitInfo& initInfo, ModDllInfo& dllInfo) override;
    virtual void InitGame(bool isHotReloading) override;

    //---------------------------------------------------------------------------------
    // Mod Shutdown
    //---------------------------------------------------------------------------------
    virtual void ShutdownGame(bool isHotUnloading) override;
    virtual void ShutdownSystem(bool isHotUnloading) override;

    //---------------------------------------------------------------------------------
    // GUI
    //---------------------------------------------------------------------------------
    virtual void Draw() override;

    //---------------------------------------------------------------------------------
    // Main Update Loop
    //---------------------------------------------------------------------------------
    virtual void UpdateBeforeSystem(unsigned updateFlags) override;
    virtual void UpdateBeforePhysics(unsigned updateFlags) override {}
    virtual void MainUpdate(unsigned updateFlags) override;
    virtual void LateUpdate(unsigned updateFlags) override;

    //---------------------------------------------------------------------------------
    // Mod Methods
    //---------------------------------------------------------------------------------
    ViewmodelSettings& GetSettings() { return m_settings; }
    PipelineDiag& GetDiag() { return m_diag; }

    //! Applies the current (blended) additive user offset to one hand's QuatT (view-space).
    void ApplyOffset(QuatT& offset) const;

    //! Called from the procedural-context hook after the game pushed its own operators.
    //! Pushes the ironsight override for the weapon hand while aiming (skeleton side).
    void OnProceduralContextUpdated(void* pContext);

    //! Called from the context hook every time (before/after the original), for diagnostics.
    void OnProceduralContextSeen(void* pContext, int instanceCount, bool forced, bool modifierNull);

    //! Called right after ArkPlayerCamera::UpdateView computed this frame's camera (render side).
    void OnCameraUpdated(ArkPlayerCamera* pCamera, SViewParams& params);
    void OnWeaponFired(CArkWeapon* pWeapon);
    void UpdateFeel(float dt, ArkPlayer* pPlayer);      //!< sprint pose / aim sway / view drag state (MainUpdate)
    void SanitizeFeel();                                 //!< resets the feel state if any number went bad
    void SanitizeSettings();                             //!< replaces non-finite persisted settings with defaults
    QuatT ComputeAimLocal() const;                       //!< weapon-local extras applied after the aim pose (sway, drag)
    const FeelState& GetFeel() const { return m_feel; }
    bool Active() const { return m_settings.enabled != 0 && m_settings.bypass == 0; } //!< viewmodel features on

    //! Procedural weapon offsets captured from the game this frame (view space).
    void SetGameOffset(int which, const QuatT& q);

    //! Shotgun spread multiplier for the given weapon (1 = unchanged).
    float GetSpreadMultiplier(const CArkItem* pWeapon);

    //! ADS sensitivity multiplier the game should use for the current call (0 = leave alone).
    float GetAimSensitivityMultiplier(float currentMultiplier, float zoomedMultiplier);
    int GetCameraZoomHandle() const { return m_cameraZoomHandle; }

    //! IInputEventListener: raw input for the aim key binding and nudge keys.
    virtual bool OnInputEvent(const SInputEvent& event) override;
    virtual bool OnInputEventUI(const SInputEvent& event) override { return false; }
    virtual int GetPriority() override { return 100; }

    const char* GetKeyName(int keyId) const;

    //! Pushes the desired weapon FOV to the renderer if it differs from the current one.
    void EnforceWeaponFov();

    float Now() const;

private:
    ViewmodelSettings m_settings;
    PipelineDiag m_diag;

    // Runtime state
    float m_crouchBlend = 0.0f;         //!< 0 = standing, 1 = crouched (linear progress, smoothed when applied).
    float m_aimBlend = 0.0f;            //!< 0 = hip, 1 = aiming.
    float m_reloadFade = 0.0f;          //!< 0 = offsets fully applied, 1 = fully faded (reload in progress).
    // Weapon state as read from the game each update (debug + gates).
    bool m_wsReloading = false, m_wsUnequipping = false, m_wsSwitching = false, m_wsReady = true;
    bool m_wsDrawing = false;           //!< a weapon was just equipped and has not reported ready yet (draw animation)
    unsigned m_wsLastWeaponId = 0;
    float m_wsDrawTimer = 0.0f;
    bool m_isCrouching = false;
    bool m_isAiming = false;
    bool m_aimKeyHeld = false;          //!< Raw state of the aim key (hold mode) or the toggle state.
    bool m_waitingForAimKey = false;    //!< UI "bind" mode: the next pressed key becomes the aim key.
    bool m_inputListenerRegistered = false;
    int m_cameraZoomHandle = 0;         //!< ArkPlayerZoomManager handle for the aim camera zoom (0 = none).
    float m_cameraZoomHfov = 0.0f;      //!< HFOV we asked for (to detect factor changes while aiming).
    AimLockState m_lock;
    RenderLockState m_render;

    // Per-weapon settings (keyed by entity class name, e.g. "ArkWeaponPistol").
    std::map<std::string, WeaponSettings> m_weapons;
    std::string m_currentWeaponClass;
    bool m_weaponsDirty = false;
    float m_weaponsSaveTimer = 0.0f;

    // Nudge keys currently held: index = axis (0..5 = x,y,z,pitch,yaw,roll), value = direction
    float m_nudgeAxis[6] = {};
    bool m_nudgeSlow = false;
    bool m_nudgeRotMode = false;

    bool m_sprintSensApplied = false;
    float m_gameSprintSensScale = -1.0f; //!< the game's own sprint scale (seen before we touched it)
    int m_reticleStyleBeforeHide = -1;  //!< hud_reticleSetting the game had before we hid it (-1 = not hidden by us)
    int m_reticleStyleApplied = -1;     //!< last hud_reticleSetting value we wrote
    int m_reticleGameStyle = -1;        //!< the game's own hud_reticleSetting (what the options menu holds), -1 = unknown
    bool m_reticlePlayerSeen = false;
    // Weapon convergence state
    float m_convergeDist = 30.0f;   //!< last raycast hit distance from the camera (m)
    bool m_convergeHit = false;
    float m_convergeYaw = 0.0f;     //!< smoothed, degrees
    float m_convergePitch = 0.0f;   //!< smoothed, degrees
    float m_convergeTargetYaw = 0.0f;
    float m_convergeTargetPitch = 0.0f;
    bool m_aimBlockedByWall = false;
    float m_aimBlockDist = 0.0f;    //!< current blocking distance (m)
    float m_wallPush = 0.0f;        //!< smoothed pull-back (m)
    float m_wallPushTarget = 0.0f;
    float m_wallBlend = 0.0f;       //!< 0..1 blend of the current weapon's near-wall pose (from the pull-back fraction)
    float m_wallPitchFade = 1.0f;   //!< 1..0 multiplier from the camera pitch (looking steeply up/down -> plain pull-back)
    float m_camPitchDeg = 0.0f;     //!< camera pitch this frame (deg, + up), debug
    int m_frameIndex = 0;
    // Frame timing as seen by MainUpdate: the smoothing filters use the measured wall-clock step between
    // updates (clamped), so a wrong game frame time or several updates per frame cannot defeat them.
    int64 m_lastAsyncTicks = -1;        //!< previous MainUpdate's async clock reading (int64, 100000/s); -1 = none yet
    float m_dtGame = 0.0f;              //!< last ITimer::GetFrameTime() (for the debug readout only)
    float m_dtUsed = 0.0f;              //!< what the filters were actually stepped with (seconds)
    float m_updateHz = 0.0f;            //!< measured MainUpdate rate (smoothed)
    QuatT m_gameOffsets[4] = { QuatT(IDENTITY), QuatT(IDENTITY), QuatT(IDENTITY), QuatT(IDENTITY) }; //!< look, strafe, recoil, bump
    int m_sensHookCalls = 0;
    bool m_playerDead = false;
    float m_reviveGuard = 0.0f;         //!< seconds after a revive during which the lock stays off
    float m_fireTimer = 0.0f;           //!< seconds left of the post-shot coupling boost
    float m_prevCamRecoilTime = 0.0f;
    float m_prevRecoilMag = 0.0f;
    int m_shotsSeen = 0;
    mutable int m_nanRecoveries = 0;    //!< how often a bad number was caught and reset (debug)
    FeelState m_feel;
    bool m_waitingForSteadyKey = false;
    bool m_shotPending = false;         //!< CArkWeapon::FireWeapon ran for the player's weapon since the last camera update
    int m_spreadHookCalls = 0;

    float m_lastAppliedFov = -1.0f;
    bool m_offsetHookActive = false;
    bool m_cameraHookActive = false;
    bool m_mouseCaptured = false;
    int m_inputModeHandle = -1;

    //! Reads stance / aim state from the player and advances the blend factors.
    void UpdateBlendStates(float dt);

    //! Pushes/pops/updates the camera zoom entry in the player's zoom manager.
    void UpdateCameraZoom(bool aiming);

    //! Applies held nudge keys to the active pose.
    void UpdateNudge(float dt);

    //! Handles a nudge key press/release. Returns true if the key was a nudge key.
    bool HandleNudgeKey(EKeyId key, bool pressed);

    //! Grabs/releases the hardware cursor + switches player input to menu mode.
    void UpdateMouseCapture(bool wantCapture);

    //! Raycasts along the view and updates the convergence angles.
    void UpdateConvergence(float dt);

    //! Computes and pushes the override for the weapon-hand IK joint given the camera in model space.
    void PushAimLock(void* pModifier, const QuatT& camAbs, const QuatT& ikAbs, const QuatT& weaponBone, float blend);

    //! The extra camera-space transform (recoil, sway, head-bob coupling) folded into the aim pose.
    QuatT ComputeAimExtra() const;

    //! Aim blend factor the lock should use this frame (0 when disabled / not aiming).
    float GetLockBlend() const;

    //! Refreshes the joint-hierarchy cache (hand subtrees) for the given character.
    void UpdateSkeletonCache(void* pCharInst, void* pAttachment);

    //! Which pose the nudge keys / "active pose" UI should edit right now.
    PoseOffset& GetActivePose(const char** outName);
    WeaponSettings& GetCurrentWeapon();
    const WeaponSettings* FindCurrentWeapon() const;
    const WeaponSettings* FindWeapon(const char* weaponClass) const; //!< user entry, else built-in, else null

    void LoadWeapons();
    void SaveWeapons();
    fs::path GetWeaponsPath() const;

    // Pop tracer
    std::vector<TraceSample> m_trace;   //!< ring buffer
    size_t m_traceHead = 0;
    size_t m_traceCount = 0;
    float m_traceLastSave = -100.0f;
    int m_traceSaves = 0;
    bool m_traceAuto = true;
    std::string m_traceStatus;
    void RecordTrace();
    void SaveTrace(const char* reason);

    //! Applies the world FOV / sprint sensitivity overrides.
    void UpdateCameraInputOverrides();

    //! Applies the reticle position/style settings (and the hide-while-aiming) to the game's cvars.
    void UpdateReticle();

    void RegisterCVars();
    void DrawWindow();
    bool DrawPoseSliders(PoseOffset& pose, const char* id, float posRangeCm, float rotRangeDeg);
public:
    //! Diagnostics for the shotgun/pistol pellet spread (vm_spread_debug), called from the SpawnPellets hook.
    void OnSpawnPellets(const void* pWeapon, const Vec3& position, const Vec3& aimPoint, bool bShootStraight);

    //! ArkPlayerInteraction::Interact pre-hook. Starts the support-hand reach and, when deferring, stores the
    //! call for later. Returns true if the original call must NOT run now (it has been deferred).
    bool OnInteract(void* pInteraction, int mode);
    //! PerformInteraction(carry) hook: schedules the grab for when the key has been held, returns the carry delay to use (hold + reach).
    float OnPerformCarry(void* pInteraction, int mode, IEntity* pEntity, float delay);
    void OnCarryStarted();              //!< ArkPlayerCarry::StartCarrying happened
    void StartMelee();                  //!< quick melee key: the punch starts (cooldown permitting)
    void DoMeleeHit();                  //!< at the punch's apex: the wrench's hit, scaled
    bool SupportHandOffWeapon() const;  //!< the support hand is animated out of view (one-handed weapon, no weapon): come up from the hidden spot
    void UpdateCarry(float dt);         //!< the pending / playing carry grab: start it, or call it off when the key was released
    //! Starts a reach animation towards a world point (or the fixed test point when pWorld is null).
    void StartReach(int style, const Vec3* pWorld);
    const InteractState& GetInteract() const { return m_interact; }
private:
    InteractState m_interact;
    bool m_interactReentry = false;     //!< our own deferred Interact call is running: let it through
    void UpdateInteract(float dt);      //!< timeline + deferred call countdown (MainUpdate)
    void FireDeferredInteract();        //!< makes the stored Interact call (UpdateBeforeSystem)
    //! Skeleton-side: pushes the additive offset of the support hand's IK target for this frame.
    void PushInteractReach(void* pModifier, void* pSkelPose, const QuatT& camAbs);
    //! Best model-space camera for the frame being animated (see OnProceduralContextUpdated).
    bool PredictCamera(ArkPlayer* pPlayer, QuatT& camAbs) const;
    void UpdateArmsVisibility();
    bool ExaminingWorldUI() const;
    //! World point a screen click aims at (examination mode): the centre ray of the view camera.
    bool CursorWorldPoint(Vec3& out);
    bool m_waitingForMeleeKey = false;  //!< UI "bind": the next key becomes the quick melee key
    int m_waitingForExamKey = 0;        //!< UI "bind": the next key becomes screen click key 1 / 2
    int m_examZoomHandle = 0;           //!< our zoom-manager entry overriding the screen zoom (0 = none)
    float m_examZoomHfov = 0.0f;
    void UpdateExamZoom();
    //! Per-weapon entry for the interaction reach; with no weapon out the "_none" entry.
    const WeaponSettings* InteractWeaponEntry() const;
    //! Whether an interaction of this type / mode should animate right now; fills skipReason when not.
    bool InteractAnimAllowed(int type, int mode);
    //! Finds the point the hand should go to for the given target entity (world). False = no usable point.
    bool FindInteractPoint(IEntity* pEntity, Vec3& outWorld) const;
    void DrawInteractTab();
    void DrawInteractMarkers(float dt);
    void DrawHandPoseEditor();
    void LogHandJoints();

    // Hand poses (Vee.ViewmodelTweaks.poses.xml)
    std::vector<HandPose> m_poses;
    std::vector<InteractRule> m_rules;  //!< which animation an interaction gets (poses file, <Rules>)
    int m_editRule = -1;
    //! Style for an interaction: 0 press, 1 grab, 2 none. className / text may be null.
    int ResolveStyle(int type, int mode, const char* className, const char* text, bool* pHover = nullptr) const;
    void SeedDefaultRules();
    static void SeedDefaultPoses(std::vector<HandPose>& poses, std::string* pStylePose);

    std::string m_stylePose[4] = { "point", "", "", "" };  //!< pose slots: 0 press, 1 grab, 2 the resting hand, 3 punch ("" = fingers keep the animation; rest: "" = the press pose)
    float m_stylePoseAmount[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static int PoseSlotOfStyle(int style) { return style == 1 ? 1 : (style == 2 ? 3 : 0); } //!< reach style (0 press, 1 grab, 2 punch) -> pose slot
    int m_uiExamineApplied[4] = { -1, -1, -1, -1 };    //!< what we last wrote to ui_examine_{keypad,fabricator,securitystation,workstation} (-1 = nothing yet)
    int m_editPose = 0;                 //!< index of the pose being edited in the UI
    bool m_posesDirty = false;
    float m_posesSaveTimer = 0.0f;
    char m_newPoseName[32] = "";
    HandPose* FindPose(const char* name);
    const HandPose* ActivePose() const; //!< pose for the current animation (style / posing mode)
    const HandPose* RestPose() const;   //!< pose of the resting hand (its own, or the press pose)
    const ReachStyle& CurStyle() const; //!< the style of the current reach (grab / press / the gentler press on screens)
    void UpdateHover();                 //!< is there something usable in front of us to hold the hand up for?
    void ApplyExamineCVars();           //!< ui_examine_* (the game's automatic zoom-in per screen type) from our settings
    void LoadPoses();
    void SavePoses();
    fs::path GetPosesPath() const;
    //! Pushes the hand orientation / finger overrides for this frame (called from PushInteractReach).
    void PushHandPose(void* pModifier, void* pSkelPose, const QuatT& camAbs, float weight);
};

extern ModMain* gMod;
