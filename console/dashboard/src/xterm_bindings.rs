use wasm_bindgen::prelude::*;

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = ["Terminal"])]
    pub type XTerminal;

    #[wasm_bindgen(constructor, js_namespace = ["Terminal"])]
    pub fn new(options: &JsValue) -> XTerminal;

    #[wasm_bindgen(method)]
    pub fn open(this: &XTerminal, element: &web_sys::HtmlElement);

    #[wasm_bindgen(method)]
    pub fn write(this: &XTerminal, data: &str);
}
