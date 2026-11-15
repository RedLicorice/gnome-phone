// The owl of the Renaissance looks, alone (no frame, no plate): the Minerva agent's mark on its stack chip and on its
// notifications. images/owl.png is the cut-out of the statue owl; it has a faint warm glow at night and a soft shadow
// by day (the mockup's drop-shadow).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {restyleActor} from './palette.js';
import {isDay} from './theme.js';

const OWL = GLib.build_filenamev([GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]), 'images', 'owl.png']);

function glow() {
    return isDay() ? 'icon-shadow: 0 1px 2px rgba(40, 44, 60, 0.35);' : 'icon-shadow: 0 0 6px rgba(255, 214, 140, 0.35);';
}

// The owl as an icon actor, @size logical px wide; its shadow follows the look
export function owlIcon(size) {
    const icon = new St.Icon({gicon: new Gio.FileIcon({file: Gio.File.new_for_path(OWL)}), icon_size: Math.round(size)});
    icon.style = glow();
    restyleActor(icon, () => {
        icon.style = glow();
    });
    return icon;
}
