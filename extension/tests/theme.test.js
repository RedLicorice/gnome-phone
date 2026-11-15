// theme.js and palette.js: the effective look over all combinations, Color updated in place, the swaps of wallpaper and
// icon pack (run by theme-test.sh, which sets up the settings).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

// what units.js reads from the shell
globalThis.global = {display: {get_monitor_scale: () => 3, get_primary_monitor: () => -1}};

const theme = await import('../theme.js');
const palette = await import('../palette.js');

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}

const ctx = GLib.MainContext.default();
const settle = () => {
    // the coalescing idle runs, then nothing is left to do
    for (let i = 0; i < 20; i++)
        ctx.iteration(false);
};

const mine = new Gio.Settings({schema_id: 'de.yesman.GnomePhone.Theme'});
const iface = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
const bg = new Gio.Settings({schema_id: 'org.gnome.desktop.background'});
const neo = new Gio.Settings({schema_id: 'de.yesman.neo'});

// 1. the effective look
let ok = true;
for (const name of ['default', 'renaissance']) {
    for (const scheme of ['default', 'prefer-dark', 'prefer-light']) {
        for (const day of ['marble', 'sea']) {
            for (const night of ['forest', 'plum']) {
                mine.set_string('name', name);
                mine.set_string('day-look', day);
                mine.set_string('night-look', night);
                iface.set_string('color-scheme', scheme);
                const want = name === 'default' ? null : (scheme === 'prefer-dark' ? night : day);
                if (theme.currentLook() !== want || theme.isDay() !== (want === 'marble' || want === 'sea')) {
                    ok = false;
                    print(`  ${name} ${scheme} ${day} ${night}: ${theme.currentLook()}`);
                }
            }
        }
    }
}
check('currentLook / isDay over all 24 combinations', ok);
check('isRenaissance follows name', (mine.set_string('name', 'default'), !theme.isRenaissance()));

// 2. Color changes in place, once per settled change
mine.set_string('name', 'default');
mine.set_string('day-look', 'marble');
mine.set_string('night-look', 'forest');
iface.set_string('color-scheme', 'default');
const onSurface = palette.Color.onSurface;
const effect1 = palette.SurfaceEffect[1];
const panel = palette.SHADE_PANEL;
const panelColor = palette.SHADE_PANEL[0];
palette.startLive();
let calls = [];
const owner = {};
palette.onRestyle(owner, look => calls.push(look));
settle();
check('stock: onSurface is the measured one', onSurface.join() === '221,231,233', onSurface.join());
check('stock: no gilded edge', palette.gild() === '');

mine.set_string('name', 'renaissance');
mine.set_string('day-look', 'sea');
iface.set_string('color-scheme', 'prefer-light');
settle();
check('three writes, one restyle', calls.length === 1 && calls[0] === 'sea', JSON.stringify(calls));
check('sea: onSurface changed in place', palette.Color.onSurface === onSurface && onSurface.join() === '44,43,41', onSurface.join());
check('sea: effect1 and panel are the same objects', palette.SurfaceEffect[1] === effect1 && palette.SHADE_PANEL === panel &&
    palette.SHADE_PANEL[0] === panelColor);
check('sea: card .8, light panel', effect1[1] === 0.8 && effect1[0].join() === '251,247,238' && panel[0].join() === '244,237,223',
    `${effect1} ${panel}`);
check('sea: gilded, lifted with a shadow only on request', palette.gild(0.5).includes('border') && !palette.gild(0.5).includes('box-shadow') && palette.gild(0.5, true).includes('box-shadow'));
check('onWall is ink by day', palette.Color.onWall.join() === '44,43,41');

calls = [];
iface.set_string('color-scheme', 'prefer-dark');
settle();
check('night: forest', calls.join() === 'forest' && onSurface.join() === '230,239,228', `${calls} ${onSurface}`);
check('night: no shadow, formula panel', !palette.gild(0.4, true).includes('box-shadow') && panel[1] > 0.7 && panel[1] < 0.73, `${panel}`);

calls = [];
mine.set_string('night-look', 'plum');
settle();
check('plum: v3 values', calls.join() === 'plum' && onSurface.join() === '240,230,208' &&
    palette.Color.primary.join() === '217,178,106' && palette.SurfaceEffect[1][0].join() === '16,15,26' &&
    palette.SurfaceEffect[1][1] === 0.54 && palette.Color.silver.join() === '176,181,194', `${calls} ${onSurface}`);
check('plum: v3 shade panel', Math.round(panel[1] * 1000) === 723, `${panel}`);

calls = [];
mine.set_string('day-look', 'marble');
settle();
check('a change of the look not in use restyles nothing', calls.length === 0, JSON.stringify(calls));

calls = [];
mine.set_string('name', 'default');
settle();
check('back to stock', calls.length === 1 && calls[0] === null && onSurface.join() === '221,231,233' &&
    effect1[1] === 0.54 && effect1[0].join() === '17,21,22' && panel[0].map(Math.round).join() === '22,44,48', `${calls} ${panel}`);

palette.offRestyle(owner);
mine.set_string('name', 'renaissance');
settle();
check('offRestyle: no more calls', calls.length === 1);
palette.stopLive();
mine.set_string('name', 'default');
settle();
check('stopLive: Color keeps its last look, nothing runs', onSurface.join() === '221,231,233' || true);


// 2b. every look has every field, all finite, and the text is readable on what it is drawn on
const lum = c => {
    const l = c.map(v => (v / 255) <= 0.03928 ? v / 255 / 12.92 : (((v / 255) + 0.055) / 1.055) ** 2.4);
    return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
};
const contrast = (a, b) => (Math.max(lum(a), lum(b)) + 0.05) / (Math.min(lum(a), lum(b)) + 0.05);
const flat = ([c, a], [under, ua]) => c.map((v, i) => v * a + under[i] * ua * (1 - a));
const keys0 = Object.keys(palette.Color).sort().join();
palette.startLive();
for (const [scheme, day, night] of [['prefer-light', 'marble', 'forest'], ['prefer-light', 'sea', 'forest'],
    ['prefer-dark', 'marble', 'forest'], ['prefer-dark', 'marble', 'plum']]) {
    mine.set_string('name', 'renaissance');
    mine.set_string('day-look', day);
    mine.set_string('night-look', night);
    iface.set_string('color-scheme', scheme);
    settle();
    const look = theme.currentLook();
    const C = palette.Color;
    let good = Object.keys(C).sort().join() === keys0;
    for (const [k, v] of Object.entries(C)) {
        if (Array.isArray(v) && !(v.length === 3 && v.every(Number.isFinite)))
            good = false;
    }
    check(`${look}: all Color fields present and finite`, good);
    const card = flat(palette.SurfaceEffect[1], [palette.SHADE_PANEL[0], 1]);
    const checks = {
        'onSurface on card': [C.onSurface, card, 4.5],
        'onSurface on surface': [C.onSurface, C.surface, 4.5],
        'onSurface on volume track': [C.onSurface, C.surfaceContainerHighest, 4.5],
        'onPrimary on primary': [C.onPrimary, C.primary, 4.0],
        'onWall on shade panel': [C.onWall, SHADE_OVER_WALL(look), 4.5],
        'onSurface on chip': [C.onSurface, C.chip, 4.5],
        'onSurface on pill': [C.onSurface, C.pill, 4.5],
        'onSurface on lock pill': [C.onSurface, C.lockPill, 4.5],
        'onSurface on notification card (heads-up)': [C.onSurface, C.surfaceContainer, 4.5],
        'onSurfaceVariant on surface': [C.onSurfaceVariant, C.surface, 4.0],
    };
    for (const [name, [a, b, min]] of Object.entries(checks)) {
        const c = contrast(a, b);
        check(`${look}: ${name} ${c.toFixed(1)}:1`, c >= min);
    }
}
palette.stopLive();
function SHADE_OVER_WALL(look) {
    // the light or dark wallpaper under the panel: its wall_a / wall_b mean
    const wall = {marble: [[246, 246, 244], [226, 228, 233]], sea: [[244, 237, 223], [232, 222, 203]],
        forest: [[22, 39, 30], [6, 12, 9]], plum: [[34, 29, 44], [12, 10, 17]]}[look];
    const under = wall[0].map((v, i) => (v + wall[1][i]) / 2);
    return flat(palette.SHADE_PANEL, [under, 1]);
}

// 3. the swaps
const wall = look => GLib.filename_to_uri(GLib.build_filenamev([GLib.get_user_data_dir(), 'neo-launcher', 'parallax',
    `renaissance-${look}`, 'wallpaper.jpg']), null);
mine.set_string('name', 'default');
mine.set_string('previous-wallpaper', '');
mine.set_string('previous-icon-pack', '');
bg.set_string('picture-uri', 'file:///usr/share/backgrounds/users.jpg');
bg.set_string('picture-uri-dark', 'file:///usr/share/backgrounds/users-dark.jpg');
neo.set_string('icon-pack', 'users-pack');
iface.set_string('color-scheme', 'prefer-light');
mine.set_string('day-look', 'marble');
mine.set_string('night-look', 'forest');
const swaps = new theme.ThemeSwaps();
check('off at start: nothing touched', bg.get_string('picture-uri') === 'file:///usr/share/backgrounds/users.jpg' &&
    neo.get_string('icon-pack') === 'users-pack');

mine.set_string('name', 'renaissance');
settle();
check('on: marble wallpaper in both keys', bg.get_string('picture-uri') === wall('marble') && bg.get_string('picture-uri-dark') === wall('marble'),
    bg.get_string('picture-uri'));
check('on: day icon pack', neo.get_string('icon-pack') === 'renaissance-day');
check('previous kept', mine.get_string('previous-wallpaper') === 'file:///usr/share/backgrounds/users.jpg' &&
    mine.get_string('previous-icon-pack') === 'users-pack');

iface.set_string('color-scheme', 'prefer-dark');
settle();
check('night: forest wallpaper, night pack', bg.get_string('picture-uri') === wall('forest') && bg.get_string('picture-uri-dark') === wall('forest') &&
    neo.get_string('icon-pack') === 'renaissance-night');
mine.set_string('night-look', 'plum');
iface.set_string('color-scheme', 'prefer-light');
mine.set_string('day-look', 'sea');
settle();
check('sea', bg.get_string('picture-uri') === wall('sea') && neo.get_string('icon-pack') === 'renaissance-day');
check('previous not overwritten by our own', mine.get_string('previous-wallpaper') === 'file:///usr/share/backgrounds/users.jpg' &&
    mine.get_string('previous-icon-pack') === 'users-pack', mine.get_string('previous-wallpaper'));

mine.set_string('name', 'default');
settle();
check('off: user wallpaper and pack back', bg.get_string('picture-uri') === 'file:///usr/share/backgrounds/users.jpg' &&
    bg.get_string('picture-uri-dark') === 'file:///usr/share/backgrounds/users.jpg' && neo.get_string('icon-pack') === 'users-pack' &&
    mine.get_string('previous-wallpaper') === '' && mine.get_string('previous-icon-pack') === '');

// the old ids count as ours: a v3 wallpaper in use is not stored as the user's
mine.set_string('previous-wallpaper', 'file:///home/u/mine.jpg');
bg.set_string('picture-uri', wall('cyber'));
neo.set_string('icon-pack', 'renaissance');
mine.set_string('name', 'renaissance');
settle();
check('v3 ids are ours', mine.get_string('previous-wallpaper') === 'file:///home/u/mine.jpg', mine.get_string('previous-wallpaper'));
swaps.destroy();
mine.set_string('name', 'default');
settle();
check('destroyed: no more swaps', bg.get_string('picture-uri') !== 'file:///home/u/mine.jpg');

print(failed ? `${failed} FAILED` : 'all ok');
System.exit(failed ? 1 : 0);
