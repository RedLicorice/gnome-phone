// What the camera indicator says (camera.js draws it): the words for a camera in use, from the state minerva-camwatch
// publishes (org.minerva.Camera1, eyes/minerva_eyes/camwatch.py). No shell imports: it runs under plain gjs too
// (tests/camera.test.js).

const PURPOSE = {
    who: 'Sto riconoscendo chi è davanti alla fotocamera',
    look: 'Sto guardando attraverso la fotocamera',
    preview: 'Anteprima della fotocamera',
    watch: 'Sto osservando chi passa',
    greeter: 'Saluto al risveglio dello schermo',
};

// Known programs by their process name
const APPS = {
    'neo-camera': 'Fotocamera',
    'snapshot': 'Snapshot',
    'gst-launch-1.0': 'gst-launch',
};

// The state camwatch sent, or an empty one for a bad or missing document
export function parseState(text) {
    try {
        const state = JSON.parse(text);
        return {active: Array.isArray(state.active) ? state.active : [], history: Array.isArray(state.history) ? state.history : []};
    } catch (e) {
        return {active: [], history: []};
    }
}

// Who has the camera: Minerva for its own captures, else the program (or "Un'app")
export function owner(entry) {
    if (entry.source === 'minerva')
        return 'Minerva';
    const names = (entry.who ?? '').split(', ').filter(Boolean).map(n => APPS[n] ?? n);
    return names.join(', ') || "Un'app";
}

// What it is doing: "Sto imparando il volto di Giuseppe" for an enrolment, the tool's line for Minerva's other
// captures, "usa la fotocamera <which>" for another app
export function doing(entry) {
    if (entry.source !== 'minerva')
        return `sta usando la fotocamera ${entry.label}`;
    if (entry.purpose === 'enroll')
        return `Sto imparando il volto di ${entry.detail || 'una persona'}`;
    if (entry.purpose === 'enroll_pet')
        return `Sto imparando a riconoscere ${entry.detail || 'una bestiola'}`;
    return PURPOSE[entry.purpose] ?? 'Minerva sta usando la fotocamera';
}

const fixed = score => score.toFixed(2).replace('.', ',');

// "Giuseppe (0,71), sconosciuto (0,31)"; the capture's note ("nessun volto", "campione 2 di 5") when it has no face
export function facesText(entry) {
    const faces = entry.faces ?? [];
    if (faces.length)
        return faces.map(f => `${f.name === 'unknown' ? 'sconosciuto' : f.name} (${fixed(f.score)})`).join(', ');
    return entry.note ?? '';
}

// HH:MM in the local time of a Unix time in seconds
export function clock(seconds) {
    const d = new Date(seconds * 1000);
    return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

// What the sheet's release button says: Minerva's capture lets go of the camera, another app is closed
export function releaseLabel(entry) {
    return entry.source === 'minerva' ? 'Rilascia fotocamera' : `Chiudi ${owner(entry)}`;
}

// The line of a past capture: "22:41  Minerva · Sto riconoscendo chi è davanti alla fotocamera · Giuseppe (0,71)"
export function historyLine(entry) {
    const parts = [owner(entry), entry.source === 'minerva' ? doing(entry) : `fotocamera ${entry.label}`];
    const faces = facesText(entry);
    if (faces)
        parts.push(faces);
    return `${clock(entry.start)}  ${parts.join(' · ')}`;
}
