// Stacks: who owns which window. Pure logic, no GNOME imports, so node can test it (tests/stacks-model.test.mjs).
//
// A stack is an owner (the user, Minerva, any agent) and its windows. The user's stack is the default: a window
// nobody asked for is the user's. An agent asks for a window with Open: a claim {owner, app} that the first new
// window of that app consumes within CLAIM_MS. Windows a window opens (dialogs) are not tasks and do not count.
//
// Identity is a token the caller presents, from the config file (see parseConfig). Same-uid processes can read that
// file, so the tokens separate cooperating agents, they do not wall off a hostile one: the grants and the shell's
// own labelling are the real limits.

export const USER = 'user';
export const CLAIM_MS = 10000;
export const DEFAULT_MAX_WINDOWS = 8;

export class StackError extends Error {}

// config: {"agents": {"<name>": {"token": "...", "see": ["user"], "act": [], "focus": false, "max_windows": 8}}}
// An agent always sees and acts on its own stack. see/act name other stacks ("*" is every one). "admin": true is the
// owner's own command line: sees and acts on everything and owns no windows.
export function parseConfig(text) {
    let raw;
    try {
        raw = JSON.parse(text);
    } catch (e) {
        throw new StackError(`stacks config is not JSON: ${e.message}`);
    }
    const byToken = new Map();
    for (const [name, a] of Object.entries(raw?.agents ?? {})) {
        if (name === USER)
            throw new StackError(`"${USER}" is the owner's stack, not an agent`);
        if (typeof a?.token !== 'string' || a.token.length < 16)
            throw new StackError(`agent ${name}: token must be a string of at least 16 characters`);
        if (byToken.has(a.token))
            throw new StackError(`agent ${name}: token is not unique`);
        byToken.set(a.token, {
            name,
            see: new Set(a.see ?? []),
            act: new Set(a.act ?? []),
            focus: a.focus === true,
            admin: a.admin === true,
            maxWindows: Number.isInteger(a.max_windows) && a.max_windows > 0 ? a.max_windows : DEFAULT_MAX_WINDOWS,
        });
    }
    return byToken;
}

export class StackModel {
    // byToken: parseConfig's result; now: () => ms
    constructor(byToken = new Map(), now = () => Date.now()) {
        this._tokens = byToken;
        this._now = now;
        this._owner = new Map();        // window id -> owner
        this._app = new Map();          // window id -> app id
        this._claims = [];              // {owner, app, until}
    }

    setTokens(byToken) {
        this._tokens = byToken;
    }

    // who the token is; throws for an unknown one
    caller(token) {
        const c = this._tokens.get(token);
        if (!c)
            throw new StackError('unknown token');
        return c;
    }

    canSee(caller, owner) {
        return caller.admin || caller.name === owner || caller.see.has('*') || caller.see.has(owner);
    }

    canAct(caller, owner) {
        return caller.admin || caller.name === owner || caller.act.has('*') || caller.act.has(owner);
    }

    // ---- windows -----------------------------------------------------------------------------------------------

    // a new task window of app: the owner who asked for it, else the user
    assign(winId, app) {
        const now = this._now();
        this._claims = this._claims.filter(c => c.until > now);
        const i = this._claims.findIndex(c => c.app === app);
        const owner = i < 0 ? USER : this._claims.splice(i, 1)[0].owner;
        this._owner.set(winId, owner);
        this._app.set(winId, app);
        return owner;
    }

    release(winId) {
        this._owner.delete(winId);
        this._app.delete(winId);
    }

    ownerOf(winId) {
        return this._owner.get(winId) ?? USER;
    }

    windows(owner) {
        return [...this._owner].filter(([, o]) => o === owner).map(([id]) => id);
    }

    // the owners that have a window, the user first, the rest in the order they got their first one
    owners() {
        const out = [USER];
        for (const o of this._owner.values()) {
            if (!out.includes(o))
                out.push(o);
        }
        return out;
    }

    // ---- what an agent may ask --------------------------------------------------------------------------------

    // the agent opens an app in its own stack: records the claim, or throws (admins own nothing)
    claim(token, app) {
        const c = this.caller(token);
        if (c.admin)
            throw new StackError('an admin token owns no windows: use an agent token to open one');
        if (!app)
            throw new StackError('no app');
        const live = this.windows(c.name).length;
        const pending = this._claims.filter(x => x.owner === c.name && x.until > this._now()).length;
        if (live + pending >= c.maxWindows)
            throw new StackError(`stack ${c.name} is full (${c.maxWindows} windows)`);
        this._claims.push({owner: c.name, app, until: this._now() + CLAIM_MS});
        return {owner: c.name, expires_ms: CLAIM_MS};
    }

    // the stacks a caller may see, as owner names (the user's first when visible)
    visible(token) {
        const c = this.caller(token);
        const all = this.owners();
        return all.filter(o => this.canSee(c, o));
    }

    // throws unless the caller may act on the window's stack; returns the owner
    authorizeAct(token, winId) {
        const c = this.caller(token);
        const owner = this.ownerOf(winId);
        if (!this._owner.has(winId))
            throw new StackError(`no window ${winId}`);
        if (!this.canAct(c, owner))
            throw new StackError(`not allowed to act on stack ${owner}`);
        return owner;
    }

    authorizeFocus(token, winId) {
        const owner = this.authorizeAct(token, winId);
        const c = this.caller(token);
        if (!c.admin && !c.focus)
            throw new StackError('this token may not move the focus');
        return owner;
    }
}
