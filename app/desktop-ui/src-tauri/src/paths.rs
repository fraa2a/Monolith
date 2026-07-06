use std::env;
use std::path::PathBuf;

pub struct OutputDirs {
    pub clips: PathBuf,
    pub recs: PathBuf,
}

pub fn local_app_data() -> PathBuf {
    env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .or_else(|| env::var_os("USERPROFILE").map(|home| PathBuf::from(home).join("AppData").join("Local")))
        .unwrap_or_default()
}

pub fn monolith_data_dir() -> PathBuf {
    local_app_data().join("Monolith")
}

fn videos_dir() -> PathBuf {
    env::var_os("USERPROFILE")
        .map(|home| PathBuf::from(home).join("Videos"))
        .unwrap_or_else(|| PathBuf::from("Videos"))
}

pub fn default_output_dirs() -> OutputDirs {
    let root = videos_dir().join("Monolith");
    OutputDirs {
        clips: root.join("Clips"),
        recs: root.join("Recordings"),
    }
}
