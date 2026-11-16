# Multi-language rollout plan

The desktop and projected VR settings already use `Strings.resx`, `Loc`, WPF resource
bindings, Windows' UI culture and satellite-resource fallback. The current shipped copy
is English. This update keeps the new settings and theme names in that system and tests
them with the existing expanded pseudo-locale. This document plans real translations;
it does not claim that additional languages have shipped.

## First delivery

Proposed first languages: English, French, German and Spanish. Follow with Italian,
Brazilian Portuguese, Japanese and Simplified Chinese after native-speaker review and
font checks. Language identifiers and saved theme/preset IDs must remain stable across
translations. Translated labels must never become persistence keys or IPC values.

1. Add a separate app-wide language preference: System default plus languages for which
   complete, reviewed resource files actually exist. Preserve the Windows formatting
   culture for displayed measurements; keep JSON, shader data and control IPC invariant.
2. Extract and review `Strings.fr.resx`, `Strings.de.resx` and `Strings.es.resx` against the
   English keys. Translate tooltips, errors, accessibility names, selectors and controller
   instructions, including correct terminology for each controller family. Keep product
   names VRX and Vrex unchanged. English remains the fallback for a missing key.
3. Refresh all generated selectors and live projected controls when changing language.
   Preserve the current settings page, scroll offset, panel pose and active profile.
   Test that a language change does not write different theme IDs, preset masks or keys.
4. Replace Vrex's current English speech and interaction-menu bitmap atlases with locale-selected atlases built
   from translated short phrases. The current editable English source and generator are
   under `desktop/assets`. Do not translate a bitmap by drawing over its pixels. Chat
   phrase IDs stay stable, and the VRX chest badge remains the original logo.

## Layout, fonts and input

Use the existing roughly 35% longer pseudo-locale as the first gate for every new
control. Test the desktop at 100%, 150% and 200% DPI and the controller panel at its
minimum readable size. Verify all nine theme palettes, light/dark contrast, long German
labels, accented text, number formatting and keyboard input. Avoid fixed-width label
assumptions; let labels wrap while preserving controller targets large enough to select.

Japanese and Chinese need glyph coverage in the desktop font, projected WPF bitmap,
browser chrome and Vrex atlas. Select and package an appropriately licensed fallback
font if the Windows fallback cannot cover those paths reliably. The floating swipe
keyboard currently uses an English word predictor: add locale-specific dictionaries and
segmentation, or explicitly offer tap typing without English suggestions in unsupported
languages. Do not imply that translated UI means the predictor is multilingual.

Right-to-left languages are a later milestone: mirror layouts and navigation deliberately,
keep numeric values and URLs readable, and test ray hit targets against the mirrored
visuals. A resource-file translation alone is insufficient for RTL support.

## Release gates

* Automated key parity, placeholder parity, plural forms, resource fallback and no
  untranslated user-facing literals; retain the existing pseudo-locale screenshot checks.
* Native-speaker review in context, with desktop and headset screenshots, rather than
  accepting unreviewed machine output as the final translation.
* Desktop and VR checks for preset layering, theme selection, error messages, saved
  language preferences, page position and Vrex bubbles.
* Version translated resources with the app, document remaining English-only features,
  and list only verified languages in the language picker and release notes.
