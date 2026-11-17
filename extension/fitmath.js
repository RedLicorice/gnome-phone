// How much a window must shrink to lie wholly inside an area: plain numbers, no shell library, so a test can run it.

// @width, @height: the window's frame; @areaWidth, @areaHeight: the room it has. 1 when it fits already (a window is
// never made bigger here), and when a size is not a positive number there is nothing to fit.
export function fitScale(width, height, areaWidth, areaHeight) {
    if (!(width > 0) || !(height > 0) || !(areaWidth > 0) || !(areaHeight > 0))
        return 1;
    return Math.min(1, areaWidth / width, areaHeight / height);
}

// Where the buffer's top left goes so a frame of @frame (x, y, width, height in stage pixels, inside the buffer at
// @buffer) comes out at @scale in the middle of @area: the translation of an actor scaled about its own top left.
export function fitOffset(scale, frame, buffer, area) {
    return {
        x: Math.round(area.x + (area.width - frame.width * scale) / 2 - buffer.x - (frame.x - buffer.x) * scale),
        y: Math.round(area.y + (area.height - frame.height * scale) / 2 - buffer.y - (frame.y - buffer.y) * scale),
    };
}
