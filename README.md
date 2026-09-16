# Viewmodel Tweaks & Ironsights (Prey 2017)

A [Chairloader](https://github.com/thelivingdiamond/Chairloader) mod that gives Prey proper aim-down-sights,
an animated left hand for interactions and a quick melee punch, and lets you place the first-person weapon
exactly where you want it. Everything is tuned live from an in-game window; nothing is written to save games.

## Requirements

Prey (2017) on PC with **Chairloader 1.3.x or newer** installed and the game patched by ChairManager. The mod
hooks the exact game build Chairloader supports; on any other build it disables itself and says so in the log.

## Installation

1. In ChairManager: **Install Mod**, pick `Vee.ViewmodelTweaks-<version>.zip` (or unzip it yourself so that
   `Prey/Mods/Vee.ViewmodelTweaks/ModInfo.xml` exists).
2. Enable **Viewmodel Tweaks & Ironsights** in the mod list, then **Deploy**.
3. In the game, **Options -> Controls**: move *Select equipped power* off the right mouse button. The mod aims
   with RMB and swallows the game's action on it. (Or choose another aim key in Quick Settings.)

To update, install the new zip the same way. Settings live in `Prey/Mods/config/` and are kept. To uninstall,
disable the mod and deploy; saves are unaffected.

## What it does

**Ironsights.** Hold or toggle the aim key and the weapon comes up to the sights, aligned with the crosshair,
with a camera zoom, its own weapon FOV and its own look sensitivity. Every stock weapon has a tuned pose built
in, adjustable with sliders or nudge keys.

**Interaction animation.** The left hand reaches out when you interact: a press for buttons, switches,
terminals and hacking, a grab for pickups, loot, containers and things you carry. It is procedural - a tween on
the hand's IK target on top of the live animation - so it works with any weapon, or none. The game's side of
the interaction can be deferred so the button clicks and the item vanishes when the hand actually arrives. The
wrist and every finger can be posed by hand and assigned to a style. On in-world screens and keypads the arms
follow you into the zoomed view and the hand presses where you click; between clicks it rests in view. Which
interaction gets which animation is a rule list you can edit.

**Quick melee.** A punch on its own key (V by default): the hand winds up and strikes, and at the apex the
wrench's own hit lands, at full damage or any fraction you set. Cooldown, swing sound, impact shake, and a
camera swing that accelerates the way the arm goes, peaks on the impact and settles back. Needs a wrench in
the inventory, but works with any weapon out.

**Manual reloading** (optional, off by default). The magazine is only refilled when you press reload: firing an
empty gun does nothing, and holding the trigger until the last round simply stops the firing, so Prey's "shoot
to reload" and reload-after-the-last-shot both go away. The empty click, which vanilla only plays when you are
out of ammo entirely, then plays whenever the magazine is empty.

**Cancelling a reload** (part of the above). A reload in progress can be thrown away by firing, by raising the
sights, by a quick melee punch or by switching weapons - each its own switch, where stock Prey commits you to
the animation and remembers the shot for afterwards. It has a window: not in the first moments, not in the
last, and each weapon has a *point of no return* for animations that visibly commit, like the stun gun
ejecting its batteries. The weapon eases out of the pose the reload was in instead of snapping.

**Weapon feel.** Head-bob coupling while aiming (still sights, or a little lag), the game's procedural recoil
and the fire animation's own kick, both per weapon, with a per-shot boost. Bullet-spread multipliers for
aiming and hip fire on the pistol and shotgun - the only weapons in the game that have spread.

**Movement feel.** A sprint pose (the weapon drops and tilts, with extra sway), sway and settle while aiming -
a slow figure-eight, large when the sights come up or while you move, calming as you hold still, with a
hold-breath key - and GoldenEye-style view drag, the weapon following your turns on a spring.

**Placement.** Global standing and crouch offsets, per-weapon hip offsets, weapon FOV (fixed at 55 in vanilla),
world FOV, sprint look sensitivity.

**Hip fire.** Weapon convergence, so the barrel points at what the crosshair is over. Wall pull-back: the
weapon slides towards you near walls, further for longer weapons, with a per-weapon near-wall pose blended in
as it presses against the wall (the shotgun goes muzzle-up). No aiming with the muzzle in a wall, while
reloading or mid weapon-switch.

**Reticle.** Centred or custom position, weapon reticle / dot / hidden, hide while aiming. A hidden reticle
still returns as the cursor on in-world screens.

## Using it

Press **F1** for the Chairloader GUI and open the *Viewmodel Tweaks* window from the top bar. While it is open
the cursor is shown and the camera is frozen, so you can drag sliders with the game running; F1 again to play.

| Tab | What is in it |
| --- | --- |
| Quick Settings | The essentials: offsets, weapon and world FOV, aiming (key, hold/toggle, zoom, sensitivity), convergence, wall pull-back, reticle |
| Global | Standing and crouch offsets for all weapons, convergence and wall pull-back tuning |
| Weapon | The equipped weapon: hip offset, near-wall pose, aim pose, firing feel, spread, reloading |
| Feel | Sprint pose, sway and settle, hold breath, view drag |
| Aim | Key, transition, zoom, ironsight motion, sensitivity |
| Interact | The reach: timings, paths, hand poses, rules, screens and keypads, resting hand, quick melee |
| FOV, Nudge keys, Options | Weapon and world FOV, the in-game pose editor keys, window and diagnostics |

### Tuning a weapon's aim pose

1. Equip the weapon, open the window, tick *Toggle instead of hold* in **Aim**, aim, then open **Weapon**.
2. *Forward* moves the weapon towards or away from you; *Up / Down* raises the sights onto the crosshair;
   *Right / Left* stays 0 for a centred weapon. A small *Pitch* fixes a barrel that does not point straight.
   The sights should sit on the crosshair at any distance.
3. Or turn on **Nudge keys** and use I/J/K/L/U/O (hold H to rotate, N for slow) without opening the window.

Poses save automatically to `Prey/Mods/config/Vee.ViewmodelTweaks.weapons.xml`.

### Comparing against vanilla

*Vanilla viewmodel* (top of the window, or `vm_bypass`) switches every viewmodel feature off at once - offsets,
ironsights, feel, weapon FOV, convergence, wall pull-back, spread - and keeps the reticle, world FOV and sprint
sensitivity. Your values are kept; untick to get everything back.

### Sway and the reticle

The aim sway moves the sights off the crosshair. With *Hide reticle while aiming* on (the default) that is real
inaccuracy: shots go where the camera points, so time them for the calm part of the sway, or hold your breath.
With the reticle visible the sway is only cosmetic.

### Console

Every setting is also a `vm_*` cvar in the Chairloader console: `vm_fov`, `vm_aim_key`, `vm_converge`,
`vm_wall_push`, `vm_reticle_mode`, `vm_interact_*`, `vm_melee_*`, `vm_reload_*` and so on. `vm_show_advanced 1`
adds diagnostics and test buttons to the window.

## Compatibility and limitations

* Works alongside other Chairloader mods, including *Vee.Mediumcore* (the Terraria-style death rework that
  used to ship inside this one).
* Bullet-spread multipliers affect the pistol and the shotgun only - no other weapon has spread.
* Grenades, the wrench and the Nullwave transmitter have aiming disabled by default; enable it per weapon.
* The aim pose is relative to the camera, so it survives FOV changes, but the weapon's apparent size in
  ironsights follows the weapon FOV - adjust *Ironsights FOV* to taste.
* On a keypad you are nose-to-nose with, the pointing finger fills much of the view during the press; below a
  configurable distance the hand stays where it is instead.
* Resting the hand in view with a two-handed weapon takes the support hand off the grip. It can be switched
  off per case: weapon out, no weapon, while aiming.
* During very fast leans the hands can slip a millimetre on the grip for a frame; the sights stay put.

## Building from source

Standard Chairloader DLL-mod workflow (Visual Studio 2022 + CMake + vcpkg), see the
[mod project setup guide](https://github.com/thelivingdiamond/Chairloader/wiki/DLL-Modding-%E2%80%90-Mod-Project-Setup):

```
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
  -DCHAIRLOADER_COMMON_PATH=<Chairloader repo>/Common ^
  -DMOD_DLL_PATH=<Prey>/Mods/Vee.ViewmodelTweaks
cmake --build _build --config Release
```

| Path | |
| --- | --- |
| `Src/ModMain.cpp`, `Src/ModMain.h` | the whole mod |
| `DEVNOTES.md` | reverse-engineering notes: the rig, the hooks, what does not work |
| `CommonMod/`, `CMake/` | unmodified Chairloader mod SDK glue |

## Credits

* thelivingdiamond & tmp64 - Chairloader and the PDB-derived Prey SDK headers.
* Crytek - the GameSDK weapon-offset system this piggybacks on.
* Vee - the mod. MIT licensed.
