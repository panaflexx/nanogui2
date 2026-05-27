/**
 * test_bindings.mjs — NanoGUI2 JS binding surface tests.
 *
 * Tests the shape of the JS API exposed by the nanogui module:
 * constructor types, prototype inheritance chain, own property/method
 * surfaces, constructor error handling (no GL context available headlessly).
 *
 * Run with:
 *   ./build/nanojs testapp/test_bindings.mjs
 *
 * Exit 0 = all pass, 1 = failures.
 */

import { Widget, Screen, Window } from 'nanogui';

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------

let pass = 0, fail = 0;
const RESET = '', GREEN = '', RED = '';  // no ANSI needed, clear output

function test(label, fn) {
    try {
        fn();
        console.log('  PASS  ' + label);
        ++pass;
    } catch(e) {
        console.log('  FAIL  ' + label + '\n         ' + e.message);
        ++fail;
    }
}

function assert(cond, msg) {
    if (!cond) throw new Error(msg || 'assertion failed');
}

function assertThrows(fn, label) {
    let threw = false;
    try { fn(); } catch(_) { threw = true; }
    if (!threw) throw new Error(label || 'expected an exception but none was thrown');
}

function hasOwnProp(obj, name) {
    return Object.getOwnPropertyDescriptor(obj, name) !== undefined;
}

// ---------------------------------------------------------------------------
// 1. Import
// ---------------------------------------------------------------------------
console.log('\n[import]');

test('Widget  is a constructor function', () => assert(typeof Widget  === 'function'));
test('Screen  is a constructor function', () => assert(typeof Screen  === 'function'));
test('Window  is a constructor function', () => assert(typeof Window  === 'function'));

// ---------------------------------------------------------------------------
// 2. Constructor names
// ---------------------------------------------------------------------------
console.log('\n[constructor names]');

test('Widget.name  === "Widget"',  () => assert(Widget.name  === 'Widget'));
test('Screen.name  === "Screen"',  () => assert(Screen.name  === 'Screen'));
test('Window.name  === "Window"',  () => assert(Window.name  === 'Window'));

// ---------------------------------------------------------------------------
// 3. Prototype chain (inheritance)
// ---------------------------------------------------------------------------
console.log('\n[prototype chain]');

test('Screen.prototype inherits Widget.prototype', () =>
    assert(Object.getPrototypeOf(Screen.prototype) === Widget.prototype,
           'Object.getPrototypeOf(Screen.prototype) !== Widget.prototype'));

test('Window.prototype inherits Widget.prototype', () =>
    assert(Object.getPrototypeOf(Window.prototype) === Widget.prototype,
           'Object.getPrototypeOf(Window.prototype) !== Widget.prototype'));

test('Screen.prototype !== Window.prototype', () =>
    assert(Screen.prototype !== Window.prototype));

test('Widget.prototype.constructor === Widget', () =>
    assert(Widget.prototype.constructor === Widget));

test('Screen.prototype.constructor === Screen', () =>
    assert(Screen.prototype.constructor === Screen));

test('Window.prototype.constructor === Window', () =>
    assert(Window.prototype.constructor === Window));

// ---------------------------------------------------------------------------
// 4. Widget is abstract from JS
// ---------------------------------------------------------------------------
console.log('\n[Widget is abstract]');

test('new Widget() throws TypeError', () =>
    assertThrows(() => new Widget(), 'Widget() should throw'));

// ---------------------------------------------------------------------------
// 5. Widget.prototype — property surface
// ---------------------------------------------------------------------------
console.log('\n[Widget.prototype properties]');

const WIDGET_PROPS = ['x', 'y', 'width', 'height',
                      'visible', 'enabled', 'tooltip', 'id',
                      'font_size', 'child_count'];

for (const p of WIDGET_PROPS) {
    test(`Widget.prototype own property: "${p}"`, () =>
        assert(hasOwnProp(Widget.prototype, p),
               `"${p}" missing from Widget.prototype`));
}

// child_count is read-only: should have a getter but no setter
test('child_count is read-only (getter only)', () => {
    const d = Object.getOwnPropertyDescriptor(Widget.prototype, 'child_count');
    assert(typeof d.get === 'function', 'child_count.get is not a function');
    assert(d.set === undefined,         'child_count.set should be undefined');
});

// writable properties should have both getter and setter
for (const p of ['x', 'y', 'width', 'height', 'visible', 'enabled',
                  'tooltip', 'id', 'font_size']) {
    test(`"${p}" is read-write (has setter)`, () => {
        const d = Object.getOwnPropertyDescriptor(Widget.prototype, p);
        assert(typeof d.get === 'function', `${p}.get missing`);
        assert(typeof d.set === 'function', `${p}.set missing`);
    });
}

// ---------------------------------------------------------------------------
// 6. Widget.prototype — method surface
// ---------------------------------------------------------------------------
console.log('\n[Widget.prototype methods]');

const WIDGET_METHODS = ['request_focus', 'child_at', 'remove_child', 'perform_layout'];

for (const m of WIDGET_METHODS) {
    test(`Widget.prototype method: "${m}"`, () =>
        assert(typeof Widget.prototype[m] === 'function',
               `"${m}" is not a function on Widget.prototype`));
}

// ---------------------------------------------------------------------------
// 7. Window.prototype — own property surface
// ---------------------------------------------------------------------------
console.log('\n[Window.prototype own properties]');

const WINDOW_PROPS = ['title', 'modal', 'resizable', 'can_move'];

for (const p of WINDOW_PROPS) {
    test(`Window.prototype own property: "${p}"`, () =>
        assert(hasOwnProp(Window.prototype, p),
               `"${p}" missing from Window.prototype`));
}

for (const p of WINDOW_PROPS) {
    test(`Window "${p}" is read-write`, () => {
        const d = Object.getOwnPropertyDescriptor(Window.prototype, p);
        assert(typeof d.get === 'function', `${p}.get missing`);
        assert(typeof d.set === 'function', `${p}.set missing`);
    });
}

// ---------------------------------------------------------------------------
// 8. Window.prototype — method surface
// ---------------------------------------------------------------------------
console.log('\n[Window.prototype methods]');

for (const m of ['center', 'dispose']) {
    test(`Window.prototype method: "${m}"`, () =>
        assert(typeof Window.prototype[m] === 'function',
               `"${m}" is not a function on Window.prototype`));
}

// ---------------------------------------------------------------------------
// 9. Screen.prototype — own property surface
// ---------------------------------------------------------------------------
console.log('\n[Screen.prototype own properties]');

const SCREEN_PROPS_RW = ['caption', 'background'];
const SCREEN_PROPS_RO = ['pixel_ratio', 'framebuffer_size'];

for (const p of [...SCREEN_PROPS_RW, ...SCREEN_PROPS_RO]) {
    test(`Screen.prototype own property: "${p}"`, () =>
        assert(hasOwnProp(Screen.prototype, p),
               `"${p}" missing from Screen.prototype`));
}

for (const p of SCREEN_PROPS_RW) {
    test(`Screen "${p}" is read-write`, () => {
        const d = Object.getOwnPropertyDescriptor(Screen.prototype, p);
        assert(typeof d.get === 'function', `${p}.get missing`);
        assert(typeof d.set === 'function', `${p}.set missing`);
    });
}

for (const p of SCREEN_PROPS_RO) {
    test(`Screen "${p}" is read-only`, () => {
        const d = Object.getOwnPropertyDescriptor(Screen.prototype, p);
        assert(typeof d.get === 'function', `${p}.get missing`);
        assert(d.set === undefined,          `${p}.set should be undefined`);
    });
}

// ---------------------------------------------------------------------------
// 10. Screen.prototype — method surface
// ---------------------------------------------------------------------------
console.log('\n[Screen.prototype methods]');

for (const m of ['redraw', 'draw_all', 'perform_layout']) {
    test(`Screen.prototype method: "${m}"`, () =>
        assert(typeof Screen.prototype[m] === 'function',
               `"${m}" is not a function on Screen.prototype`));
}

// ---------------------------------------------------------------------------
// 11. Inherited Widget surface accessible via Screen / Window prototypes
// ---------------------------------------------------------------------------
console.log('\n[Widget methods accessible via Screen / Window prototype chain]');

for (const m of WIDGET_METHODS) {
    test(`Screen.prototype chain has Widget method "${m}"`, () =>
        assert(typeof Screen.prototype[m] === 'function',
               `"${m}" not accessible on Screen prototype chain`));

    test(`Window.prototype chain has Widget method "${m}"`, () =>
        assert(typeof Window.prototype[m] === 'function',
               `"${m}" not accessible on Window prototype chain`));
}

for (const p of ['visible', 'enabled', 'tooltip', 'x', 'y']) {
    test(`"${p}" accessible on Screen prototype chain`, () =>
        assert(p in Screen.prototype,
               `"${p}" not in Screen.prototype chain`));

    test(`"${p}" accessible on Window prototype chain`, () =>
        assert(p in Window.prototype,
               `"${p}" not in Window.prototype chain`));
}

// ---------------------------------------------------------------------------
// 12. Constructor error handling (no GL context available headlessly)
// ---------------------------------------------------------------------------
console.log('\n[constructor error handling]');

test('new Screen() throws without width/height', () =>
    assertThrows(() => new Screen(), 'Screen() needs at least 2 args'));

test('new Screen(800,600) throws without GL context', () =>
    assertThrows(() => new Screen(800, 600),
                 'Screen(800,600) should throw without a GL/GLFW context'));

test('new Window() throws without parent', () =>
    assertThrows(() => new Window(), 'Window() needs a parent arg'));

test('new Window(null) throws (null parent)', () =>
    assertThrows(() => new Window(null, 'Test'),
                 'Window(null) should throw TypeError'));

test('new Window(42) throws (wrong type)', () =>
    assertThrows(() => new Window(42, 'Test'),
                 'Window(number) should throw TypeError'));

test('new Window("bad") throws (string parent)', () =>
    assertThrows(() => new Window('bad'),
                 'Window(string) should throw TypeError'));

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('\n=== Results: ' + pass + ' passed, ' + fail + ' failed ===');

if (fail > 0) {
    throw new Error(fail + ' test(s) failed');
}
