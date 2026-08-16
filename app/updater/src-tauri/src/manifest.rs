use serde::Deserialize;
use std::collections::BTreeMap;

pub const DEFAULT_MANIFEST_URL: &str =
    "https://github.com/fraa2a/Monolith/releases/latest/download/update-manifest.json";

#[derive(Deserialize, Clone)]
pub struct Manifest {
    pub schema: u32,
    #[serde(default)]
    pub published_at: String,
    #[serde(default)]
    pub release: ReleaseInfo,
    pub components: BTreeMap<String, ComponentInfo>,
}

#[derive(Deserialize, Clone, Default)]
pub struct ReleaseInfo {
    #[serde(default)]
    pub tag: String,
    #[serde(default)]
    pub notes_url: String,
}

#[derive(Deserialize, Clone)]
pub struct ComponentInfo {
    pub version: String,
    pub url: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub sha256: String,
    #[serde(default)]
    pub ed_signature: String,
}

pub fn manifest_url() -> String {
    std::env::var("MONOLITH_UPDATE_MANIFEST")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| DEFAULT_MANIFEST_URL.to_string())
}

pub fn fetch() -> Result<Manifest, String> {
    let body = crate::http::get_to_string(&manifest_url())?;
    let value: serde_json::Value =
        serde_json::from_str(&body).map_err(|e| format!("invalid manifest JSON: {e}"))?;
    let m: Manifest =
        serde_json::from_value(value).map_err(|e| format!("invalid manifest: {e}"))?;
    if m.schema != 1 {
        return Err(format!("unsupported manifest schema {}", m.schema));
    }
    if m.components.is_empty() {
        return Err("manifest contains no components".to_string());
    }
    Ok(m)
}
