#pragma once
#include <Chairloader/ModSDK/ChairloaderModBase.h>
#include <Prey/CryInput/IInput.h>
#include <map>
#include <deque>
#include <string>
#include <vector>

class ArkPlayerCamera;
class CArkItem;
struct SViewParams;

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
    void FromQuatT(const QuatT& q)
    {
        Ang3 a(q.q);
        posX = q.t.x; posY = q.t.y; posZ = q.t.z;
        pitch = RAD2DEG(a.x); roll = RAD2DEG(a.y); yaw = RAD2DEG(a.z);
    }
};

//! All user-tunable global settings. Backed by CVars flagged VF_DUMPTOCHAIR so Chairloader
//! persists them between sessions (Mods/config/Chairloader_CVars.xml) and they can
//! also be changed from the in-game console (vm_*).
struct ViewmodelSettings
{
    int enabled = 1;            //!< Master switch for position/rotation offsets.

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
    float aimWallBlockScale = 1.75f; //!< Tolerance multiplier on the blocking distance (higher = blocks further from the wall).

    int   wallPushEnabled = 1;      //!< Pull the weapon back towards the camera when the view ray hits something close.
    float wallPushStartDist = 1.2f; //!< meters from the camera where the pull-back starts
    float wallPushFullDist = 0.45f; //!< meters where the full per-weapon pull-back is reached
    float wallPushSmoothTime = 0.15f; //!< seconds (exponential smoothing)
    int   wallPushWhileAiming = 0;  //!< keep pulling back while aiming (the sights stay on the view axis)


    int   worldFovEnabled = 0;  //!< Override the game's horizontal FOV (cl_hfov).
    float worldFov = 85.0f;     //!< Horizontal FOV in degrees when worldFovEnabled.
    int   sprintSensEnabled = 0;//!< Override the look-sensitivity scale the game applies while sprinting.
    float sprintSensScale = 1.0f; //!< 1 = same sensitivity as walking.

    int   fovEnabled = 1;       //!< Master switch for the weapon (near-scene) FOV override.
    float fov = 55.0f;          //!< Weapon FOV in degrees. Stock game uses 55.

    int   showWindow = 1;       //!< Whether the ImGui window is open when the Chairloader GUI is visible.
    int   showAdvanced = 0;     //!< Show diagnostics, self-tests and experimental features (Death tab).
    int   guiMouse = 1;         //!< Show the cursor / block player look input while the window is open (game keeps running).

    // Not persisted: render-time placement self-test (moves the weapon/hands by this much, always).
    float testOffsetUp = 0.0f;  //!< meters
    int   testHands = 1;
};

//! Per-weapon settings (keyed by entity class name, stored in Vee.ViewmodelTweaks.weapons.xml).
struct WeaponSettings
{
    PoseOffset hip;         //!< Additive offset while not aiming, on top of the global standing/crouch offsets.
    PoseOffset aim;         //!< Ironsight pose: weapon (attachment) relative to the camera while aiming.
    bool aimAllowed = true; //!< Whether the aim key does anything with this weapon.
    float wallPush = 0.06f; //!< How far (m) this weapon moves back towards the camera against a wall (longer weapon = more).
    float fireCoupling = 0.35f;     //!< Head-bob coupling right after a shot (lets the fire kick show while aiming).
    float fireCouplingTime = 0.30f; //!< Seconds the fire coupling decays over.
    float aimRecoilScale = 1.0f;    //!< Multiplier on the game's procedural recoil/bump offsets while aiming.
    float aimSpreadMult = 1.0f;     //!< Per-weapon spread multiplier while aiming (times the global one).
    float hipSpreadMult = 1.0f;     //!< Per-weapon spread multiplier while not aiming (times the global one).
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
    virtual void UpdateBeforeSystem(unsigned updateFlags) override {}
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

    //! Procedural weapon offsets captured from the game this frame (view space).
    void SetGameOffset(int which, const QuatT& q) { m_gameOffsets[which] = q; }

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
    int m_frameIndex = 0;
    QuatT m_gameOffsets[4] = { QuatT(IDENTITY), QuatT(IDENTITY), QuatT(IDENTITY), QuatT(IDENTITY) }; //!< look, strafe, recoil, bump
    int m_sensHookCalls = 0;
    bool m_playerDead = false;
    float m_reviveGuard = 0.0f;         //!< seconds after a revive during which the lock stays off
    float m_fireTimer = 0.0f;           //!< seconds left of the post-shot coupling boost
    float m_prevCamRecoilTime = 0.0f;
    float m_prevRecoilMag = 0.0f;
    int m_shotsSeen = 0;
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

    //! Applies the world FOV / sprint sensitivity overrides.
    void UpdateCameraInputOverrides();

    //! Applies the reticle position/style settings (and the hide-while-aiming) to the game's cvars.
    void UpdateReticle();

    void RegisterCVars();
    void DrawWindow();
    bool DrawPoseSliders(PoseOffset& pose, const char* id, float posRangeCm, float rotRangeDeg);
};

extern ModMain* gMod;
