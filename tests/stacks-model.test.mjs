// node --test tests/
import assert from 'node:assert/strict';
import {test} from 'node:test';

import {CLAIM_MS, DEFAULT_MAX_MEMORY_MB, MAX_EDGES, parseConfig, StackError, StackModel, USER} from '../extension/stacks-model.js';

const CONFIG = JSON.stringify({agents: {
    minerva: {token: 'm'.repeat(20), see: ['user'], act: [], max_windows: 2},
    bob: {token: 'b'.repeat(20)},
    cli: {token: 'c'.repeat(20), admin: true},
}});
const M = 'm'.repeat(20), B = 'b'.repeat(20), C = 'c'.repeat(20);

function setup() {
    let t = 1000;
    const model = new StackModel(parseConfig(CONFIG), () => t);
    return {model, advance: ms => (t += ms)};
}

test('config: short, duplicate and user tokens are refused', () => {
    assert.throws(() => parseConfig('{"agents": {"a": {"token": "short"}}}'), StackError);
    assert.throws(() => parseConfig(JSON.stringify({agents: {a: {token: M}, b: {token: M}}})), /unique/);
    assert.throws(() => parseConfig(JSON.stringify({agents: {user: {token: M}}})), /owner's stack/);
    assert.throws(() => parseConfig('nope'), /not JSON/);
    assert.equal(parseConfig('{}').size, 0);
});

test('an unclaimed window is the user\'s', () => {
    const {model} = setup();
    assert.equal(model.assign(1, 'de.yesman.Clock.desktop'), USER);
    assert.deepEqual(model.owners(), [USER]);
    assert.deepEqual(model.windows(USER), [1]);
});

test('a claim goes to the first new window of that app, once', () => {
    const {model} = setup();
    model.claim(M, 'a.desktop');
    assert.equal(model.assign(1, 'other.desktop'), USER);
    assert.equal(model.assign(2, 'a.desktop'), 'minerva');
    assert.equal(model.assign(3, 'a.desktop'), USER);
    assert.deepEqual(model.owners(), [USER, 'minerva']);
});

test('a claim expires', () => {
    const {model, advance} = setup();
    model.claim(M, 'a.desktop');
    advance(CLAIM_MS + 1);
    assert.equal(model.assign(1, 'a.desktop'), USER);
});

test('a full stack refuses, counting windows and pending claims; closing frees a slot', () => {
    const {model} = setup();
    model.claim(M, 'a.desktop');
    model.assign(1, 'a.desktop');
    model.claim(M, 'b.desktop');
    assert.throws(() => model.claim(M, 'c.desktop'), /full/);
    model.assign(2, 'b.desktop');
    assert.throws(() => model.claim(M, 'c.desktop'), /full/);
    model.release(1);
    model.claim(M, 'c.desktop');
});

test('an admin token owns no windows; an unknown token is refused', () => {
    const {model} = setup();
    assert.throws(() => model.claim(C, 'a.desktop'), /admin/);
    assert.throws(() => model.claim('x'.repeat(20), 'a.desktop'), /unknown token/);
    assert.throws(() => model.claim(M, ''), /no app/);
});

test('visibility: own stack always, others by grant, admin all', () => {
    const {model} = setup();
    model.claim(B, 'a.desktop');
    model.assign(1, 'a.desktop');
    model.claim(M, 'b.desktop');
    model.assign(2, 'b.desktop');
    model.assign(3, 'c.desktop');
    assert.deepEqual(model.visible(M), [USER, 'minerva']);     // see: ["user"], not bob's
    assert.deepEqual(model.visible(B), ['bob']);               // no grants: only its own
    assert.deepEqual(model.visible(C), [USER, 'bob', 'minerva']);
});

test('acting: own stack yes, another needs act grant, focus needs its own', () => {
    const {model} = setup();
    model.claim(B, 'a.desktop');
    model.assign(1, 'a.desktop');
    model.assign(2, 'u.desktop');
    assert.equal(model.authorizeAct(B, 1), 'bob');
    assert.throws(() => model.authorizeAct(B, 2), /not allowed/);
    assert.throws(() => model.authorizeAct(M, 1), /not allowed/);     // minerva sees the user, not bob; acts nowhere else
    assert.throws(() => model.authorizeAct(B, 99), /no window/);
    assert.throws(() => model.authorizeFocus(B, 1), /focus/);
    assert.equal(model.authorizeFocus(C, 2), USER);                   // admin
});

test('owners keep the order of their first window; an emptied stack leaves', () => {
    const {model} = setup();
    model.claim(B, 'a.desktop');
    model.assign(1, 'a.desktop');
    model.claim(M, 'b.desktop');
    model.assign(2, 'b.desktop');
    assert.deepEqual(model.owners(), [USER, 'bob', 'minerva']);
    model.release(1);
    assert.deepEqual(model.owners(), [USER, 'minerva']);
});

// ---- memory caps, the lock, the take-over, the graph ---------------------------------------------------------------

test('config: memory cap and show_when_locked, with defaults', () => {
    const cfg = parseConfig(JSON.stringify({agents: {
        a: {token: M, max_memory_mb: 300, show_when_locked: true}, b: {token: B}}}));
    assert.equal(cfg.get(M).maxMemoryMb, 300);
    assert.equal(cfg.get(M).showWhenLocked, true);
    assert.equal(cfg.get(B).maxMemoryMb, DEFAULT_MAX_MEMORY_MB);
    assert.equal(cfg.get(B).showWhenLocked, false);
    assert.equal(parseConfig(JSON.stringify({agents: {a: {token: M, max_memory_mb: -5}}})).get(M).maxMemoryMb,
        DEFAULT_MAX_MEMORY_MB);
});

test('memory cap: a claim over it is refused; overCap names the newest window, never the user\'s stack', () => {
    const {model} = setup();
    const cap = model.grantOf('minerva').maxMemoryMb;
    model.claim(M, 'a.desktop', {usedMb: cap - 1});
    assert.throws(() => model.claim(M, 'b.desktop', {usedMb: cap}), /MB of its/);
    model.assign(1, 'a.desktop');
    model.claim(M, 'b.desktop');
    model.assign(2, 'b.desktop');
    assert.equal(model.overCap('minerva', cap), 0);
    assert.equal(model.overCap('minerva', cap + 1), 2);
    model.assign(3, 'u.desktop');
    assert.equal(model.overCap(USER, 1e9), 0);
    assert.equal(model.capMb(USER), 0);
});

test('locked: an agent sees only its own stack, acts nowhere else, nobody moves the focus; unlocked it is back', () => {
    const {model} = setup();
    model.claim(M, 'a.desktop');
    model.assign(1, 'a.desktop');
    model.assign(2, 'u.desktop');
    model.locked = true;
    assert.deepEqual(model.visible(M), ['minerva']);
    assert.deepEqual(model.visible(C), [USER, 'minerva']);        // the owner's command line sees all
    assert.throws(() => model.authorizeSee(M, 2), /locked/);
    assert.equal(model.authorizeSee(M, 1), 'minerva');
    assert.throws(() => model.authorizeFocus(C, 1), /locked/);
    assert.deepEqual(model.uiOwners(), [USER]);
    model.locked = false;
    assert.deepEqual(model.visible(M), [USER, 'minerva']);
    assert.deepEqual(model.uiOwners(), [USER, 'minerva']);
});

test('locked: an act grant on another stack lapses', () => {
    const cfg = parseConfig(JSON.stringify({agents: {a: {token: M, act: ['bob']}, bob: {token: B}}}));
    const model = new StackModel(cfg, () => 1);
    model.claim(B, 'x.desktop');
    model.assign(1, 'x.desktop');
    assert.equal(model.authorizeAct(M, 1), 'bob');
    model.locked = true;
    assert.throws(() => model.authorizeAct(M, 1), /locked/);
});

test('show when locked: the user\'s window by its app, an agent\'s only with the grant', () => {
    const cfg = parseConfig(JSON.stringify({agents: {a: {token: M, show_when_locked: true}, b: {token: B}}}));
    const model = new StackModel(cfg, () => 1);
    model.claim(M, 'x.desktop');
    model.assign(1, 'x.desktop');
    model.claim(B, 'y.desktop');
    model.assign(2, 'y.desktop');
    model.assign(3, 'z.desktop');
    assert.equal(model.mayShowWhenLocked(1), true);
    assert.equal(model.mayShowWhenLocked(2), false);
    assert.equal(model.mayShowWhenLocked(3), true);
});

test('take-over: refuses the agent\'s Close and Focus, not the owner\'s command line; resume gives it back', () => {
    const {model} = setup();
    model.claim(B, 'a.desktop');
    model.assign(1, 'a.desktop');
    model.assign(2, 'u.desktop');
    assert.equal(model.takeOver(2), false);                       // the user's own window: nothing to take
    assert.equal(model.takeOver(99), false);
    assert.equal(model.takeOver(1), true);
    assert.equal(model.isTakenOver(1), true);
    assert.throws(() => model.authorizeAct(B, 1), /took over/);
    assert.equal(model.authorizeAct(C, 1), 'bob');                // admin
    assert.equal(model.resume(1), true);
    assert.equal(model.authorizeAct(B, 1), 'bob');
    model.takeOver(1);
    model.release(1);
    assert.equal(model.isTakenOver(1), false);
});

test('graph: opened-from by the claim or by the front window, switches counted, hidden ends hide the edge', () => {
    const {model} = setup();
    model.assign(1, 'u1.desktop');
    model.assign(2, 'u2.desktop', 1);                              // came from 1
    model.claim(M, 'a.desktop', {from: 2});
    assert.equal(model.assign(3, 'a.desktop', 1), 'minerva');     // the claim's from wins over the front window
    model.claim(B, 'b.desktop');
    model.assign(4, 'b.desktop', 3);                               // bob's, came from minerva's
    model.switched(1, 2);
    model.switched(1, 2);
    model.switched(2, 2);                                          // not a move
    model.switched(2, 99);                                         // not a window
    const m = model.graph(M);                                      // minerva sees user and herself, not bob
    assert.deepEqual(m.nodes.map(n => n.id), [1, 2, 3]);
    assert.deepEqual(m.edges, [
        {kind: 'opened_from', from: 1, to: 2, n: 1},
        {kind: 'opened_from', from: 2, to: 3, n: 1},
        {kind: 'switched', from: 1, to: 2, n: 2}]);
    const b = model.graph(B);                                      // bob sees only himself: no edge
    assert.deepEqual(b.nodes.map(n => n.id), [4]);
    assert.deepEqual(b.edges, []);
    assert.equal(model.graph(C).edges.length, 4);
    model.release(2);
    assert.deepEqual(model.graph(C).edges, [{kind: 'opened_from', from: 3, to: 4, n: 1}]);
});

test('graph: a claim may only name a window the agent can see; locked, the user\'s windows are hidden', () => {
    const {model} = setup();
    model.assign(1, 'u.desktop');
    model.claim(B, 'a.desktop');
    model.assign(2, 'a.desktop');
    assert.throws(() => model.claim(B, 'c.desktop', {from: 1}), /not allowed to see/);
    assert.throws(() => model.claim(B, 'c.desktop', {from: 99}), /no window/);
    model.claim(M, 'd.desktop', {from: 1});
    model.locked = true;
    assert.throws(() => model.claim(M, 'e.desktop', {from: 1}), /locked/);
});

test('graph: the switch counts are bounded, the facts stay', () => {
    const {model, advance} = setup();
    model.assign(1, 'u.desktop');
    model.claim(M, 'a.desktop', {from: 1});
    model.assign(2, 'a.desktop');
    for (let i = 3; i < 3 + MAX_EDGES + 20; i++) {
        advance(1);
        model.assign(i, 'u.desktop');
        model.switched(1, i);
    }
    const edges = model.graph(C).edges;
    assert.ok(edges.length <= MAX_EDGES);
    assert.ok(edges.some(e => e.kind === 'opened_from' && e.to === 2));
});
