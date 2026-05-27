#pragma once

#include <quickjs.h>

namespace nanogui::js {

/// Register the built-in "nanogui" C module with the given runtime and context.
///
/// Call this once, before evaluating any JS that does:
///   import { Screen, Window, Widget } from 'nanogui';
///
/// The function:
///   1. Creates the "nanogui" JSModuleDef and records its init callback.
///   2. Pre-declares all exports so the linker phase can resolve them.
///
/// The actual class registration (prototype construction, class-ID allocation,
/// etc.) happens lazily when the module is first imported/instantiated by the
/// JS engine, which invokes js_nanogui_module_init().
void register_nanogui_module(JSRuntime* rt, JSContext* ctx);

} // namespace nanogui::js
