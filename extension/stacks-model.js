// Stacks: who owns which window. Pure logic, no GNOME imports, so node can test it (tests/stacks-model.test.mjs).
//
// A stack is an owner (the user, Minerva, any agent) and its windows. The user's stack is the default: a window
// nobody asked for is the user's. An agent asks for a window with Open: a claim {owner, app} that the first new
// window of that app consumes within CLAIM_MS. Windows a window opens (dialogs) are not tasks and do not count.
//
// Around the owner and its windows:
//  - a memory cap per stack (the shell measures, the model decides: claim refuses over the cap, overCap names it);
//  - the lock: while the phone is locked an agent sees only its own stack, no one moves the focus, and an agent's
//    window stays off the lock screen unless its grant says show_when_locked;
//  - the take-over: the user touches an agent's window and the agent may not close or focus it until they hand it
//    back (the shell draws the chip that does that);
//  - the graph: edges between windows, "opened from" (a fact) and "switched" (a count of the user's moves), kept in
//    memory only (a record of habits: it goes with the shell, it is never written down).
//
// Identity is a token the caller presents, from the config file (see parseConfig). Same-uid processes can read that
// file, so the tokens separate cooperating agents, they do not wall off a hostile one: the grants and the shell's
// own labelling are the real limits.

export const USER = 'user';
export const CLAIM_MS = 10000;
export const DEFAULT_MAX_WINDOWS = 8;
export const DEFAULT_MAX_MEMORY_MB = 1024;
export const MAX_EDGES = 400;

export class StackError extends Error {}

// config: {"agents": {"<name>": {"token": "...", "see": ["user"], "act": [], "focus": false, "max_windows": 8,
//                                "max_memory_mb": 1024, "show_when_locked": false}}}
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
            showWhenLocked: a.show_when_locked === true,
            maxWindows: Number.isInteger(a.max_windows) && a.max_windows > 0 ? a.max_windows : DEFAULT_MAX_WINDOWS,
            maxMemoryMb: Number.isInteger(a.max_memory_mb) && a.max_memory_mb > 0 ? a.max_memory_mb
                : DEFAULT_MAX_MEMORY_MB,
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
        this._claims = [];              // {owner, app, from, until}
        this._taken = new Set();        // window ids the user has taken over
        this._edges = new Map();        // "kind from>to" -> {kind, from, to, n, t}
        this.locked = false;            // the lock screen is up
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

    // the grants of an agent by its name (the shell asks about a window's owner), or null
    grantOf(owner) {
        for (const c of this._tokens.values()) {
            if (c.name === owner)
                return c;
        }
        return null;
    }

    // Locked, the phone shows an agent nothing of the user's or another agent's: only its own stack. An admin is the
    // owner's own command line.
    canSee(caller, owner) {
        if (caller.admin || caller.name === owner)
            return true;
        if (this.locked)
            return false;
        return caller.see.has('*') || caller.see.has(owner);
    }

    canAct(caller, owner) {
        return caller.admin || caller.name === owner || (!this.locked && (caller.act.has('*') || caller.act.has(owner)));
    }

    // ---- windows -----------------------------------------------------------------------------------------------

    // A new task window of app: the owner who asked for it, else the user. parent: the window it came from when
    // the shell knows (the one in front when it appeared); a claim's own "from" wins.
    assign(winId, app, parent = 0) {
        const now = this._now();
        this._claims = this._claims.filter(c => c.until > now);
        const i = this._claims.findIndex(c => c.app === app);
        const claim = i < 0 ? null : this._claims.splice(i, 1)[0];
        const owner = claim ? claim.owner : USER;
        this._owner.set(winId, owner);
        this._app.set(winId, app);
        const from = claim?.from || parent;
        if (from && from !== winId && this._owner.has(from))
            this._edge('opened_from', from, winId, 1);
        return owner;
    }

    release(winId) {
        this._owner.delete(winId);
        this._app.delete(winId);
        this._taken.delete(winId);
        for (const [key, e] of this._edges) {
            if (e.from === winId || e.to === winId)
                this._edges.delete(key);
        }
    }

    ownerOf(winId) {
        return this._owner.get(winId) ?? USER;
    }

    appOf(winId) {
        return this._app.get(winId) ?? '';
    }

    has(winId) {
        return this._owner.has(winId);
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

    // the owners the user's own screens show (recents' tabs): locked, only theirs
    uiOwners() {
        return this.locked ? [USER] : this.owners();
    }

    // ---- what an agent may ask --------------------------------------------------------------------------------

    // The agent opens an app in its own stack: records the claim, or throws (admins own nothing).
    //  from: a window id it opens the app from (0: none); it must see that window.
    //  usedMb: what its stack uses now, measured by the shell (0: not measured)
    claim(token, app, {from = 0, usedMb = 0} = {}) {
        const c = this.caller(token);
        if (c.admin)
            throw new StackError('an admin token owns no windows: use an agent token to open one');
        if (!app)
            throw new StackError('no app');
        if (from)
            this.authorizeSee(token, from);
        const live = this.windows(c.name).length;
        const pending = this._claims.filter(x => x.owner === c.name && x.until > this._now()).length;
        if (live + pending >= c.maxWindows)
            throw new StackError(`stack ${c.name} is full (${c.maxWindows} windows)`);
        if (usedMb >= c.maxMemoryMb)
            throw new StackError(`stack ${c.name} uses ${Math.round(usedMb)} MB of its ${c.maxMemoryMb} MB: close a window first`);
        this._claims.push({owner: c.name, app, from, until: this._now() + CLAIM_MS});
        return {owner: c.name, expires_ms: CLAIM_MS};
    }

    // the owner's cap in MB, or 0 when it has none (the user's stack is not capped)
    capMb(owner) {
        return this.grantOf(owner)?.maxMemoryMb ?? 0;
    }

    // The window to close to bring the owner's stack under its cap, or 0 when it is under: the newest, so what the
    // agent has been working in longest stays. usedMb: the stack's measured use.
    overCap(owner, usedMb) {
        const cap = this.capMb(owner);
        if (!cap || usedMb <= cap)
            return 0;
        return this.windows(owner).at(-1) ?? 0;
    }

    // the stacks a caller may see, as owner names (the user's first when visible)
    visible(token) {
        const c = this.caller(token);
        const all = this.owners();
        return all.filter(o => this.canSee(c, o));
    }

    // throws unless the caller may see the window; returns the owner
    authorizeSee(token, winId) {
        const c = this.caller(token);
        if (!this._owner.has(winId))
            throw new StackError(`no window ${winId}`);
        const owner = this.ownerOf(winId);
        if (!this.canSee(c, owner))
            throw new StackError(this.locked ? 'the phone is locked' : `not allowed to see stack ${owner}`);
        return owner;
    }

    // throws unless the caller may act on the window's stack; returns the owner. Not an admin (the owner's own
    // command line) on a window the user has taken over: they are using it.
    authorizeAct(token, winId) {
        const c = this.caller(token);
        const owner = this.ownerOf(winId);
        if (!this._owner.has(winId))
            throw new StackError(`no window ${winId}`);
        if (!this.canAct(c, owner))
            throw new StackError(this.locked ? 'the phone is locked' : `not allowed to act on stack ${owner}`);
        if (!c.admin && this._taken.has(winId))
            throw new StackError(`the user took over window ${winId}: wait until they hand it back`);
        return owner;
    }

    authorizeFocus(token, winId) {
        const owner = this.authorizeAct(token, winId);
        const c = this.caller(token);
        if (this.locked)
            throw new StackError('the phone is locked');
        if (!c.admin && !c.focus)
            throw new StackError('this token may not move the focus');
        return owner;
    }

    // ---- the lock screen ---------------------------------------------------------------------------------------

    // whether the window may be the one over the lock screen: the user's always (its app's own declaration decides),
    // an agent's only with the grant
    mayShowWhenLocked(winId) {
        const owner = this.ownerOf(winId);
        return owner === USER || this.grantOf(owner)?.showWhenLocked === true;
    }

    // ---- the take-over -----------------------------------------------------------------------------------------

    // The user has the window now (false: it is not an agent's, nothing to take). The agent's Close and Focus on it
    // are refused until resume.
    takeOver(winId) {
        if (!this._owner.has(winId) || this.ownerOf(winId) === USER)
            return false;
        this._taken.add(winId);
        return true;
    }

    resume(winId) {
        return this._taken.delete(winId);
    }

    isTakenOver(winId) {
        return this._taken.has(winId);
    }

    // ---- the graph ---------------------------------------------------------------------------------------------

    _edge(kind, from, to, n) {
        const key = `${kind} ${from}>${to}`;
        const e = this._edges.get(key);
        if (e) {
            e.n += n;
            e.t = this._now();
        } else {
            this._edges.set(key, {kind, from, to, n, t: this._now()});
        }
        if (this._edges.size > MAX_EDGES) {
            // the facts stay; the oldest count of moves goes
            let old = null;
            for (const x of this._edges.values()) {
                if (x.kind === 'switched' && (!old || x.t < old.t))
                    old = x;
            }
            if (old)
                this._edges.delete(`${old.kind} ${old.from}>${old.to}`);
        }
    }

    // the focus went from one task window to another (the user's move, or an agent's Focus)
    switched(from, to) {
        if (from && to && from !== to && this._owner.has(from) && this._owner.has(to))
            this._edge('switched', from, to, 1);
    }

    // The windows and the edges between them a caller may see: {nodes: [{id, owner, app, taken_over}],
    // edges: [{kind, from, to, n}]}. Both ends of an edge must be visible, else it is not shown (an edge to a hidden
    // window would say that the window exists).
    graph(token) {
        const c = this.caller(token);
        const nodes = [];
        const shown = new Set();
        for (const [id, owner] of this._owner) {
            if (this.canSee(c, owner)) {
                nodes.push({id, owner, app: this._app.get(id), taken_over: this._taken.has(id)});
                shown.add(id);
            }
        }
        const edges = [...this._edges.values()].filter(e => shown.has(e.from) && shown.has(e.to))
            .map(({kind, from, to, n}) => ({kind, from, to, n}));
        return {you: c.name, nodes, edges};
    }
}
