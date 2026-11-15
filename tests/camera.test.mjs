// node --test tests/
// The camera indicator's words (extension/cameratext.js), including the release button's label.
import assert from 'node:assert/strict';
import {test} from 'node:test';

import {doing, owner, releaseLabel} from '../extension/cameratext.js';

const minerva = {source: 'minerva', who: 'Minerva', purpose: 'enroll_pet', detail: 'Silvio', label: 'principale'};
const app = {source: 'system', who: 'neo-camera', purpose: '', detail: '', label: 'frontale'};

test('a Minerva capture is released, an app is closed', () => {
    assert.equal(releaseLabel(minerva), 'Rilascia fotocamera');
    assert.equal(releaseLabel(app), 'Chiudi Fotocamera');
});

test('an unnamed app is still closable', () => {
    assert.equal(releaseLabel({...app, who: ''}), "Chiudi Un'app");
});

test('what a Minerva capture is doing', () => {
    assert.equal(doing(minerva), 'Sto imparando a riconoscere Silvio');
    assert.equal(doing({...minerva, purpose: 'enroll', detail: 'Giuseppe'}), 'Sto imparando il volto di Giuseppe');
    assert.equal(doing({...minerva, purpose: 'look'}), 'Sto guardando attraverso la fotocamera');
    assert.equal(doing(app), 'sta usando la fotocamera frontale');
    assert.equal(owner(minerva), 'Minerva');
});
