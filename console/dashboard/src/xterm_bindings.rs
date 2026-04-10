use wasm_bindgen::prelude::*;

#[wasm_bindgen]
extern "C" {
    pub type XTerminal;

    /// Calls `new Terminal(options)` on the global `window.Terminal` class
    /// provided by the xterm.js CDN bundle.
    #[wasm_bindgen(constructor, js_name = "Terminal")]
    pub fn new(options: &JsValue) -> XTerminal;

    /// Mount the terminal into a DOM element.
    #[wasm_bindgen(method)]
    pub fn open(this: &XTerminal, element: &web_sys::HtmlElement);

    /// Write text or binary data to the terminal display.
    #[wasm_bindgen(method)]
    pub fn write(this: &XTerminal, data: &str);

    /// Register a callback for data produced by user input (keyboard/paste).
    /// Returns an IDisposable (ignored here).
    #[wasm_bindgen(method, js_name = "onData")]
    pub fn on_data(this: &XTerminal, listener: &JsValue) -> JsValue;

    /// Dispose the terminal and free all resources.
    #[wasm_bindgen(method)]
    pub fn dispose(this: &XTerminal);
}
