/**
 * open_window.mjs — NanoGUI2 window with a BoxLayout, Labels, and Buttons.
 *
 * Run with:
 *   ./build/nanojs testapp/open_window.mjs
 */

import {
  init,
  shutdown,
  mainloop,
  Screen,
  Window,
  BoxLayout,
  Orientation,
  Alignment,
  Label,
  Button,
} from "nanogui";

init();

// ---- Screen ----------------------------------------------------------------

const screen = new Screen(480, 320, "NanoJS Demo", true);
screen.background = [0.15, 0.15, 0.18, 1.0];

screen.onResize = (w, h) => {
  console.log(`[onResize] ${w} x ${h}`);
  screen.perform_layout();
};

// ---- Window with a vertical BoxLayout -------------------------------------

const win = new Window(screen, "Hello from JavaScript!", true);
win.set_layout(new BoxLayout(Orientation.Vertical, Alignment.Fill, 12, 8));
win.onDispose = () => console.log("[onDispose] window closed");

// ---- Labels ----------------------------------------------------------------

const title = new Label(win, "NanoGUI powered by QuickJS", "sans-bold");
const sub = new Label(win, "Layout · Labels · Buttons — all from JS", "sans");

// ---- Buttons ---------------------------------------------------------------

let clickCount = 0;

const btn1 = new Button(win, "Click me!");
btn1.onClick = () => {
  clickCount++;
  btn1.caption = `Clicked ${clickCount} time${clickCount === 1 ? "" : "s"}`;
  screen.perform_layout();
};

const btn2 = new Button(win, "Reset");
btn2.onClick = () => {
  clickCount = 0;
  btn1.caption = "Click me!";
  screen.perform_layout();
};

// ---- Show ------------------------------------------------------------------

screen.perform_layout();
screen.draw_all();
screen.visible = true;

console.log("Window open. Click the buttons or resize the window.");
console.log("Close the OS window to exit.");

mainloop(1000 / 60);

console.log(`Final click count: ${clickCount}`);
shutdown();
