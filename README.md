# Chocofi ZMK Firmware

This configuration targets a 36-key Chocofi with nice!nano v2 controllers and
nice!view displays. ZMK Studio is enabled over USB on the left half.

## Base layout

The base layer uses standard QWERTY key presses without auto-shift:

```text
Q W E R T     Y U I O P
A S D F G     H J K L ;
Z X C V B     N M , . /
    Esc - Space     Enter Delete Backspace
```

The second left thumb key is disabled. Space and Enter send their normal keys
when tapped and activate the existing **Raise** layer (layer 2, used as the Num
layer) when held. Lower and Raise are otherwise unchanged, including auto-shift
where previously configured. Lower no longer has a direct binding on the base
layer; assign one in Studio if needed.

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
