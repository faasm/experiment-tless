use clap::ValueEnum;
use crate::env::Env;
use rand::Rng;
use std::fmt;
use std::process::{Command, Stdio};
use std::str::FromStr;

#[derive(Clone, Debug, ValueEnum, PartialEq, Eq, PartialOrd, Ord)]
pub enum DockerContainer {
    Experiments,
    Worker,
}

impl fmt::Display for DockerContainer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DockerContainer::Experiments => write!(f, "tless-experiments"),
            DockerContainer::Worker => write!(f, "tless-knative-worker"),
        }
    }
}

impl FromStr for DockerContainer {
    type Err = ();

    fn from_str(input: &str) -> Result<DockerContainer, Self::Err> {
        match input {
            "tless-experiments" => Ok(DockerContainer::Experiments),
            "tless-knative-worker" => Ok(DockerContainer::Worker),
            _ => Err(()),
        }
    }
}

impl DockerContainer {
    pub fn iter_variants() -> std::slice::Iter<'static, DockerContainer> {
        static VARIANTS: [DockerContainer; 2] = [
            DockerContainer::Experiments,
            DockerContainer::Worker,
        ];
        VARIANTS.iter()
    }
}

#[derive(Debug)]
pub struct Docker {}

impl Docker {
    pub fn get_docker_tag(ctr: &DockerContainer) -> String {
        // Prepare image tag
        let version = match Env::get_version() {
            Ok(ver) => ver,
            Err(e) => {
                panic!("invrs(docker): error getting version from file: {}", e);
            }
        };

        format!("{}/{}:{}", Env::CONTAINER_REGISTRY_URL, ctr, version)
    }

    fn do_build(ctr: &DockerContainer, nocache: bool) {
        // Prepare dockerfile path
        let mut dockerfile_path = Env::docker_root();
        dockerfile_path.push(format!("{ctr}.dockerfile"));

        let image_tag = Self::get_docker_tag(ctr);

        // Set arguments for the command
        let mut cmd = Command::new("docker");
        cmd.env("DOCKER_BUILDKIT", "1");
        cmd.current_dir(Env::proj_root());

        cmd.arg("build")
            .arg("-t")
            .arg(image_tag)
            .arg("-f")
            .arg(dockerfile_path.to_string_lossy().into_owned())
            .arg("--build-arg")
            .arg(format!("TLESS_VERSION={}", Env::get_version().unwrap()))
            // TODO: delete this build arg
            .arg("--build-arg")
            .arg(format!("TMP_VER={}", rand::thread_rng().gen_range(0..1000)))
            .arg(".");

        if nocache {
            cmd.arg("--no-cache");
        }

        cmd.stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .output()
            .expect("invrs: failed executing command");
    }

    fn do_push(ctr: &DockerContainer) {
        let image_tag = Self::get_docker_tag(ctr);

        // Set arguments for the command
        let mut cmd = Command::new("docker");
        cmd.env("DOCKER_BUILDKIT", "1");
        cmd.current_dir(Env::proj_root());

        cmd.arg("push")
            .arg(image_tag)
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .output()
            .expect("invrs: failed executing command");
    }

    pub fn build(ctr: &DockerContainer, push: bool, nocache: bool) {
        Self::do_build(ctr, nocache);

        if push {
            Self::do_push(ctr);
        }
    }
}
