// Android's vector icons (icondata.js, generated from the drawables) painted with Cairo in one colour, plus an
// St.DrawingArea that shows one.
import GObject from 'gi://GObject';
import St from 'gi://St';

import {ICONS} from './icondata.js';
import {restyleActor, setSource} from './palette.js';

export function hasIcon(name) {
    return name in ICONS;
}

// GJS's cairo has no matrices: the group transform m = [xx, yx, xy, yy, x0, y0] is applied to the points here
function tracePath(cr, path, m) {
    const [xx, yx, xy, yy, x0, y0] = m ?? [1, 0, 0, 1, 0, 0];
    const tx = (x, y) => xx * x + xy * y + x0;
    const ty = (x, y) => yx * x + yy * y + y0;
    cr.newPath();
    for (let i = 0; i < path.length;) {
        const p = path;
        switch (p[i]) {
        case 'M':
            cr.moveTo(tx(p[i + 1], p[i + 2]), ty(p[i + 1], p[i + 2]));
            i += 3;
            break;
        case 'L':
            cr.lineTo(tx(p[i + 1], p[i + 2]), ty(p[i + 1], p[i + 2]));
            i += 3;
            break;
        case 'C':
            cr.curveTo(tx(p[i + 1], p[i + 2]), ty(p[i + 1], p[i + 2]),
                tx(p[i + 3], p[i + 4]), ty(p[i + 3], p[i + 4]),
                tx(p[i + 5], p[i + 6]), ty(p[i + 5], p[i + 6]));
            i += 7;
            break;
        case 'Z':
            cr.closePath();
            i += 1;
            break;
        default:
            i += 1;
        }
    }
}

const CAPS = {butt: 0, round: 1, square: 2};
const JOINS = {miter: 0, round: 1, bevel: 2};

// Paint icon @name into the box (x, y, size, size) of @cr's user space, fit and centred, in @color at @alpha
export function paintIcon(cr, name, x, y, size, color, alpha = 1) {
    const icon = ICONS[name];
    if (!icon)
        return;
    const s = size / Math.max(icon.w, icon.h);
    cr.save();
    cr.translate(x + (size - icon.w * s) / 2, y + (size - icon.h * s) / 2);
    cr.scale(s, s);
    for (const op of icon.ops) {
        switch (op[0]) {
        case 'push':
            cr.save();
            break;
        case 'pop':
            cr.restore();
            break;
        case 'clip':
            tracePath(cr, op[1], op[2]);
            cr.clip();
            break;
        case 'fill':
            tracePath(cr, op[1], op[4]);
            cr.setFillRule(op[3] ? 1 : 0);
            setSource(cr, color, alpha * op[2]);
            cr.fill();
            break;
        case 'stroke': {
            const [, path, a, width, cap, join, miter, m] = op;
            tracePath(cr, path, m);
            cr.save();
            cr.setLineWidth(m ? width * Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) : width);
            cr.setLineCap(CAPS[cap] ?? 0);
            cr.setLineJoin(JOINS[join] ?? 0);
            cr.setMiterLimit(miter ?? 4);
            setSource(cr, color, alpha * a);
            cr.stroke();
            cr.restore();
            break;
        }
        }
    }
    cr.restore();
}

// St.DrawingArea draws its picture again on every allocation, a move as much as a resize, and each drawing is a new
// texture that Mesa converts on the CPU on its way to the GPU. The QS tiles' icons move on every frame of the shade
// as the tiles squish, and drawing them again was half the shell's main thread there; and every icon in the shade was
// allocated anew as it showed, 10 ms of texture uploads in its first frame (2026-11-04). A Canvas draws again only
// when its size changes or it is asked to (queue_repaint). The size of a box that moved, x2 - x1, is off in its last
// bits as often as not (46.400005, 46.399998 for 46.4): that is no change. Subclasses draw in draw(cr, width, height),
// in logical px.
export const Canvas = GObject.registerClass(
class Canvas extends St.DrawingArea {
    constructor(params) {
        super(params);
        this._width = this._height = -1;
        // the look's colours changed in place (palette.js): paint again
        restyleActor(this, () => this.queue_repaint());
    }

    vfunc_allocate(box) {
        const [width, height] = box.get_size();
        if (Math.abs(width - this._width) <= 1e-3 && Math.abs(height - this._height) <= 1e-3) {
            this.set_allocation(box);
            return;
        }
        this._width = width;
        this._height = height;
        super.vfunc_allocate(box);
    }

    vfunc_repaint() {
        const cr = this.get_context();
        const [width, height] = this.get_surface_size();
        this.draw(cr, width, height);
        cr.$dispose();
    }

    draw(_cr, _width, _height) {
    }
});

// One icon as an actor, size in logical pixels
export const VectorIcon = GObject.registerClass(
class VectorIcon extends Canvas {
    constructor(name, size, color, alpha = 1) {
        super({width: size, height: size});
        this._name = name;
        this._color = color;
        this._alpha = alpha;
    }

    set name(name) {
        if (this._name === name)
            return;
        this._name = name;
        this.queue_repaint();
    }

    get name() {
        return this._name;
    }

    setColor(color, alpha = 1) {
        if (alpha === this._alpha && color.join() === this._color.join())
            return;
        this._color = color;
        this._alpha = alpha;
        this.queue_repaint();
    }

    draw(cr, w, h) {
        cr.setOperator(0);
        cr.paint();
        cr.setOperator(2);
        const size = Math.min(w, h);
        paintIcon(cr, this._name, (w - size) / 2, (h - size) / 2, size, this._color, this._alpha);
    }
});
