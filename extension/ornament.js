// Ornament of the Renaissance look (theme.js): fine silver hairlines and tick scales, the instrument-panel side of
// "Cyberpunk meets Italian Renaissance". Drawn once (Canvas redraws only when its size changes), nothing animates.
import GObject from 'gi://GObject';

import {Canvas} from './icons.js';
import {Color, setSource} from './palette.js';

const STEP = 5;             // logical px between ticks
const BAND = 14;            // the scale's height, logical px

// A scale across the width: a hairline with ticks every STEP px (long every 5th, a gold diamond at the middle) and
// a few tiny HUD squares at the right end. Under the stock look it is never built.
export const TickScale = GObject.registerClass(
class TickScale extends Canvas {
    constructor(width) {
        super({width, height: BAND});
    }

    draw(cr, width, height) {
        const y = Math.round(height / 2) + 0.5;
        const margin = 40;      // clear of the scrim's rounded corners
        cr.setLineWidth(1);
        setSource(cr, Color.silverDim, 0.55);
        cr.moveTo(margin, y);
        cr.lineTo(width - margin, y);
        cr.stroke();
        const n = Math.floor((width - 2 * margin) / STEP);
        const mid = margin + Math.round(n / 2) * STEP;
        for (let i = 0; i <= n; i++) {
            const x = margin + i * STEP + 0.5;
            const major = i % 5 === 0;
            const len = major ? 5 : 2.5;
            setSource(cr, major ? Color.silver : Color.silverDim, major ? 0.7 : 0.5);
            cr.moveTo(x, y - len);
            cr.lineTo(x, y + len);
            cr.stroke();
        }
        // a small gold diamond where the scale is centred
        setSource(cr, Color.primary, 0.9);
        cr.moveTo(mid, y - 4);
        cr.lineTo(mid + 4, y);
        cr.lineTo(mid, y + 4);
        cr.lineTo(mid - 4, y);
        cr.closePath();
        cr.fill();
        // HUD squares at the right end
        setSource(cr, Color.silver, 0.8);
        for (let k = 0; k < 3; k++)
            cr.rectangle(width - margin - 3 * (k + 1) * 3 - 1, y - 1.5, 3, 3);
        cr.fill();
    }
});

export const TICK_SCALE_HEIGHT = BAND;
