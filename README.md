# Chocofi ZMK Firmware

This configuration targets a 36-key Chocofi with nice!nano v2 controllers and
nice!view displays. ZMK Studio is enabled over USB on the left half.

## Local runtime firmware integration

All work for this integration stays local. Build scripts do not push, open PRs,
or flash controllers. Flashing and remote operations require separate authorization.
Do not use settings-reset firmware or restore stock settings merely to enable
runtime features: existing Studio bindings, Bluetooth bonds and split settings
remain authoritative. Take a backup before any separately authorized upgrade.
Saved keymap records contain numeric behavior IDs plus parameters, not full
behavior names. This central snippet explicitly retains
`CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE=y`, loading the same
`behavior/local_id/<id>` device-name table as v0.3; it does **not** switch to
CRC16. Named behavior nodes and the `0xEC000`/`0x8000` settings partition remain
unchanged. Do not build an upgrade with CRC16 unless a separately reviewed
export/import migration is available. Export the complete effective device
keymap before flashing: persisted keymap data is a delta, so bindings inherited
from changed stock defaults can change even when all saved overrides survive.
This preserves the storage policy; physical reboot/upgrade preservation still
requires hardware verification and is not claimed by host tests.
Local Corne validation on October 14, 2026 verified upgrade/settings preservation,
saved macro and combo execution after reset, both split halves and nice!view
displays, BLE Viewer recovery after reset, and restoration of the original
configuration across another reset. The local release images are
`firmware\runtime\corne-left-runtime-viewer.uf2` and
`firmware\runtime\corne-right-runtime.uf2`; the left image includes the runtime
settings load-order and vacant-combo export fixes.
The build rejects accidental CRC16 selection. A checked behavior-table
adaptation reserves even unmappable old IDs instead of recycling them for a
new behavior; unknown saved IDs remain visible in viewer binding frames and
are logged by ZMK, not silently rebound to an unrelated action.

The required custom-Studio fork is newer than ZMK v0.3 and uses Zephyr 4.1.
Both halves must be built from the matching pinned fork; its equivalent board
target is **`nice_nano@2.0.0/nrf52840/zmk`**, not `nice_nano_v2`.
`config/west.yml` pins:

| Dependency | Revision |
| --- | --- |
| cormoran/zmk, main+custom-studio-protocol | `1fc72aaff7a42bd533c1f4c9b3f23c5317c745f4` |
| zmkfirmware/zephyr | `10ba6d0cb38bc3d258775d27982f707599320085` |
| zmk-feature-runtime-combo | `d9490e7b5d41c516221d131241009e81740efcee` |
| zmk-feature-runtime-macro | `38530962eb7061b60efcb98e84585cfb1f0cb6de` |
| zmk-feature-custom-settings | `c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f` |

The fork additionally pins Studio messages to
`89b81d2e587fce807b668dff2a6967a40beef421`. Standard Studio clients without the
custom envelope cannot edit these runtime subsystems. Discover numeric custom
subsystem indices on every connection; never persist or guess them.
Discovery identifiers are `cormoran__runtime_combo`, `cormoran__runtime_macro`
and `cormoran_custom_settings`. The first two are also their settings scope IDs.
Use the pinned modules' protobuf schemas for payloads, not ordinary keymap RPCs.
Metadata/limits discovery is public; editing and content reads require Studio
unlock according to the module handlers.

The central half has **16 combo slots**, **4 positions per combo**, 32-bit layer
masks, **8 macro slots**, 24-byte macro names, 256-byte encoded bodies,
a 1536-byte shared macro name/body pool and 64 decoded macro actions.
The shared native behavior queue is bounded at 160 entries. Queue capacity is
checked before runtime playback begins. These are ceilings, not reservations
for eight maximum-size macros: pool exhaustion returns an error.
Combo mask bits address ordered layer indices, as used by
`zmk_keymap_highest_layer_active()`, rather than stable keymap layer IDs.
Masks may retain inactive indices from compiled defaults.
The central snippet sets a 512-byte Studio RPC receive buffer and an 8192-byte
RPC thread stack. The fork derives the custom-call payload capacity from that
receive-buffer size; its 30-byte default cannot carry runtime edits such as
macro rename or packed steps. Even a short macro-create request can overflow
the UART receive ring once the outer RPC envelope is included.

All eleven former native combos are now `cormoran,runtime-combo-defaults`.
There is no second `zmk,combos` matcher executing the same defaults.
Deleting a default combo persists a disabled override; resetting a combo always
persists the return to its compiled default, regardless of other unsaved edits.
Saved combo overrides win over defaults.

All eight existing movement/scroll macros use **pause-for-release**, which
runtime encoding v1 cannot represent. They remain native, preserving press,
hold and release behavior. They are listed explicitly in `factory-origin.json`
and viewer macro frames, rather than silently omitted or converted into taps.
There are no safely convertible native macros in the current factory seed.
New runtime macros use the module's v1 DOWN/UP/TAP/DELAY/packed-key format.

`tools/patch_runtime.py` makes checked build-local adaptations of the exact
pinned sources; dependency checkouts are not edited. Macro rename rewrites the
same ordinal record **in place**, retaining the slot even when all slots are
occupied. New names must still fit the bounded pool. Default macros, if added
as supported `cormoran,runtime-macro-default` nodes, are seeded once in PERSIST
mode after settings load. `runtime_macro_factory/v1` records successful seeding;
deletion therefore survives reboot and defaults retain their persisted ordinal
slots. Explicit macro reset restores the encoded default; ordinary reboot does
not recreate deleted defaults.
Per-name `runtime_macro_factory/deleted/<name>` tombstones are written before
deleting/renaming defaults, including generic settings resets. They prevent
resurrection even if initial factory-marker persistence failed. A failed
tombstone save returns an error without deleting the macro.
Persisted behavior bindings are decoded during boot and validated at settings
commit, after the saved behavior-ID table has loaded. This preserves runtime
combo bindings even when flash traversal visits custom settings before IDs.
Normal edits and post-boot reloads still validate immediately; invalid saved
bindings report a commit error instead of signaling that settings are ready.
The combo RPC handler also uses the custom-settings notification-suppression
bracket already used by macro RPCs. This avoids redundant self-notifications
competing with Save/reset replies on Studio's single connection; Viewer
configuration revision events remain enabled.
Viewer `C` temporarily rejects definition edits with `-EBUSY` to export one
coherent configuration. It does not hold the runtime/settings mutex across
transport writes or suspend physical events, HID, or macro playback.
Discarding a newly created, unsaved macro is also guarded if it would release
a referenced slot. The local discard adaptation releases never-saved macro
slots and their pooled bodies instead of retaining an entry that was created
after the last Save. Discard of edits to an already-persisted macro remains
allowed while idle, since its slot survives.

Runtime mutations are serialized with physical-event processing and rejected
with `-EBUSY` while physical keys, a runtime macro or queued native actions are
active. Macro deletion
and generic settings reset additionally reject direct references from current
keymap bindings, nonempty combos (including disabled ones), encoded runtime macro bodies and compiled
behavior children. Rename retains
references. The UI must additionally inspect backup references and any opaque
nested bindings it creates. Release keys before editing. Queued behavior
bindings are copied; mutation never swaps out an active combo's release action.
Only the central half enables runtime settings/RPC; split and nice!view remain
enabled on both halves.

## Optional standalone keyboard viewer

The left-half build now uses the `studio-viewer-usb` snippet. It retains the
existing Studio RPC port and adds a **second, read-only USB serial interface**
for the standalone ZMK Keyboard Viewer, plus the separate BLE service below.
The right half has no viewer service. Do not reset saved Studio
settings or restore stock bindings just to enable telemetry.

The module exports the selected physical layout, current in-memory layer
bindings and names (including Studio edits), behavior child actions, and
effective runtime combo definitions and native/runtime macro steps. Physical presses are observed before combo and
hold-tap event capture, without consuming or changing the original event.
Snapshots include actual active layers and each held key's originating layer.
The viewer is a learning display, not an input device or keymap editor.

**The keyboard works normally with both apps closed, either one open, or both
open.** USB viewer data is sent only in response to requests on its own port;
BLE events stream only after explicit subscription and `E`.
Studio can continue using its original port. No host requests means no
telemetry transmission or periodic viewer work. A small physical-position
bitset is maintained on the central half; a low-priority worker handles all
serialization and USB transmission, with bounded queues and transmit timeouts.
Neither USB backpressure nor a crashed/disconnected viewer waits in the HID,
combo, or hold-tap processing path. Scanning and behavior timing are unchanged.
There is some additional RAM/flash and CPU cost; hardware latency and power
measurements are still required before claiming negligible impact.

Load this repository as a Zephyr module via `zephyr/module.yml`.
With the pinned dependency workspace and a suitable local Zephyr toolchain:

```text
west build -s zmk\app -b nice_nano@2.0.0/nrf52840/zmk -S studio-viewer-usb -- -DZMK_CONFIG="<repo>\config" -DZMK_EXTRA_MODULES="<repo>;<runtime-combo>;<runtime-macro>;<custom-settings>" -DSHIELD="corne_left nice_view_adapter nice_view"
```

To disable BLE telemetry only, set `CONFIG_ZMK_KEYBOARD_VIEWER_BLE=n`.
Keep **5 Column** selected: existing combo/hold-trigger positions use that
layout. After keymap edits, reload the viewer's keymap.

### Viewer USB protocol v1

The viewer interface accepts only `C` (configuration) and `S` (snapshot).
Responses are newline-delimited JSON. `C` sends `hello` (version 1), physical
`key` records (geometry in hundredths of a key unit/degrees), `behavior`
records, current `layer` records, `capabilities`, effective `combo` records,
native/runtime `macro` records and a `ready`
delimiter. Bindings are `[behaviorId, param1, param2]`; hold-tap child 0 is hold
and child 1 is tap. Layer IDs are stable IDs, not display-order indices.
`S` returns `state` with `sequence`, `pressed` pairs `[position, originLayer]`,
`released` positions since the previous snapshot and active `layers` in
highest-priority-first order. Snapshots reconcile state without an unbounded
event backlog. `state.configRevision` is a boot-local monotonic definition
revision; re-request `C` when it changes. The interface honors Studio locking
and has no keymap/settings write commands.

### Viewer BLE protocol v1

This service is distinct from HID, split BLE and Studio's RPC characteristic:

| Attribute | UUID | Properties |
| --- | --- | --- |
| Service | `00000010-0196-6107-c967-c5cfb1c2482a` | Primary |
| Command | `00000011-0196-6107-c967-c5cfb1c2482a` | Write with response |
| Data | `00000012-0196-6107-c967-c5cfb1c2482a` | Notify |

Subscribe to Data's CCC before writing exactly one ASCII command byte.
`C` emits the same configuration-through-`ready` NDJSON as USB; `S` emits an
initial state; `E` emits an initial state and enables event-driven streaming;
`D` disables it. Invalid commands, lengths/offsets, full command queues and
missing subscription are rejected with ATT errors.

Data notifications contain arbitrary consecutive chunks of UTF-8 NDJSON,
**not one JSON object per packet**. Accumulate bytes across MTU boundaries,
split only on LF, and discard a partial record after transport failure.
There is no additional header, NUL terminator or binary envelope. Bindings use
the device's current local behavior IDs and `[id,param1,param2]` framing.
Native macros have `behaviorId`, `name`, `source:"native"` and steps including
`pause-for-release`; runtime macros have `slot`, `name`, `persisted`,
`hasUnsavedChanges` and tap/press/release binding or `delayMs` steps.
Combos retain name/positions/layers/binding/timeoutMs and add slot/enabled/source.
`capabilities` includes runtimeCombos/runtimeMacros/bleStreaming and bounded
slot/pool limits.

Only an encrypted connection to the **active host profile**, while Studio is
unlocked, can request or receive data. CCC access is encrypted too; split
peripherals and inactive profiles cannot read key telemetry. The existing
`CONFIG_ZMK_STUDIO_LOCKING=n` means Studio considers this configuration unlocked;
enable normal Studio locking if physical unlock is desired.

The priority-10 worker serializes config/state output without interleaving
records, sleeps until requests/events, coalesces state to at most one update
every 34 ms, keeps 4 queued commands and one 128-byte transmit staging buffer,
waits at most 250 ms for notification completion, and yields 3 ms between
chunks. It never requests faster HID connection parameters, performs settings
writes, or blocks the input hot path on USB/BLE delivery. Disconnect/unsubscribe
disables streaming. Measure simultaneous HID latency and power on hardware;
builds and host tests alone do not establish radio responsiveness.

## Default keymap seed

`Corne-keymap.json` is the source export used to seed the compiled defaults in
`config/corne.keymap`. The build reads the `.keymap` file, not JSON directly.
The export's behavior IDs are device-local, so they are translated to named ZMK
behaviors rather than copied as IDs. Only the first 36 bindings of each layer
belong to the selected 5-column layout; the six trailing empty entries are
ignored. Unassigned behavior-ID-0 entries within an active layer are disabled
with `&none`.

The current seed's stable IDs are **Base** 0, **Nav** 1, **Number** 2,
**Func** 3 and **Mouse** 4 (the export lists Mouse before Func).
All exported parameters are preserved using the exact named behaviors.
**Number + Shift** is appended at ID 5, with a reserved Studio layer at ID 6.
Saved layer IDs are not rewritten; an older saved ID-4 layer stays saved until
the user deliberately edits/restores it.
Run `python tools\factory_seed.py` after changing the seed, or use `--check`
to verify it. Unmappable IDs/names, duplicate IDs, nonempty padding, unused
parameters and invalid layer references fail explicitly.
`config/factory-origin.json` retains source SHA-256, device-local behavior
mapping, layer IDs and physical position provenance.

## Base layout

The base layer uses standard QWERTY key presses without auto-shift:

```text
Q W E R T     Y U I O P
A S D F G     H J K L ;
Z X C V B     N M , . /
    Esc Tab Space     Enter Delete Backspace
```

Space and Enter activate **Mouse** when held. Tab and Delete activate **Nav**;
Esc and Backspace activate **Func**. V and M activate **Number** when held.
Tap actions are preserved exactly from the current JSON seed.

The base home-row modifier holds are:

| Tap key | Hold modifier |
| --- | --- |
| A | Left Shift |
| S | Left Ctrl |
| D | Left GUI |
| F | Left Alt |
| J | Right Alt |
| K | Right GUI |
| L | Right Ctrl |
| ; | Right Shift |

## Home-row mods in ZMK Studio

The custom **Home Row Mods Left** (`&hrm_left`) and **Home Row Mods Right**
(`&hrm_right`) behaviors implement the base modifier holds. Choose the behavior
for the hand where the key is physically located.
The old **Home Row Mods** (`&hrm`) remains available for saved Studio bindings,
but has no opposite-hand filtering.
The standard **Mod-Tap** behavior is also available, but uses hold-preferred
handling rather than the typing-friendly settings below.

1. Build both halves locally. Hardware flashing is a separate authorized operation.
2. Connect the left half over USB and open [ZMK Studio](https://zmk.studio/).
   If you previously saved a Studio keymap, use **Restore Stock Settings** to load
   the updated base layout. This discards your saved Studio keymap, so record any
   customizations first. Flashing alone does not replace a saved Studio keymap.
3. Select a home-row key and choose **Home Row Mods Left** or **Home Row Mods Right**.
   Set parameter 1 to the
   modifier sent when held and parameter 2 to the key sent when tapped. For
   example, `&hrm_left LCTRL A` sends Ctrl on hold and A on tap.
4. Save your changes in Studio. Repeat for any other keys you want to configure.

Assigning Home Row Mods to an auto-shift key replaces auto-shift on that key:
holding it sends the chosen modifier rather than a shifted letter.

The behavior uses balanced hold-tap handling with a 300 ms tapping term, a
175 ms quick-tap window for tap-then-hold repetition, and a 175 ms prior-idle
requirement to reduce accidental modifiers during fast typing. Balanced handling
also resolves a hold when another key is pressed and released while it is held.
The hand-specific behaviors filter that decision to opposite-hand positions,
including only the opposite hand's three thumb keys. Same-hand key releases
resolve an undecided home-row mod as a tap. `hold-trigger-on-release` defers
the positional check until release, allowing modifiers to be stacked.
Studio can change the hold modifier and tap key, but not these timing or flavor
settings. To tune those, edit the hand-specific behaviors in
`config/corne.keymap`, rebuild, and reflash.
The longer hold threshold and prior-idle requirement reduce accidental modifier
holds, but also require more deliberate holds and a longer pause after typing.
Balanced handling can still resolve a modifier before 300 ms when another key
is pressed and released on the opposite hand. Holding a key by itself past
300 ms still activates its modifier; this is not a ban on deliberate same-hand
shortcuts after the modifier has resolved.

Saved Studio assignments to the old **Home Row Mods** do not gain filtering
automatically. Reassign A/S/D/F to **Home Row Mods Left** and J/K/L/semicolon to
**Home Row Mods Right**, preserving their parameters, or restore stock settings
after recording any customizations. Keep the selected layout at **5 Column**:
combo and positional hold-tap indices are defined for that layout.

## Home-row layer tap in ZMK Studio

**Home Row Layer Tap** (`&hrlt`) is an additional selectable behavior; existing
key assignments, standard **Layer-Tap** and home-row modifiers are unchanged.
After a separately authorized firmware upgrade, reconnect Studio, select a key,
open **Advanced**, and choose **Home Row Layer Tap**. Set parameter 1 to the
layer to activate while held and parameter 2 to the key to type when tapped.
For example, `&hrlt NAV A` types A on tap and holds Nav on a deliberate hold.
Save only when you want to persist the new assignment. Do not reset settings
to make the behavior appear; Studio discovers it from the updated firmware.

Tap and quickly press the same key again, holding the second press, to repeat
the tap key using the host's normal keyboard repeat delay/rate instead of
activating the layer. The second press must start within 175 ms of the first
press. Releasing it stops the repeat. A fresh hold after at least 175 ms of
typing inactivity activates the chosen layer after 300 ms. Balanced handling
can resolve that hold earlier when another key is pressed and released.
Like the existing home-row mods, a press within 175 ms of another typed key is
forced to a tap to protect fast typing; wait briefly before a deliberate layer
hold. This behavior works on either hand, without opposite-hand filtering.
Studio changes the layer and tap key, not the firmware timing settings.

## Mouse actions in ZMK Studio

Mouse support is enabled with `CONFIG_ZMK_POINTING=y`. The current factory
layout holds Space or Enter for Mouse, with J/K/L providing left/down/right
movement and I providing up.

Studio offers **Mouse Move Left/Right/Up/Down** and
**Mouse Scroll Left/Right/Up/Down** as named, parameter-free behaviors that can
be assigned to any key. Each action starts on press and stops on release.
These retained native fixed-parameter macro wrappers expose mouse
movement and scrolling in Studio. Direction is selected by choosing the named
behavior. Default maximum speed and acceleration are compiled parameters;
Mouse Speed (%) provides the runtime cursor-speed multiplier below.

For clicks, choose the standard **Mouse Key Press** behavior in Studio and
select MB1 (left), MB2 (right), MB3 (middle), MB4, or MB5. The updated seed already
includes clicks, movement, scrolling, and 100/200/400% speed selectors on Mouse.

Saved bindings are not automatically replaced with this seed; preserve them.
Only use **Restore Stock Settings** deliberately after backing up customizations.
Mouse support changes the HID descriptor. For an existing Bluetooth
connection, forget the keyboard on the host, clear the corresponding keyboard
bond using `&bt BT_CLR`, and pair again so the host refreshes the descriptor.
See [ZMK's mouse emulation documentation](https://zmk.dev/docs/keymaps/behaviors/mouse-emulation).

### Runtime mouse speed percentages

After flashing the updated left-half firmware, assign **Mouse Speed (%)** in
Studio and enter the desired percentage in **Percentage (100 = normal)**.
Use **100**, **150**, **200**, **300**, or another positive integer. Save the key
assignments in Studio.

| Parameter | Selected speed | Default maximum movement rate |
| --- | --- | --- |
| `100` | Normal / reset | 600 units/second |
| `150` | 1.5 times normal | 900 units/second |
| `200` | Twice normal | 1200 units/second |
| `300` | Three times normal | 1800 units/second |

Pressing an assigned key selects that speed until another percentage key is
pressed or the keyboard restarts. Releasing the key does not reset it.
Restarting returns to **100%**; the saved key mappings remain available.
Percentages below 100 slow the cursor. These are firmware movement units, not
guaranteed screen pixels; host pointer settings also affect the visible speed.
Extreme values saturate at the signed 16-bit movement limit rather than
overflowing or reversing direction.

This scales only keyboard cursor movement through `&mmv`, including the
existing Mouse Move macros. It does not change the 300 ms acceleration ramp,
scrolling, mouse clicks, another mouse/trackpad, or the current key assignments.
Do not restore stock settings just to enable it: existing saved bindings remain
usable, and you can assign the new behavior in Studio.

For bindings edited in the firmware source, use `&mouse_speed 150`,
`&mouse_speed 200`, `&mouse_speed 300`, and `&mouse_speed 100`. The standard
module registration includes this feature in future builds independently of
the optional viewer; the viewer's USB interface remains enabled.

## Combos and tap dances

These combos are recognized on Base. Press all chord keys within 80 ms.
Three-key chords take precedence over their shorter subsets when
all keys arrive within that window; for example, J+K+L sends Enter, not Space.

| Chord | Action |
| --- | --- |
| U+I | Backspace |
| I+O | Delete |
| J+K+L | Enter |
| J+K | Space |
| D+F | Space |
| S+D+F | Enter |
| Q+W | Esc |
| W+E | Tab |
| E+R | Toggle Number on/off |
| A+S | Toggle Left Shift Lock on/off |

L+; no longer has a combo. The wider 80 ms chord window makes staggered presses
easier to recognize, but may add delay to shorter overlapping chords and make
ordinary typing rolls more likely to trigger a combo.

**Troubleshooting trial:** U+I+O's Number toggle is temporarily disabled to
isolate U+I Backspace and I+O Delete from their shared three-key overlap.
Their positions, Base scope, and 80 ms window are unchanged. Live read-only
Studio diagnostics confirmed the 5-column layout and U/I/O bindings at
positions 6/7/8; this trial does not establish the cause of the physical failure.
Test U+I and I+O separately after flashing, in both press orders, along with
ordinary U/I/O typing. Number is accessible with E+R, by holding V or M,
or by assigning **Layer Toggle** for layer 2 to a key in Studio.

E+R uses `&tog NUMBER` on Base and Number, so the same physical chord turns
Number on and off (the positions contain 3+4 on Number). On Base, W+E+R's
one-shot Func action takes precedence if all three keys arrive within 80 ms.
Release all chord keys before toggling again.

A+S uses `&kt LSHIFT` on all six active layers; Shift stays
held until the chord is pressed again or Shift is otherwise released.
The Shift toggle has no tap-dance delay or double-tap action. Release all chord
keys before repeating a chord.

U+I+O and U+I+O+P have no dedicated action during this trial. Their U+I or I+O
subsets can trigger an editing combo, so do not use them as separate gestures.

W+E+R retains the existing one-shot Func action on Base. This is the only
remaining tap-dance behavior, has one action, and resolves immediately.
The standard one-shot layer expires after 1 second if unused.

The Number + Shift layer remains available in Studio, but no longer has a
dedicated chord. For shifted numbers, toggle Number with E+R or hold V or
M for Number and toggle
Shift Lock with A+S, or use a Studio-assigned Number toggle.
Later Studio edits to Number do not automatically update its shifted copy.

Studio can assign the standard Layer Toggle and Key Toggle behaviors, and the
remaining Function One Shot behavior, to individual keys. A custom-protocol
Studio client can now configure the runtime physical combos.
The old Number/Shift tap-dance behaviors have been removed;
reassign any saved Studio bindings that used them, or restore stock settings.

## Checking the configuration

`python -m unittest discover -s tests -v` exercises factory validation, runtime
reference decoding, mouse math, and compiled default-importer persistence
failure/reboot handling. Run it in a host C compiler environment to avoid skips.
For a Zephyr compile-only check of supported DT defaults, add
`tests\runtime_defaults_compile.overlay` via `EXTRA_DTC_OVERLAY_FILE` and build
the `runtime_macro_dt_defaults.c.obj` target. Remove that test overlay and
rebuild before using production artifacts; it is deliberately not a factory
definition.

Run `python -m unittest discover -s tests` to check seed fidelity, the shifted
Number layer, combo positions/scopes, tap-dance actions, and opposite-hand
home-row assignments. These checks validate definitions, not physical key-event
delivery or combo recognition. Build both halves locally; no remote operation
or hardware flashing is performed by these checks.
