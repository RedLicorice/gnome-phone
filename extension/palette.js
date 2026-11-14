// Colours and type of the Android 16 SystemUI: the dark Material 3 roles of the reference phone's dynamic palette
// (measured on its stills, same wallpaper), and the variable Google Sans Flex standing in for Android's variable-*
// font families.
import Cogl from 'gi://Cogl';

import {fontChain, isRenaissance} from './theme.js';
import {SCALE, sp} from './units.js';

// [r, g, b] 0..255; roles are the dark scheme
const STOCK = {
    primary: [156, 207, 215],               // #9CCFD7, slider, active tiles, actions
    onPrimary: [13, 71, 78],                // #0D474E
    surface: [10, 15, 16],                  // #0A0F10, volume pill
    surfaceContainerLow: [15, 21, 22],      // the power menu's panel: between surface and surfaceContainer, NOT measured
    surfaceContainer: [20, 27, 28],         // a heads-up's card: tone 9 at High's hue (2025 dark: 9, 12, 15), not measured
    surfaceContainerHigh: [24, 32, 34],
    surfaceContainerHighest: [30, 39, 41],  // #1E2729, inactive volume track
    onSurface: [221, 231, 233],             // #DDE7E9, all notification text
    onSurfaceVariant: [190, 200, 202],
    tertiary: [182, 199, 234],
    accent1_100: [184, 235, 243],           // #B8EBF3
    accent1_800: [5, 54, 63],               // tone 20, fitted to the measured shade background
    neutral1_500_l4: [11, 13, 14],          // neutral1_500 re-lit to L* 4
    neutral1_500_l6: [17, 21, 22],          // ... to L* 6, fitted to the measured cards and inactive tiles
    notificationScrim: [5, 28, 28],         // fitted: the stack backdrop measures (11, 33, 35)
    silver: [190, 200, 202],                // stock stand-ins: the Renaissance hairline colours
    silverDim: [109, 119, 121],
    white: [255, 255, 255],
    black: [0, 0, 0],
};

// The optional Renaissance look ("Cyberpunk meets Italian Renaissance", theme.js): the same roles, OLED black with a
// lapis cast, gold leaf for primary, parchment text, amber (the camera eye) as the only warm edge light. Mirrors libneo's palette.
const RENAISSANCE = {
    primary: [217, 178, 106],               // gold leaf #D9B26A
    onPrimary: [42, 29, 4],
    surface: [8, 7, 13],
    surfaceContainerLow: [13, 12, 21],
    surfaceContainer: [19, 18, 30],
    surfaceContainerHigh: [26, 25, 39],
    surfaceContainerHighest: [33, 31, 49],
    onSurface: [240, 230, 208],             // parchment
    onSurfaceVariant: [189, 178, 153],
    tertiary: [255, 159, 46],                // amber, the Kiroshi eye
    accent1_100: [243, 220, 159],           // light gold
    accent1_800: [18, 26, 66],              // lapis night
    neutral1_500_l4: [9, 8, 14],
    neutral1_500_l6: [16, 15, 26],
    notificationScrim: [10, 8, 28],
    silver: [176, 181, 194],                // #B0B5C2 hairline ornament (tools/theme/palette.json)
    silverDim: [110, 114, 128],             // #6E7280
    white: [255, 255, 255],
    black: [0, 0, 0],
};

export const Color = isRenaissance() ? RENAISSANCE : STOCK;

// surface_effect_N (dark): translucent fills drawn over the blurred shade
export const SurfaceEffect = {
    0: [Color.accent1_800, 0.50],
    1: [Color.neutral1_500_l6, 0.54],
    2: [Color.accent1_100, 0.15],
    3: [Color.accent1_100, 0.10],
};

export function rgba(c, a = 1) {
    return `rgba(${c[0]}, ${c[1]}, ${c[2]}, ${a})`;
}

export function cogl(c, a = 1) {
    return new Cogl.Color({red: c[0], green: c[1], blue: c[2], alpha: Math.round(a * 255)});
}

export function setSource(cr, c, a = 1) {
    cr.setSourceRGBA(c[0] / 255, c[1] / 255, c[2] / 255, a);
}

// a over b, both [rgb, alpha]: the opaque-equivalent colour and the combined alpha
export function over([ca, aa], [cb, ab]) {
    const a = aa + ab * (1 - aa);
    if (a === 0)
        return [[0, 0, 0], 0];
    return [ca.map((v, i) => (v * aa + cb[i] * ab * (1 - aa)) / a), a];
}

export function mix(a, b, t) {
    return a.map((v, i) => v + (b[i] - v) * t);
}

// The shade panel (behind scrim): accent1_800 α .32 over neutral1_500@L*4 α .32 over #404040 α .40, α 0.723
// (ShadeColors.kt:33-71)
export const SHADE_PANEL = over([Color.accent1_800, 0.32],
    over([Color.neutral1_500_l4, 0.32], [[64, 64, 64], 0.40]));

const FAMILY = 'Google Sans Flex';

// The theme's font chain for a role (fallback when the setting is absent), and the body chain as a prefix
export function chain(role, fallback) {
    return fontChain(role) || fallback;
}
export function bodyFamilies() {
    const body = fontChain('body');
    return body ? `${body}, ` : '';
}

// The Renaissance theme's hairline edge for a surface's St style string (one physical pixel at the phone's scale, a
// silver hairline: gold is kept for focus and accents); '' in the stock look
export function gild(alpha = 0.4) {
    return isRenaissance() ? ` border: ${(1 / SCALE).toFixed(3)}px solid rgba(176, 181, 194, ${alpha});` : '';
}

// Data text (the status bar clock) in a mono face in the Renaissance theme; Pango falls to the stock face without it
export function dataFont(sizeSp, wght) {
    return isRenaissance() ? `${chain('mono', 'JetBrains Mono')}, ${FAMILY} ${wght >= 600 ? 'Bold ' : ''}${sp(sizeSp).toFixed(3)}px` : null;
}

// The Renaissance theme sets big text (clocks) in Michelangelus; where it is not installed Pango falls to the stock face
const DISPLAY_FROM_SP = 24;
export function displayFont(sizeSp, wght) {
    return `${chain('display', 'Michelangelus')}, ${FAMILY} ${wght >= 600 ? 'Bold ' : ''}${sp(sizeSp).toFixed(3)}px`;
}

// A Pango font description for a Clutter.Text: size in sp, the variable axes as Android's variable-* families set
// them (rounded terminals)
export function font(sizeSp, wght, {wdth = 100, rond = 100, opsz = sizeSp} = {}) {
    if (isRenaissance() && sizeSp >= DISPLAY_FROM_SP)
        return displayFont(sizeSp, wght);
    return `${isRenaissance() ? bodyFamilies() : ''}${FAMILY} ${sp(sizeSp).toFixed(3)}px @wght=${wght},wdth=${wdth},ROND=${rond},opsz=${opsz}`;
}

// The M3 type scale (sp, weight) of the roles SystemUI uses; *Emphasized are the variable-*-emphasized families
export const Type = {
    labelLarge: [14, 500],
    labelLargeEmphasized: [14, 700, {opsz: 16}],   // matched to the status bar clock's pixels
    labelMedium: [12, 500],
    labelSmallEmphasized: [11, 600],
    bodySmall: [12, 400],
    bodyMedium: [14, 400],
    bodyMediumEmphasized: [14, 500],
    titleSmallEmphasized: [14, 600],
    titleMediumEmphasized: [16, 600],
    displaySmallEmphasized: [36, 500],
};
