use serde::Deserialize;
use std::collections::BTreeMap;

pub const DEFAULT_MANIFEST_URL: &str =
    "https://github.com/fraa2a/Monolith/releases/latest/download/update-manifest.json";

pub const PUBLIC_KEY_B64: &str = "GgyaSRupUFn5Omaa90w0H2xDTrqff2DdzRDtbeplvKA=";

pub const MAX_COMPONENT_BYTES: u64 = 512 * 1024 * 1024;

#[derive(Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Manifest {
    pub schema: u32,
    #[serde(default)]
    pub published_at: String,
    #[serde(default)]
    pub release: ReleaseInfo,
    pub components: BTreeMap<String, ComponentInfo>,
}

#[derive(Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub struct ReleaseInfo {
    #[serde(default)]
    pub tag: String,
    #[serde(default)]
    pub notes_url: String,
}

#[derive(Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct ComponentInfo {
    pub version: String,
    pub url: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub sha256: String,
    #[serde(default)]
    pub ed_signature: String,
    #[serde(default)]
    pub metadata_signature: String,
}

impl ComponentInfo {
    fn authenticate_with_key(
        &self,
        component: &str,
        key: &ed25519_dalek::VerifyingKey,
    ) -> Result<(), String> {
        use base64::Engine;
        self.validate()?;
        let bytes: [u8; 64] = base64::engine::general_purpose::STANDARD
            .decode(self.metadata_signature.trim())
            .map_err(|_| "invalid metadata signature")?
            .try_into()
            .map_err(|_| "missing or invalid metadata signature")?;
        let message = format!(
            "monolith-component-v1\n{component}\n{}\n{}\n{}\n",
            self.version,
            self.size,
            self.sha256.to_ascii_lowercase()
        );
        key.verify_strict(
            message.as_bytes(),
            &ed25519_dalek::Signature::from_bytes(&bytes),
        )
        .map_err(|_| "component metadata signature verification failed".into())
    }

    pub fn authenticate(&self, component: &str) -> Result<(), String> {
        use base64::Engine;
        let bytes: [u8; 32] = base64::engine::general_purpose::STANDARD
            .decode(PUBLIC_KEY_B64)
            .map_err(|_| "invalid public key")?
            .try_into()
            .map_err(|_| "invalid public key")?;
        let key =
            ed25519_dalek::VerifyingKey::from_bytes(&bytes).map_err(|_| "invalid public key")?;
        self.authenticate_with_key(component, &key)
    }
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
    for (component, info) in &m.components {
        info.authenticate(component)
            .map_err(|e| format!("{component}: {e}"))?;
    }
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[ignore = "requires a PowerShell-generated signed release fixture"]
    fn generated_release_contract() {
        use base64::Engine;
        let path = std::env::var("MONOLITH_GENERATED_MANIFEST").unwrap();
        let text = std::fs::read_to_string(path).unwrap();
        #[derive(serde::Deserialize)]
        struct LegacyComponent {
            ed_signature: String,
        }
        #[derive(serde::Deserialize)]
        struct LegacyManifest {
            components: BTreeMap<String, LegacyComponent>,
        }
        let legacy: LegacyManifest = serde_json::from_str(&text).unwrap();
        assert!(legacy
            .components
            .values()
            .all(|component| !component.ed_signature.is_empty()));
        let m: Manifest = serde_json::from_str(&text).unwrap();
        m.validate().unwrap();
        assert!(!m.published_at.is_empty());
        assert_eq!(m.components.len(), 3);
        let bytes: [u8; 32] = base64::engine::general_purpose::STANDARD
            .decode(std::env::var("MONOLITH_TEST_PUBLIC_KEY").unwrap())
            .unwrap()
            .try_into()
            .unwrap();
        let key = ed25519_dalek::VerifyingKey::from_bytes(&bytes).unwrap();
        let payload = std::fs::read(std::env::var("MONOLITH_TEST_PAYLOAD").unwrap()).unwrap();
        for (name, info) in m.components {
            info.authenticate_with_key(&name, &key).unwrap();
            let signature: [u8; 64] = base64::engine::general_purpose::STANDARD
                .decode(&info.ed_signature)
                .unwrap()
                .try_into()
                .unwrap();
            key.verify_strict(&payload, &ed25519_dalek::Signature::from_bytes(&signature))
                .unwrap();
            assert_eq!(
                legacy.components.get(&name).unwrap().ed_signature,
                info.ed_signature
            );
        }
    }
    #[test]
    fn signed_identity_rejects_relabelled_old_payload() {
        use base64::Engine;
        use ed25519_dalek::{Signer, SigningKey};
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let mut info: ComponentInfo = serde_json::from_value(serde_json::json!({"version":"1.2.3","url":"https://example.com/a","size":1,"sha256":"ab".repeat(32),"edSignature":base64::engine::general_purpose::STANDARD.encode([0u8;64])})).unwrap();
        let message = format!("monolith-component-v1\nengine\n1.2.3\n1\n{}\n", info.sha256);
        info.metadata_signature = base64::engine::general_purpose::STANDARD
            .encode(key.sign(message.as_bytes()).to_bytes());
        info.authenticate_with_key("engine", &key.verifying_key())
            .unwrap();
        assert!(info
            .authenticate_with_key("ui", &key.verifying_key())
            .is_err());
        info.version = "99.0.0".into();
        assert!(info
            .authenticate_with_key("engine", &key.verifying_key())
            .is_err());
        info.version = "1.2.3".into();
        info.size = 2;
        assert!(info
            .authenticate_with_key("engine", &key.verifying_key())
            .is_err());
        info.size = 1;
        info.sha256 = "cd".repeat(32);
        assert!(info
            .authenticate_with_key("engine", &key.verifying_key())
            .is_err());
    }
    #[test]
    fn camel_case_release_contract_round_trips() {
        let json = serde_json::json!({"schema":1,"publishedAt":"date","release":{"notesUrl":"notes"},"components":{"engine":{"version":"1.2.3","url":"https://example.com/a","size":1,"sha256":"ab".repeat(32),"edSignature":base64::Engine::encode(&base64::engine::general_purpose::STANDARD,[0u8;64])}}});
        let m: Manifest = serde_json::from_value(json).unwrap();
        m.validate().unwrap();
        assert_eq!(m.published_at, "date");
        assert_eq!(m.release.notes_url, "notes");
    }
}
