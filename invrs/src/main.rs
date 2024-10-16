use crate::tasks::docker::{Docker, DockerContainer};
use crate::tasks::eval::{Eval, EvalExperiment, EvalRunArgs};
use crate::tasks::s3::S3;
use clap::{Parser, Subcommand};
use env_logger;

pub mod env;
pub mod tasks;

#[derive(Parser)]
struct Cli {
    // The name of the task to execute
    #[clap(subcommand)]
    task: Command,

    #[arg(short, long, global = true)]
    debug: bool,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Build and push different docker images
    Docker {
        #[command(subcommand)]
        docker_command: DockerCommand,
    },
    /// Run evaluation experiments and plot results
    Eval {
        #[command(subcommand)]
        eval_command: EvalCommand,
    },
    /// Interact with an S3 (MinIO server)
    S3 {
        #[command(subcommand)]
        s3_command: S3Command,
    },
}

#[derive(Debug, Subcommand)]
enum DockerCommand {
    Build {
        #[arg(short, long, num_args = 1.., value_name = "CTR_NAME")]
        ctr: Vec<DockerContainer>,
        #[arg(long)]
        push: bool,
        #[arg(long)]
        nocache: bool,
    },
    BuildAll {
        #[arg(long)]
        push: bool,
        #[arg(long)]
        nocache: bool,
    },
}

#[derive(Debug, Subcommand)]
enum EvalSubCommand {
    /// Run
    Run(EvalRunArgs),
    /// Plot
    Plot {},
}

#[derive(Debug, Subcommand)]
enum EvalCommand {
    /// Evaluate end-to-end execution latency for different workflows
    E2eLatency {
        #[command(subcommand)]
        eval_sub_command: EvalSubCommand,
    },
}

#[derive(Debug, Subcommand)]
enum S3Command {
    /// Clear a given bucket in an S3 server
    ClearBucket {
        #[arg(long)]
        bucket_name: String,
    },
    /// Clear a sub-tree in an S3 bucket indicated by a prefix
    ClearDir {
        #[arg(long)]
        bucket_name: String,
        #[arg(long)]
        prefix: String,
    },
    /// Clear a sub-tree in an S3 bucket indicated by a prefix
    GetKey {
        #[arg(long)]
        bucket_name: String,
        #[arg(long)]
        key: String,
    },
    /// List all buckets in an S3 server
    ListBuckets {},
    /// List all keys in an S3 bucket
    ListKeys {
        /// Name of the bucket
        #[arg(long)]
        bucket_name: String,
        /// Prefix
        #[arg(long)]
        prefix: Option<String>,
    },
    /// Upload a directory to S3
    UploadDir {
        /// Name of the bucket to store files in
        #[arg(long)]
        bucket_name: String,
        /// Host path to upload files from
        #[arg(long)]
        host_path: String,
        /// Path in the S3 server to store files to
        #[arg(long)]
        s3_path: String,
    },
}

#[tokio::main]
async fn main() {
    let cli = Cli::parse();

    // Initialize the logger based on the debug flag
    if cli.debug {
        env_logger::Builder::from_default_env()
            .filter_level(log::LevelFilter::Debug)
            .init();
    } else {
        env_logger::Builder::from_default_env()
            .filter_level(log::LevelFilter::Info)
            .init();
    }

    match &cli.task {
        Command::Docker { docker_command } => match docker_command {
            DockerCommand::Build { ctr, push, nocache } => {
                for c in ctr {
                    Docker::build(c, *push, *nocache);
                }
            }
            DockerCommand::BuildAll { push, nocache } => {
                for ctr in DockerContainer::iter_variants() {
                    // Do not push the base build image
                    if *ctr == DockerContainer::Experiments {
                        Docker::build(ctr, false, *nocache);
                    } else {
                        Docker::build(ctr, *push, *nocache);
                    }
                }
            }
        },
        Command::Eval { eval_command } => match eval_command {
            EvalCommand::E2eLatency { eval_sub_command } => match eval_sub_command {
                EvalSubCommand::Run(run_args) => {
                    Eval::run(&EvalExperiment::E2eLatency, run_args).await;
                }
                EvalSubCommand::Plot {} => {
                    Eval::plot(&EvalExperiment::E2eLatency);
                }
            },
        },
        // FIXME: move all S3 methods to &str
        Command::S3 { s3_command } => match s3_command {
            S3Command::ClearBucket { bucket_name } => {
                S3::clear_bucket(bucket_name.to_string()).await;
            }
            S3Command::ClearDir {
                bucket_name,
                prefix,
            } => {
                S3::clear_dir(bucket_name.to_string(), prefix.to_string()).await;
            }
            // TODO: delete me!
            S3Command::GetKey { bucket_name, key } => {
                S3::wait_for_key(bucket_name, key).await;
            }
            S3Command::ListBuckets {} => {
                S3::list_buckets().await;
            }
            S3Command::ListKeys { bucket_name, prefix } => {
                S3::list_keys(bucket_name.to_string(), prefix).await;
            }
            S3Command::UploadDir {
                bucket_name,
                host_path,
                s3_path,
            } => {
                S3::upload_dir(
                    bucket_name.to_string(),
                    host_path.to_string(),
                    s3_path.to_string(),
                )
                .await;
            },
        },
    }
}
