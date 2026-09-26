/* Async dashboard regressions. Optional Node.js check; the application runtime is C++17. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

function dashboard() {
  const elements = new Map();
  const element = (selector) => {
    if (!elements.has(selector)) elements.set(selector, {
      value: '',
      disabled: false, hidden: false, innerHTML: '', textContent: '',
      addEventListener() {}, focus() {}, classList: { remove() {} },
    });
    return elements.get(selector);
  };
  const requests = [];
  const context = vm.createContext({
    document: { querySelector: element, querySelectorAll: () => [] },
    window: { matchMedia: () => ({ matches: false }) },
    AbortController, setTimeout,
    fetch: (_url, options) => new Promise((resolve, reject) => {
      // Intentionally allow completion after abort to exercise the revision guard.
      requests.push({ options, resolve: (summary) => resolve({ ok: true, json: async () => ({ summary }) }), reject });
    }),
  });
  const source = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
  const start = source.lastIndexOf('\ninit().catch(');
  assert.ok(start > 0, 'application boot entry exists');
  vm.runInContext(source.slice(0, start), context);
  vm.runInContext('applyView = () => {}; viewBefore = () => ({}); renderKPIs = () => {}; renderOverview = () => {}; showStage = async () => {};', context);
  return { context, requests, element, eval: (code) => vm.runInContext(code, context) };
}
const summary = (hour) => ({ hour, faults: [], latency_ms: 1, policy: { battery: 'dp' } });

test('a late older response cannot overwrite the newest decision', async () => {
  const app = dashboard();
  const first = app.eval('run()');
  app.eval('setHour(20)');
  const second = app.eval('run()');
  app.requests[1].resolve(summary(20));
  await second;
  app.requests[0].resolve(summary(19));
  await first;
  assert.equal(app.eval('S.result.summary.hour'), 20);
  assert.equal(app.eval('S.dirty'), false);
  assert.equal(app.element('#compare').disabled, false);
});

test('editing inputs invalidates an in-flight decision and its comparison', async () => {
  const app = dashboard();
  const run = app.eval('run()');
  app.eval('setHour(10); markDirty()');
  app.requests[0].resolve(summary(19));
  await run;
  assert.equal(app.eval('S.result'), null);
  assert.equal(app.eval('S.dirty'), true);
  assert.equal(app.element('#compare').disabled, true);
  assert.equal(app.element('#run').disabled, false);
});

test('a connection failure clears results and leaves Run available for retry', async () => {
  const app = dashboard();
  const run = app.eval('run()');
  app.requests[0].reject(new Error('offline'));
  assert.equal(await run, false);
  assert.equal(app.eval('S.result'), null);
  assert.equal(app.element('#run').disabled, false);
  assert.equal(app.element('#compare').disabled, true);
  assert.match(app.element('#status').textContent, /offline.*retry/);
});

test('a pending policy comparison is discarded when inputs change', async () => {
  const app = dashboard();
  app.eval('S.result = {summary: {hour:19, faults:[], policy:{battery:"dp"}}}; S.dirty=false; S.controller=new AbortController()');
  const comparison = app.eval('comparePolicies()');
  assert.equal(app.requests.length, 3);
  app.eval('markDirty()');
  for (const request of app.requests) request.resolve(summary(19));
  await comparison;
  assert.equal(app.element('#comparison').hidden, true);
  assert.equal(app.element('#compare').disabled, true);
});

test('invalid hours cannot propagate NaN or out-of-range values to the display', () => {
  const app = dashboard();
  for (const [input, expected] of [['NaN', 19], ['-2', 0], ['99', 23], ['10.9', 10]]) {
    app.eval(`setHour(${input})`);
    assert.equal(app.eval('S.hour'), expected);
  }
});
