// Colours and type of the Android 16 SystemUI (the stock look: the dark Material 3 roles of the reference phone's
// dynamic palette, measured on its stills, same wallpaper) and of the four looks of the optional Renaissance theme
// (theme.js, docs/theme.md): marble, sea (day, light), forest, plum (night). The variable Google Sans Flex stands in
// for Android's variable-* font families.
//
// The look is live. Color, SurfaceEffect and SHADE_PANEL are stable objects whose arrays are updated IN PLACE when the
// look changes, so a surface holding Color.onSurface (or a Canvas drawing it) sees the new colour on its next paint. What
// a surface copied into an actor (an St style string, a Cogl colour, a font) it restyles from a callback it registers
// with onRestyle(owner, callback); offRestyle(owner) when it goes. startLive() / stopLive() (extension enable /
// disable) connect the whole machinery to the settings.
import Cogl from 'gi://Cogl';

import {currentLook, fontChain, offLookChanged, onLookChanged} from './theme.js';
import {px, SCALE, sp} from './units.js';

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
    onWall: [255, 255, 255],                // text and icons straight on the wallpaper (day looks: ink)
    chip: [34, 44, 45],                     // materialColorSurfaceBright, measured: recents chips and menu
    button: [31, 41, 42],                   // recents buttons
    pill: [41, 53, 55],                     // surface_effect_3 flattened on the card (measured)
    glyph: [13, 26, 27],                    // a circle's glyph is punched out in the card's colour (measured)
    dim: [0, 0, 0],                         // modal dim (power menu, camera sheet)
    hairline: [176, 181, 194],              // gilded edges (no edge in the stock look)
    effect0: [5, 54, 63],                   // = accent1_800
    effect1: [17, 21, 22],                  // = neutral1_500_l6
    effect2: [184, 235, 243],               // = accent1_100
    effect3: [184, 235, 243],
};
const STOCK_SCALARS = {hairlineScale: 1, effectAlpha: [0.50, 0.54, 0.15, 0.10], shadow: null};

// The Renaissance looks: roles and extras are the subset of tools/theme/palette.json (looks.<look>.roles / .extras) the
// shell uses; 'shell' is palette.js's own block (also in palette.json). hairline: the gilded edge colour (hairline_scale
// scales gild()'s alpha), shadow: a day look's card shadow, shade_panel / effects: given where the stock formula
// would not do (a light panel, the cards at .8): [colour, alpha]; absent they are the stock formulas.
const isRenaissance = () => activeLook !== null;
const hex = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
const LOOKS = {
    marble: {
        roles: {
            primary: '#9a6f2c',
            on_primary: '#ffffff',
            surface: '#f6f6f4',
            surface_container_low: '#f1f1ef',
            surface_container: '#ebebe9',
            surface_container_high: '#e5e5e3',
            surface_container_highest: '#dfe0e2',
            surface_bright: '#fbfbfa',
            on_surface: '#2b2c31',
            on_surface_variant: '#55575f',
            tertiary: '#a5522c',
        },
        extras: {
            silver: '#7c8190',
            silver_dim: '#a9adb7',
            scrim: '#282c3c',
        },
        shell: {
            accent1_100: '#2f6b4f',
            accent1_800: '#c9d6ce',
            neutral1_500_l4: '#f6f6f4',
            neutral1_500_l6: '#e5e5e3',
            notification_scrim: '#d6dae2',
            on_wall: '#2b2c31',
            chip: '#fafaf7',
            button: '#f3f3f0',
            pill: '#ecece8',
            glyph: '#fafaf7',
            hairline: '#3c4050',
            hairline_scale: 0.4,
            shadow: 'rgba(60, 64, 80, 0.16)',
            shade_panel: ['#f6f6f4', 0.66],
            effects: [['#c9d6ce', 0.5], ['#fafaf7', 0.8], ['#2f6b4f', 0.14], ['#2f6b4f', 0.09]],
        },
    },
    sea: {
        roles: {
            primary: '#9a6f2c',
            on_primary: '#ffffff',
            surface: '#f6efe2',
            surface_container_low: '#f2eadb',
            surface_container: '#ece3d2',
            surface_container_high: '#e6dccb',
            surface_container_highest: '#e0d6c4',
            surface_bright: '#faf5ea',
            on_surface: '#2c2b29',
            on_surface_variant: '#5a574f',
            tertiary: '#a5522c',
        },
        extras: {
            silver: '#8b8470',
            silver_dim: '#b3ac98',
            scrim: '#282c3c',
        },
        shell: {
            accent1_100: '#2f7fb8',
            accent1_800: '#c8dae8',
            neutral1_500_l4: '#f6efe2',
            neutral1_500_l6: '#e6dccb',
            notification_scrim: '#e2d6c0',
            on_wall: '#2c2b29',
            chip: '#fbf7ee',
            button: '#f4eee2',
            pill: '#ece5d6',
            glyph: '#fbf7ee',
            hairline: '#3c4050',
            hairline_scale: 0.4,
            shadow: 'rgba(60, 64, 80, 0.16)',
            shade_panel: ['#f4eddf', 0.66],
            effects: [['#c8dae8', 0.5], ['#fbf7ee', 0.8], ['#2f7fb8', 0.14], ['#2f7fb8', 0.09]],
        },
    },
    forest: {
        roles: {
            primary: '#e3c98a',
            on_primary: '#2a2008',
            surface: '#09110d',
            surface_container_low: '#0d1511',
            surface_container: '#121c17',
            surface_container_high: '#182420',
            surface_container_highest: '#1e2b25',
            surface_bright: '#26352d',
            on_surface: '#e6efe4',
            on_surface_variant: '#a9b8ab',
            tertiary: '#ff9f2e',
        },
        extras: {
            silver: '#9fb3a5',
            silver_dim: '#5f6f64',
            scrim: '#030806',
        },
        shell: {
            accent1_100: '#bfe6c8',
            accent1_800: '#12281c',
            neutral1_500_l4: '#080c0a',
            neutral1_500_l6: '#0e1611',
            notification_scrim: '#030806',
            on_wall: '#ffffff',
            chip: '#26352d',
            button: '#182420',
            pill: '#1e2b25',
            glyph: '#0c1611',
            hairline: '#9fb3a5',
            hairline_scale: 0.8,
            shadow: 'rgba(0, 0, 0, 0.5)',
            effects: [['#12281c', 0.5], ['#0c1611', 0.8], ['#8fc29a', 0.15], ['#8fc29a', 0.1]],
        },
    },
    plum: {
        roles: {
            primary: '#d9b26a',
            on_primary: '#2a1d04',
            surface: '#08070d',
            surface_container_low: '#0d0c15',
            surface_container: '#13121e',
            surface_container_high: '#1a1927',
            surface_container_highest: '#211f31',
            surface_bright: '#2a283d',
            on_surface: '#f0e6d0',
            on_surface_variant: '#bdb299',
            tertiary: '#ff9f2e',
        },
        extras: {
            silver: '#b0b5c2',
            silver_dim: '#6e7280',
            scrim: '#04030a',
        },
        shell: {
            accent1_100: '#f3dc9f',
            accent1_800: '#121a42',
            neutral1_500_l4: '#09080e',
            neutral1_500_l6: '#100f1a',
            notification_scrim: '#0a081c',
            on_wall: '#ffffff',
            chip: '#2a283d',
            button: '#1a1927',
            pill: '#211f31',
            glyph: '#0d0c15',
            hairline: '#b0b5c2',
            hairline_scale: 1,
            shadow: 'rgba(0, 0, 0, 0.5)',
        },
    },
};

// The flat table of a look: the Color fields (arrays), the scalars and the shade panel
function resolve(look) {
    if (!look)
        return {colors: STOCK, scalars: STOCK_SCALARS, panel: null};
    const {roles: r, extras: x, shell: s} = LOOKS[look];
    const colors = {
        primary: hex(r.primary),
        onPrimary: hex(r.on_primary),
        surface: hex(r.surface),
        surfaceContainerLow: hex(r.surface_container_low),
        surfaceContainer: hex(r.surface_container),
        surfaceContainerHigh: hex(r.surface_container_high),
        surfaceContainerHighest: hex(r.surface_container_highest),
        onSurface: hex(r.on_surface),
        onSurfaceVariant: hex(r.on_surface_variant),
        tertiary: hex(r.tertiary),
        accent1_100: hex(s.accent1_100),
        accent1_800: hex(s.accent1_800),
        neutral1_500_l4: hex(s.neutral1_500_l4),
        neutral1_500_l6: hex(s.neutral1_500_l6),
        notificationScrim: hex(s.notification_scrim),
        silver: hex(x.silver),
        silverDim: hex(x.silver_dim),
        white: [255, 255, 255],
        black: [0, 0, 0],
        onWall: hex(s.on_wall),
        chip: hex(s.chip),
        button: hex(s.button),
        pill: hex(s.pill),
        glyph: hex(s.glyph),
        dim: hex(x.scrim),
        hairline: hex(s.hairline),
    };
    const effects = s.effects ?? [[s.accent1_800, 0.50], [s.neutral1_500_l6, 0.54], [s.accent1_100, 0.15],
        [s.accent1_100, 0.10]];
    effects.forEach(([c], i) => {
        colors[`effect${i}`] = hex(c);
    });
    return {
        colors,
        scalars: {hairlineScale: s.hairline_scale, effectAlpha: effects.map(e => e[1]),
            shadow: look === 'marble' || look === 'sea' ? s.shadow : null},
        panel: s.shade_panel ? [hex(s.shade_panel[0]), s.shade_panel[1]] : null,
    };
}

export const Color = {hairlineScale: 1, shadow: null};
export const SurfaceEffect = {0: [[0, 0, 0], 0], 1: [[0, 0, 0], 0], 2: [[0, 0, 0], 0], 3: [[0, 0, 0], 0]};
export const SHADE_PANEL = [[0, 0, 0], 0];
let activeLook = null;

function assign(array, values) {
    for (let i = 0; i < values.length; i++)
        array[i] = values[i];
}

// Put a look's values into Color, SurfaceEffect and SHADE_PANEL, in place. The Color fields are arrays made once.
function apply(look) {
    activeLook = look;
    const {colors, scalars, panel} = resolve(look);
    for (const [key, value] of Object.entries(colors)) {
        if (Color[key])
            assign(Color[key], value);
        else
            Color[key] = [...value];
    }
    Color.hairlineScale = scalars.hairlineScale;
    Color.shadow = scalars.shadow;
    for (let i = 0; i < 4; i++) {
        SurfaceEffect[i][0] = Color[`effect${i}`];
        SurfaceEffect[i][1] = scalars.effectAlpha[i];
    }
    // The shade panel (behind scrim): accent1_800 α .32 over neutral1_500@L*4 α .32 over #404040 α .40, α 0.723
    // (ShadeColors.kt:33-71); a day look gives its own light panel
    const [c, a] = panel ?? over([Color.accent1_800, 0.32], over([Color.neutral1_500_l4, 0.32], [[64, 64, 64], 0.40]));
    assign(SHADE_PANEL[0], c);
    SHADE_PANEL[1] = a;
}

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

// The look's callbacks: owner -> [callback(look)], called after the colours changed. A surface registers one for what
// it copied (styles, Cogl colours, fonts, layouts) and drops it with offRestyle when it is destroyed.
const restylers = new Map();
const LIVE = {};

export function onRestyle(owner, callback) {
    const list = restylers.get(owner);
    if (list)
        list.push(callback);
    else
        restylers.set(owner, [callback]);
}

export function offRestyle(owner) {
    restylers.delete(owner);
}

function restyle(look) {
    apply(look);
    for (const [owner, callbacks] of [...restylers]) {
        if (!restylers.has(owner))
            continue;
        for (const callback of callbacks) {
            try {
                callback(look);
            } catch (e) {
                logError(e, 'gnome-phone: restyle');
            }
        }
    }
}

// Follow the settings (extension enable) until stopLive (disable), which also drops every callback left
export function startLive() {
    apply(currentLook());
    onLookChanged(LIVE, restyle);
}

export function stopLive() {
    offLookChanged(LIVE);
    restylers.clear();
}

apply(currentLook());

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
    if (!activeLook)
        return '';
    const [r, g, b] = Color.hairline;
    const edge = ` border: ${(1 / SCALE).toFixed(3)}px solid rgba(${r}, ${g}, ${b}, ${(alpha * Color.hairlineScale).toFixed(3)});`;
    // a day look's cards also lift off the light shade with a soft shadow (the mockup's .card)
    return Color.shadow ? `${edge} box-shadow: 0 ${px(2).toFixed(2)}px ${px(8).toFixed(2)}px ${Color.shadow};` : edge;
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
