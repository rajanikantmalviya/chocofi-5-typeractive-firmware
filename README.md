# Chocofi ZMK Firmware

This configuration targets a 36-key Chocofi with nice!nano v2 controllers and
nice!view displays. ZMK Studio is enabled over USB on the left half.

## Optional standalone keyboard viewer

The left-half build now uses the `studio-viewer-usb` snippet. It retains the
existing Studio RPC port and adds a **second, read-only USB serial interface**
for the standalone ZMK Keyboard Viewer. The right-half build is unchanged.
Flash the new left-half UF2 to enable the viewer; do not reset saved Studio
settings or restore stock bindings just to enable telemetry.

The module exports the selected physical layout, current in-memory layer
bindings and names (including Studio edits), behavior child actions, and
compiled combo definitions. Physical presses are observed before combo and
hold-tap event capture, without consuming or changing the original event.
Snapshots include actual active layers and each held key's originating layer.
The viewer is a learning display, not an input device or keymap editor.

**The keyboard works normally with both apps closed, either one open, or both
open.** Viewer data is sent only in response to requests on its own port.
Studio can continue using its original port. No host requests means no
telemetry transmission or periodic viewer work. A small physical-position
bitset is maintained on the central half; a low-priority worker handles all
serialization and USB transmission, with bounded queues and transmit timeouts.
Neither USB backpressure nor a crashed/disconnected viewer waits in the HID,
combo, or hold-tap processing path. Scanning and behavior timing are unchanged.
There is some additional RAM/flash and CPU cost; hardware latency and power
measurements are still required before claiming negligible impact.

The normal reusable build workflow automatically loads this repository as a
Zephyr module via `zephyr/module.yml`. To build manually with ZMK v0.3:

```text
west build -s zmk/app -b nice_nano_v2 -S studio-viewer-usb -- -DZMK_CONFIG=<repo>/config -DZMK_EXTRA_MODULES=<repo> -DSHIELD="corne_left nice_view_adapter nice_view"
```

To disable the extension entirely, switch the left build's snippet back to
`studio-rpc-usb-uart` and rebuild; the module defaults to disabled.
Keep **5 Column** selected: existing combo/hold-trigger positions use that
layout. After keymap edits, reload the viewer's keymap.

### Viewer USB protocol v1

The viewer interface accepts only `C` (configuration) and `S` (snapshot).
Responses are newline-delimited JSON. `C` sends `hello` (version 1), physical
`key` records (geometry in hundredths of a key unit/degrees), `behavior`
records, current `layer` records, compiled `combo` records and a `ready`
delimiter. Bindings are `[behaviorId, param1, param2]`; hold-tap child 0 is hold
and child 1 is tap. Layer IDs are stable IDs, not display-order indices.
`S` returns `state` with `sequence`, `pressed` pairs `[position, originLayer]`,
`released` positions since the previous snapshot and active `layers` in
highest-priority-first order. Snapshots reconcile state without an unbounded
event backlog. The interface honors Studio locking and has no write commands.

## Default keymap seed

`Corne-keymap.json` is the source export used to seed the compiled defaults in
`config/corne.keymap`. The build reads the `.keymap` file, not JSON directly.
The export's behavior IDs are device-local, so they are translated to named ZMK
behaviors rather than copied as IDs. Only the first 36 bindings of each layer
belong to the selected 5-column layout; the six trailing empty entries are
ignored. Unassigned behavior-ID-0 entries within an active layer are disabled
with `&none`.

The seed's four layers are **Base**, **Nav** (layer 1), **Number** (layer 2), and
**Func** (layer 3). All 36 exported hold/tap parameters are preserved; home-row
mods use hand-specific behaviors instead of the seed's generic behavior. Number now
uses plain key presses, not auto-shift. **Number + Shift** (layer 4) mirrors its
enabled keys with Shift applied, and layer 5 remains reserved for Studio.

## Base layout

The base layer uses standard QWERTY key presses without auto-shift:

```text
Q W E R T     Y U I O P
A S D F G     H J K L ;
Z X C V B     N M , . /
    Esc Tab Space     Enter Delete Backspace
```

Space and Enter activate **Nav** when held. Tab and Delete activate **Number**
when held. Esc activates **Func** when held. All five send their normal keys
when tapped, as specified by the JSON seed.

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

1. Build the updated firmware using the repository's GitHub Actions workflow and
   flash the corresponding UF2 to each half.
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

## Mouse actions in ZMK Studio

Mouse support is enabled with `CONFIG_ZMK_POINTING=y`. Hold Space or Enter to
activate Nav, then hold S to move the cursor left or F to move it right.

Studio offers **Mouse Move Left/Right/Up/Down** and
**Mouse Scroll Left/Right/Up/Down** as named, parameter-free behaviors that can
be assigned to any key. Each action starts on press and stops on release.
These are fixed-parameter macro wrappers because ZMK v0.3's native mouse
movement and scrolling behaviors lack Studio parameter metadata. Direction is
selected by choosing the named behavior; speed and acceleration still require
firmware changes.

For clicks, choose the standard **Mouse Key Press** behavior in Studio and
select MB1 (left), MB2 (right), MB3 (middle), MB4, or MB5. The updated seed already
includes left/right clicks, movement, scrolling, and navigation keys on Nav.

After flashing, use **Restore Stock Settings** in Studio to load the new seed
if a saved keymap exists; record customizations first because this replaces
them. Mouse support changes the HID descriptor. For an existing Bluetooth
connection, forget the keyboard on the host, clear the corresponding keyboard
bond using `&bt BT_CLR`, and pair again so the host refreshes the descriptor.
See [ZMK's mouse emulation documentation](https://zmk.dev/docs/keymaps/behaviors/mouse-emulation).

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
ordinary U/I/O typing. Number is accessible with E+R, by holding Tab or Delete,
or by assigning **Layer Toggle** for layer 2 to a key in Studio.

E+R uses `&tog NUMBER` on Base and Number, so the same physical chord turns
Number on and off (the positions contain 3+4 on Number). On Base, W+E+R's
one-shot Func action takes precedence if all three keys arrive within 80 ms.
Release all chord keys before toggling again.

A+S uses `&kt LSHIFT` on all five active layers; Shift stays
held until the chord is pressed again or Shift is otherwise released.
The Shift toggle has no tap-dance delay or double-tap action. Release all chord
keys before repeating a chord.

U+I+O and U+I+O+P have no dedicated action during this trial. Their U+I or I+O
subsets can trigger an editing combo, so do not use them as separate gestures.

W+E+R retains the existing one-shot Func action on Base. This is the only
remaining tap-dance behavior, has one action, and resolves immediately.
The standard one-shot layer expires after 1 second if unused.

The Number + Shift layer remains available in Studio, but no longer has a
dedicated chord. For shifted numbers, toggle Number with E+R or hold Tab or
Delete for Number and toggle
Shift Lock with A+S, or use a Studio-assigned Number toggle.
Later Studio edits to Number do not automatically update its shifted copy.

Studio can assign the standard Layer Toggle and Key Toggle behaviors, and the
remaining Function One Shot behavior, to individual keys. It cannot configure
the physical combos. The old Number/Shift tap-dance behaviors have been removed;
reassign any saved Studio bindings that used them, or restore stock settings.

## Checking the configuration

Run `python -m unittest discover -s tests` to check seed fidelity, the shifted
Number layer, combo positions/scopes, tap-dance actions, and opposite-hand
home-row assignments. These checks validate definitions, not physical key-event
delivery or combo recognition. GitHub Actions builds
the firmware for both halves.
