# Chocofi ZMK Firmware

This configuration targets a 36-key Chocofi with nice!nano v2 controllers and
nice!view displays. ZMK Studio is enabled over USB on the left half.

## Default keymap seed

`Corne-keymap.json` is the source export used to seed the compiled defaults in
`config/corne.keymap`. The build reads the `.keymap` file, not JSON directly.
The export's behavior IDs are device-local, so they are translated to named ZMK
behaviors rather than copied as IDs. Only the first 36 bindings of each layer
belong to the selected 5-column layout; the six trailing empty entries are
ignored. Unassigned behavior-ID-0 entries within an active layer are disabled
with `&none`.

The seed's four layers are **Base**, **Nav** (layer 1), **Number** (layer 2), and
**Func** (layer 3). All 36 exported bindings per layer are preserved. Number now
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

The custom **Home Row Mods** behavior (`&hrm`) implements the base modifier holds
and is available for additional assignments in Studio.
The standard **Mod-Tap** behavior is also available, but uses hold-preferred
handling rather than the typing-friendly settings below.

1. Build the updated firmware using the repository's GitHub Actions workflow and
   flash the corresponding UF2 to each half.
2. Connect the left half over USB and open [ZMK Studio](https://zmk.studio/).
   If you previously saved a Studio keymap, use **Restore Stock Settings** to load
   the updated base layout. This discards your saved Studio keymap, so record any
   customizations first. Flashing alone does not replace a saved Studio keymap.
3. Select a home-row key and choose **Home Row Mods**. Set parameter 1 to the
   modifier sent when held and parameter 2 to the key sent when tapped. For
   example, `&hrm LCTRL A` sends Ctrl on hold and A on tap.
4. Save your changes in Studio. Repeat for any other keys you want to configure.

Assigning Home Row Mods to an auto-shift key replaces auto-shift on that key:
holding it sends the chosen modifier rather than a shifted letter.

The behavior uses balanced hold-tap handling with a 250 ms tapping term, a
175 ms quick-tap window for tap-then-hold repetition, and a 125 ms prior-idle
requirement to reduce accidental modifiers during fast typing. Balanced handling
also resolves a hold when another key is pressed and released while it is held.
Studio can change the hold modifier and tap key, but not these timing or flavor
settings. To tune those, edit `hrm` in `config/corne.keymap`, rebuild, and reflash.

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

These combos are recognized on Base. Press all chord keys within 50 ms.
Three-key chords take precedence over their shorter subsets when
all keys arrive within that window; for example, J+K+L sends Enter, not Space.

| Chord | Action |
| --- | --- |
| I+O | Backspace |
| L+; | Delete |
| J+K+L | Enter |
| J+K | Space |
| D+F | Space |
| S+D+F | Enter |
| Q+W | Esc |
| W+E | Tab |
| U+I+O | Toggle Number on/off |
| A+S | Toggle Left Shift Lock on/off |

U+I+O uses `&tog NUMBER` on both Base and Number, so the same chord turns the
layer on and off. A+S uses `&kt LSHIFT` on all five active layers; Shift stays
held until the chord is pressed again or Shift is otherwise released.
Neither toggle has a tap-dance delay or a double-tap action. Release all chord
keys before repeating a chord.

U+I+O+P has no dedicated combo or tap-dance action. Its U+I+O subset can still
trigger the Number toggle, so it should not be used as a separate gesture.

W+E+R retains the existing one-shot Func action on Base. This is the only
remaining tap-dance behavior, has one action, and resolves immediately.
The standard one-shot layer expires after 1 second if unused.

The Number + Shift layer remains available in Studio, but no longer has a
dedicated chord. For shifted numbers using the new toggles, enable Number with
U+I+O and Shift Lock with A+S; disable them independently with the same chords.
Later Studio edits to Number do not automatically update its shifted copy.

Studio can assign the standard Layer Toggle and Key Toggle behaviors, and the
remaining Function One Shot behavior, to individual keys. It cannot configure
the physical combos. The old Number/Shift tap-dance behaviors have been removed;
reassign any saved Studio bindings that used them, or restore stock settings.

## Checking the configuration

Run `python -m unittest discover -s tests` to check seed fidelity, the shifted
Number layer, combo positions/scopes, and tap-dance actions. GitHub Actions builds
the firmware for both halves.
