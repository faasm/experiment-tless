use clap::ValueEnum;
use crate::tasks::s3::S3;
use std::path::PathBuf;
use std::str::FromStr;
use std::{env, fmt};

#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, ValueEnum)]
pub enum AvailableWorkflow {
    Finra,
    MlTraining,
    MlInference,
    WordCount,
}

impl fmt::Display for AvailableWorkflow {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AvailableWorkflow::Finra => write!(f, "finra"),
            AvailableWorkflow::MlTraining => write!(f, "ml-training"),
            AvailableWorkflow::MlInference => write!(f, "ml-inference"),
            AvailableWorkflow::WordCount => write!(f, "word-count"),
        }
    }
}

impl FromStr for AvailableWorkflow {
    type Err = ();

    fn from_str(input: &str) -> Result<AvailableWorkflow, Self::Err> {
        match input {
            "finra" => Ok(AvailableWorkflow::Finra),
            "ml-inference" => Ok(AvailableWorkflow::MlTraining),
            "ml-training" => Ok(AvailableWorkflow::MlInference),
            "word-count" => Ok(AvailableWorkflow::WordCount),
            _ => Err(()),
        }
    }
}

impl AvailableWorkflow {
    pub fn iter_variants() -> std::slice::Iter<'static, AvailableWorkflow> {
        static VARIANTS: [AvailableWorkflow; 4] = [
            AvailableWorkflow::Finra,
            AvailableWorkflow::MlTraining,
            AvailableWorkflow::MlInference,
            AvailableWorkflow::WordCount,
        ];
        VARIANTS.iter()
    }
}

#[derive(Debug)]
pub struct Workflows {}

impl Workflows {
    pub fn get_root() -> PathBuf {
        let mut path = env::current_dir().expect("invrs: failed to get current directory");
        path.push("workflows");
        path
    }

    pub async fn upload_workflow_state(
        workflow: &AvailableWorkflow,
        bucket_name: &str,
        clean: bool,
    ) {
        if clean {
            S3::clear_dir(bucket_name.to_string(), format!("{workflow}").to_string()).await;
        }

        match workflow {
            AvailableWorkflow::Finra => {
                let mut host_path = S3::get_datasets_root();
                host_path.push(format!("{workflow}"));
                host_path.push("yfinance.csv");
                let s3_path = format!("{workflow}/yfinance.csv");
                S3::upload_file(bucket_name, host_path.to_str().unwrap(), &s3_path).await;
            }
            AvailableWorkflow::MlTraining => {
                // We upload both datasets until we decide which one to use
                for dataset in vec!["mnist-images-2k", "mnist-images-10k"] {
                    let mut host_path = S3::get_datasets_root();
                    host_path.push(format!("{workflow}"));
                    host_path.push(format!("{dataset}"));
                    S3::upload_dir(
                        bucket_name.to_string(),
                        host_path.display().to_string(),
                        format!("{workflow}/{dataset}"),
                    )
                    .await;
                }
            }
            AvailableWorkflow::MlInference => {
                let mut host_path = S3::get_datasets_root();
                host_path.push(format!("{workflow}"));
                host_path.push("images-inference-1k");
                S3::upload_dir(
                    bucket_name.to_string(),
                    host_path.display().to_string(),
                    format!("{workflow}/images-inference-1k"),
                )
                .await;
            }
            AvailableWorkflow::WordCount => {
                let mut host_path = S3::get_datasets_root();
                host_path.push(format!("{workflow}"));
                host_path.push("few-files");
                S3::upload_dir(
                    bucket_name.to_string(),
                    host_path.display().to_string(),
                    format!("{workflow}/few-files"),
                )
                .await;
            }
        };
    }

    pub async fn upload_state(bucket_name: &str, clean: bool) {
        if clean {
            S3::clear_bucket(bucket_name.to_string()).await;
        }

        // Upload state for different workflows
        for workflow in AvailableWorkflow::iter_variants() {
            Self::upload_workflow_state(&workflow, bucket_name, clean).await;
        }
    }

    pub fn get_faasm_cmdline(workflow: &AvailableWorkflow) -> &str {
        match workflow {
            AvailableWorkflow::Finra => "finra/yfinance.csv 20",
            // ML Training workflow with SGX on mnist-10k takes ~30'
            // AvailableWorkflow::MlTraining => "ml-training/mnist-images-10k 4 8",
            AvailableWorkflow::MlTraining => "ml-training/mnist-images-2k 2 8",
            // ML Inference relies on the model outputed by ML Training
            AvailableWorkflow::MlInference => {
                "ml-inference/model ml-inference/images-inference-1k 16"
            }
            AvailableWorkflow::WordCount => "word-count/few-files",
        }
    }
}
