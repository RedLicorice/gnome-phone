// node --test tests/
import assert from 'node:assert/strict';
import {test} from 'node:test';

import {CLAIM_MS, parseConfig, StackError, StackModel, USER} from '../extension/stacks-model.js';

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
