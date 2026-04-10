use leptos::*;
use serde::Deserialize;

#[derive(Clone, Deserialize)]
pub struct ImageEntry {
    pub name:       String,
    pub size_bytes: u64,
    pub status:     String, // "cached" | "missing" | "downloading"
}

#[component]
pub fn ImagesPanel() -> impl IntoView {
    let (images,  set_images)  = create_signal(Vec::<ImageEntry>::new());
    let (loading, set_loading) = create_signal(false);
    let (error,   set_error)   = create_signal(Option::<String>::None);

    let fetch = move || {
        set_loading.set(true);
        set_error.set(None);
        wasm_bindgen_futures::spawn_local(async move {
            match gloo_net::http::Request::get("/api/images").send().await {
                Ok(resp) => {
                    #[derive(Deserialize)]
                    struct Wrapper { images: Vec<ImageEntry> }
                    match resp.json::<Wrapper>().await {
                        Ok(w) => set_images.set(w.images),
                        Err(e) => set_error.set(Some(e.to_string())),
                    }
                }
                Err(e) => set_error.set(Some(e.to_string())),
            }
            set_loading.set(false);
        });
    };

    // Fetch on mount
    create_effect(move |_| { fetch(); });

    view! {
        <div class="images-panel">
            <div class="images-toolbar">
                <h2 class="panel-heading">"OS Images"</h2>
                <button class="btn" on:click=move |_| fetch()>
                    {move || if loading.get() { "Loading…" } else { "↻ Refresh" }}
                </button>
            </div>

            {move || error.get().map(|e| view! {
                <div class="error-banner">{"Error: "}{e}</div>
            })}

            <div class="images-table-wrap">
                <Show
                    when=move || !images.get().is_empty()
                    fallback=|| view! {
                        <div class="empty-state">"No images found. Place .img files in the guest-images/ directory."</div>
                    }
                >
                    <table class="images-table">
                        <thead>
                            <tr>
                                <th>"Name"</th>
                                <th>"Size"</th>
                                <th>"Status"</th>
                            </tr>
                        </thead>
                        <tbody>
                            {move || images.get().into_iter().map(|img| {
                                let size_str = human_size(img.size_bytes);
                                let badge_cls = format!("image-status status-{}", img.status);
                                view! {
                                    <tr class="image-row">
                                        <td class="image-name">{img.name}</td>
                                        <td class="image-size">{size_str}</td>
                                        <td><span class=badge_cls>{img.status}</span></td>
                                    </tr>
                                }
                            }).collect_view()}
                        </tbody>
                    </table>
                </Show>
            </div>
        </div>
    }
}

fn human_size(bytes: u64) -> String {
    const GB: u64 = 1 << 30;
    const MB: u64 = 1 << 20;
    const KB: u64 = 1 << 10;
    if bytes >= GB      { format!("{:.1} GB", bytes as f64 / GB as f64) }
    else if bytes >= MB { format!("{:.1} MB", bytes as f64 / MB as f64) }
    else if bytes >= KB { format!("{:.1} KB", bytes as f64 / KB as f64) }
    else                { format!("{} B", bytes) }
}
