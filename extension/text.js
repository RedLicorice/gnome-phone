// Text as the reference phone draws it. Its Android has Bold text on (font weight adjustment +300): wherever the
// family has no instance that heavy, minikin takes its heaviest and Skia emboldens it, stroking each glyph's outline
// on top of the fill (SkScalerContext, kEmbolden_Flag). Measured on the recents chip ("Phone": text-medium 500 +
// fake bold, ink 1817 against 1759 modelled) and the Screenshot button (600 → 700 + fake bold, ink 3858 against
// 3857). Pango lays the text out; cairo fills and strokes its path the same way.
import GObject from 'gi://GObject';
import Pango from 'gi://Pango';
import PangoCairo from 'gi://PangoCairo';

import {Canvas} from './icons.js';
import {setSource} from './palette.js';
import {SCALE, screenPixels, sp} from './units.js';
import {isRenaissance} from './theme.js';

const FAMILY = 'Google Sans Flex';

// Skia's fake bold: the outline stroked at 1/24 of the size up to 9 px, 1/32 from 36 px (kStdFakeBoldInterpKeys)
export function fakeBoldWidth(sizePx) {
    const t = Math.min(Math.max((sizePx - 9) / 27, 0), 1);
    return sizePx * (1 / 24 + (1 / 32 - 1 / 24) * t);
}

// The Renaissance theme sets big text (the clocks) in Michelangelus, with the stock face as Pango's fallback
function description(sizePx, wght, {wdth = 100, rond = 100, opsz}, sizeSp) {
    if (isRenaissance() && sizeSp >= 24)
        return Pango.FontDescription.from_string(`Michelangelus, ${FAMILY} ${wght >= 600 ? 'Bold ' : ''}${sizePx.toFixed(3)}px`);
    return Pango.FontDescription.from_string(
        `${FAMILY} ${sizePx.toFixed(3)}px @wght=${wght},wdth=${wdth},ROND=${rond},opsz=${opsz}`);
}

let measureContext = null;

// One line of text, drawn in screen pixels. sizeSp, wght: the weight the phone actually draws (after its +300);
// embolden: whether Skia also fakes bold on it; letterSpacing in em
export const AndroidText = GObject.registerClass(
class AndroidText extends Canvas {
    constructor(text, sizeSp, wght, {colour, alpha = 1, embolden = false, letterSpacing = 0, opsz = sizeSp, wdth,
        rond} = {}) {
        super();
        this._sizePx = sp(sizeSp) * SCALE;
        this._font = description(this._sizePx, wght, {wdth, rond, opsz}, sizeSp);
        this._letterSpacing = letterSpacing;
        this._embolden = embolden ? fakeBoldWidth(this._sizePx) : 0;
        this._colour = colour;
        this._alpha = alpha;
        this.setText(text);
    }

    _layout(context) {
        const layout = Pango.Layout.new(context);
        layout.set_font_description(this._font);
        if (this._letterSpacing) {
            const attrs = new Pango.AttrList();
            attrs.insert(Pango.attr_letter_spacing_new(Math.round(this._letterSpacing * this._sizePx * Pango.SCALE)));
            layout.set_attributes(attrs);
        }
        layout.set_text(this._text, -1);
        return layout;
    }

    setText(text) {
        this._text = text ?? '';
        measureContext ??= PangoCairo.FontMap.get_default().create_context();
        const layout = this._layout(measureContext);
        this._measured = layout;
        const [ink, logical] = layout.get_pixel_extents();
        // screen px: the logical box, plus the stroke's reach past it
        const pad = Math.ceil(this._embolden / 2) + 1;
        this._pad = pad;
        this._box = {width: logical.width, height: logical.height, baseline: layout.get_baseline() / Pango.SCALE};
        this._ink = {x: ink.x, y: ink.y, width: ink.width, height: ink.height};
        this.set_size((logical.width + 2 * pad) / SCALE, (logical.height + 2 * pad) / SCALE);
        this.queue_repaint();
    }

    // in screen px from this actor's origin: where the text's logical box and baseline are
    get inset() {
        return this._pad;
    }

    get textWidth() {
        return this._box.width;
    }

    get textHeight() {
        return this._box.height;
    }

    get baseline() {
        return this._box.baseline;
    }

    // the ink's box, screen px from the logical box's origin
    get ink() {
        return this._ink;
    }

    setColour(colour, alpha = 1) {
        this._colour = colour;
        this._alpha = alpha;
        this.queue_repaint();
    }

    // The measured layout, not one made for cr: the shell's surfaces are at resource scale 3, so cr is 1.2x screen
    // pixels, and Pango laying the text out again at that scale rounds its advances differently, by up to 4% of the
    // width either way ("Wed, Nov 4" 251 px for 260, "17% • Charging" wider than its box and clipped). Drawing the
    // measured layout keeps the glyphs where they were measured.
    draw(cr) {
        screenPixels(cr);
        cr.moveTo(this._pad, this._pad);
        PangoCairo.layout_path(cr, this._measured);
        setSource(cr, this._colour, this._alpha);
        if (this._embolden) {
            cr.fillPreserve();
            cr.setLineWidth(this._embolden);
            cr.stroke();
        } else {
            cr.fill();
        }
    }
});
