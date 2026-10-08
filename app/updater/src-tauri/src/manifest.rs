use serde::Deserialize;
use std::collections::BTreeMap;

pub const DEFAULT_MANIFEST_URL: &str =
    "https://github.com/fraa2a/Monolith/releases/latest/download/update-manifest.json";

pub const MAX_COMPONENT_BYTES: u64 = 512 * 1024 * 1024;

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

impl ComponentInfo {
    pub fn validate(&self) -> Result<(), String> {
        semver::Version::parse(&self.version).map_err(|_| "invalid component version")?;
        if !self.url.starts_with("https://") || self.url.contains('\0') {
            return Err("component URL must use HTTPS".into());
        }
        if self.size == 0 || self.size > MAX_COMPONENT_BYTES {
            return Err("invalid component size".into());
        }
        if self.sha256.len() != 64 || !self.sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err("missing or invalid component checksum".into());
        }
        use base64::Engine;
        let signature = base64::engine::general_purpose::STANDARD
            .decode(self.ed_signature.trim())
            .map_err(|_| "invalid component signature")?;
        if signature.len() != 64 {
            return Err("missing or invalid component signature".into());
        }
        Ok(())
    }
}

impl Manifest {
    pub fn validate(&self) -> Result<(), String> {
        if self.schema != 1 {
            return Err(format!("unsupported manifest schema {}", self.schema));
        }
        if self.components.is_empty() {
            return Err("manifest contains no components".into());
        }
        for (key, component) in &self.components {
            if !["engine", "ui", "updater"].contains(&key.as_str()) {
                return Err(format!("unknown component: {key}"));
            }
            component.validate().map_err(|e| format!("{key}: {e}"))?;
        }
        Ok(())
    }
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
    m.validate()?;
    Ok(m)
}
