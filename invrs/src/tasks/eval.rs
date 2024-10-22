use crate::env::Env;
use crate::tasks::docker::{Docker, DockerContainer};
use crate::tasks::s3::S3;
use crate::tasks::workflows::{AvailableWorkflow, Workflows};
use chrono::{DateTime, Duration, TimeZone, Utc};
use clap::{Args, ValueEnum};
use csv::ReaderBuilder;
use indicatif::{ProgressBar, ProgressStyle};
use log::{debug, error};
use plotters::prelude::*;
use serde::Deserialize;
use shell_words;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::str::FromStr;
use std::{collections::BTreeMap, env, fmt, fs, io::Write, str, thread, time};

static EVAL_BUCKET_NAME: &str = "tless";

#[derive(Clone, Debug, ValueEnum, PartialEq, Eq, PartialOrd, Ord)]
pub enum EvalBaseline {
    Faasm,
    SgxFaasm,
    TlessFaasm,
    Knative,
    CcKnative,
    TlessKnative,
}

impl fmt::Display for EvalBaseline {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            EvalBaseline::Faasm => write!(f, "faasm"),
            EvalBaseline::SgxFaasm => write!(f, "sgx-faasm"),
            EvalBaseline::TlessFaasm => write!(f, "tless-faasm"),
            EvalBaseline::Knative => write!(f, "knative"),
            EvalBaseline::CcKnative => write!(f, "cc-knative"),
            EvalBaseline::TlessKnative => write!(f, "tless-knative"),
        }
    }
}

impl FromStr for EvalBaseline {
    type Err = ();

    fn from_str(input: &str) -> Result<EvalBaseline, Self::Err> {
        match input {
            "faasm" => Ok(EvalBaseline::Faasm),
            "sgx-faasm" => Ok(EvalBaseline::SgxFaasm),
            "tless-faasm" => Ok(EvalBaseline::TlessFaasm),
            "knative" => Ok(EvalBaseline::Knative),
            "cc-knative" => Ok(EvalBaseline::CcKnative),
            "tless-knative" => Ok(EvalBaseline::TlessKnative),
            _ => Err(()),
        }
    }
}

impl EvalBaseline {
    pub fn iter_variants() -> std::slice::Iter<'static, EvalBaseline> {
        static VARIANTS: [EvalBaseline; 6] = [
            EvalBaseline::Faasm,
            EvalBaseline::SgxFaasm,
            EvalBaseline::TlessFaasm,
            EvalBaseline::Knative,
            EvalBaseline::CcKnative,
            EvalBaseline::TlessKnative,
        ];
        VARIANTS.iter()
    }

    pub fn get_color(&self) -> RGBColor {
        match self {
            EvalBaseline::Faasm => RGBColor(171, 222, 230),
            EvalBaseline::SgxFaasm => RGBColor(203, 170, 203),
            EvalBaseline::TlessFaasm => RGBColor(255, 255, 181),
            EvalBaseline::Knative => RGBColor(255, 204, 182),
            EvalBaseline::CcKnative => RGBColor(243, 176, 195),
            EvalBaseline::TlessKnative => RGBColor(151, 193, 169),
        }
    }
}

pub enum EvalExperiment {
    E2eLatency,
    E2eLatencyCold,
}

impl fmt::Display for EvalExperiment {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            EvalExperiment::E2eLatency => write!(f, "e2e-latency"),
            EvalExperiment::E2eLatencyCold => write!(f, "e2e-latency-cold"),
        }
    }
}

#[derive(Debug, Args)]
pub struct EvalRunArgs {
    #[arg(short, long, num_args = 1.., value_name = "BASELINE")]
    baseline: Vec<EvalBaseline>,
    #[arg(long, default_value = "3")]
    num_repeats: u32,
    #[arg(long, default_value = "1")]
    num_warmup_repeats: u32,
}

pub struct ExecutionResult {
    start_time: DateTime<Utc>,
    end_time: DateTime<Utc>,
    iter: u32,
}

#[derive(Debug)]
pub struct Eval {}

impl Eval {
    fn get_root() -> PathBuf {
        let mut path = env::current_dir().expect("invrs: failed to get current directory");
        path.push("eval");
        path
    }

    fn get_data_file_name(
        workflow: &AvailableWorkflow,
        exp: &EvalExperiment,
        baseline: &EvalBaseline,
    ) -> String {
        format!(
            "{}/{exp}/data/{baseline}_{workflow}.csv",
            Self::get_root().display()
        )
    }

    fn init_data_file(workflow: &AvailableWorkflow, exp: &EvalExperiment, baseline: &EvalBaseline) {
        let mut file = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(Self::get_data_file_name(workflow, exp, baseline))
            .expect("invrs(eval): failed to write to file");

        match exp {
            EvalExperiment::E2eLatency | EvalExperiment::E2eLatencyCold => {
                writeln!(file, "Run,TimeMs").expect("invrs(eval): failed to write to file");
            }
        }
    }

    fn write_result_to_file(
        workflow: &AvailableWorkflow,
        exp: &EvalExperiment,
        baseline: &EvalBaseline,
        result: &ExecutionResult,
    ) {
        let mut file = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .append(true)
            .open(Self::get_data_file_name(workflow, exp, baseline))
            .expect("invrs(eval): failed to write to file");

        match exp {
            EvalExperiment::E2eLatency | EvalExperiment::E2eLatencyCold => {
                let duration: Duration = result.end_time - result.start_time;
                writeln!(file, "{},{}", result.iter, duration.num_milliseconds())
                    .expect("invrs(eval): failed to write to file");
            }
        }
    }

    fn get_all_data_files(exp: &EvalExperiment) -> Vec<PathBuf> {
        // TODO: change to data
        let data_path = format!("{}/{exp}/data", Self::get_root().display());

        // Collect all CSV files in the directory
        let mut csv_files = Vec::new();
        for entry in fs::read_dir(data_path).unwrap() {
            let entry = entry.unwrap();
            if entry.path().extension().and_then(|e| e.to_str()) == Some("csv") {
                csv_files.push(entry.path());
            }
        }

        return csv_files;
    }

    fn get_progress_bar(
        num_repeats: u64,
        exp: &EvalExperiment,
        baseline: &EvalBaseline,
        workflow: &str,
    ) -> ProgressBar {
        let pb = ProgressBar::new(num_repeats);
        pb.set_style(
            ProgressStyle::default_bar()
                .template("{msg} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%)")
                .expect("invrs(eval): error creating progress bar")
                .progress_chars("#>-"),
        );
        pb.set_message(format!("{exp}/{baseline}/{workflow}"));
        pb
    }

    // ------------------------------------------------------------------------
    // Run with Knative Functions
    // ------------------------------------------------------------------------

    fn get_kubectl_cmd() -> String {
        // For the moment, we literally run the `kubectl` command installed
        // as part of `coco-serverless`. We may change this in the future
        match env::var("COCO_SOURCE") {
            Ok(value) => format!("{value}/bin/kubectl"),
            Err(_) => panic!("invrs(eval): failed to read COCO_SOURCE env. var"),
        }
    }

    fn run_kubectl_cmd(cmd: &str) -> String {
        debug!("{}(eval): running kubectl command: {cmd}", Env::SYS_NAME);
        let args: Vec<&str> = cmd.split_whitespace().collect();

        let output = Command::new(Self::get_kubectl_cmd())
            .args(&args[0..])
            .output()
            .expect("invrs(eval): failed to execute kubectl command");

        String::from_utf8(output.stdout)
            .expect("invrs(eval): failed to convert kube command output to string")
    }

    fn wait_for_pods(namespace: &str, label: &str, num_expected: usize) {
        loop {
            thread::sleep(time::Duration::from_secs(2));

            let output = Self::run_kubectl_cmd(&format!("-n {namespace} get pods -l {label} -o jsonpath='{{..status.conditions[?(@.type==\"Ready\")].status}}'"));
            let values: Vec<&str> = output.split_whitespace().collect();

            debug!(
                "{}(eval): waiting for {num_expected} pods (label: {label}) to be ready...",
                Env::SYS_NAME
            );
            if values.len() != num_expected {
                debug!(
                    "{}(eval): not enough pods: {} != {num_expected}",
                    Env::SYS_NAME,
                    values.len()
                );
                continue;
            }

            if !values.iter().all(|&item| item == "'True'") {
                debug!("{}(eval): not enough pods in 'Ready' state", Env::SYS_NAME);
                continue;
            }

            break;
        }
    }

    fn wait_for_pod(namespace: &str, label: &str) {
        Self::wait_for_pods(namespace, label, 1);
    }

    fn template_yaml(yaml_path: PathBuf, env_vars: BTreeMap<&str, &str>) -> String {
        let yaml_content = fs::read_to_string(yaml_path).expect("invrs(eval): failed to read yaml");

        // Use envsubst to substitute environment variables in the YAML
        let mut envsubst_cmd = Command::new("envsubst");
        for (key, value) in &env_vars {
            envsubst_cmd.env(key, value);
        }

        let mut envsubst = envsubst_cmd
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .expect("invrs(eval): failed to start envsubst");

        envsubst
            .stdin
            .as_mut()
            .expect("invrs(eval): failed to open stdin for envsubst")
            .write_all(yaml_content.as_bytes())
            .expect("invrs(eval): failed to write to envsubst");

        // Collect the output (substituted YAML)
        let result = envsubst
            .wait_with_output()
            .expect("invrs(eval): failed to read envsubst result");

        String::from_utf8(result.stdout).expect("Failed to convert envsubst output to string")
    }

    fn deploy_workflow(workflow: &AvailableWorkflow, baseline: &EvalBaseline) {
        let mut workflow_yaml = Workflows::get_root();
        workflow_yaml.push(format!("{workflow}"));
        workflow_yaml.push("knative");
        workflow_yaml.push("workflow.yaml");
        let templated_yaml = Self::template_yaml(
            workflow_yaml,
            BTreeMap::from([
                (
                    "RUNTIME_CLASS_NAME",
                    match baseline {
                        EvalBaseline::Knative => "kata-qemu",
                        EvalBaseline::CcKnative | EvalBaseline::TlessKnative => "kata-qemu-sev",
                        _ => panic!("woops"),
                    },
                ),
                ("TLESS_VERSION", &Env::get_version().unwrap()),
                ("TLESS_MODE",
                match baseline {
                    EvalBaseline::Knative | EvalBaseline::CcKnative => "off",
                    EvalBaseline::TlessKnative => "on",
                    _ => panic!("woops"),
                }),
            ]),
        );

        let mut kubectl = Command::new(Self::get_kubectl_cmd())
            .arg("apply")
            .arg("-f")
            .arg("-")
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("invrs(eval): failed to start kubectl apply");

        kubectl
            .stdin
            .as_mut()
            .expect("invrs(eval): failed to open stdin for kubectl")
            .write_all(templated_yaml.as_bytes())
            .expect("invrs(eval): failed to feed kubectl through stdin");

        // Check if the kubectl command succeeded
        kubectl
            .wait_with_output()
            .expect("invrs(eval): failed to run kubectl command");

        // Specific per-workflow wait command
        match workflow {
            AvailableWorkflow::Finra => {
                Self::wait_for_pod("tless", "tless.workflows/name=finra-fetch-private");
                Self::wait_for_pod("tless", "tless.workflows/name=finra-fetch-public");
                Self::wait_for_pod("tless", "tless.workflows/name=finra-merge");
            }
            AvailableWorkflow::MlTraining => {
                Self::wait_for_pod("tless", "tless.workflows/name=ml-training-partition");
                Self::wait_for_pod("tless", "tless.workflows/name=ml-training-validation");
            }
            AvailableWorkflow::MlInference => {
                Self::wait_for_pod("tless", "tless.workflows/name=ml-inference-load");
                Self::wait_for_pod("tless", "tless.workflows/name=ml-inference-partition");
            }
            AvailableWorkflow::WordCount => {
                Self::wait_for_pod("tless", "tless.workflows/name=word-count-splitter");
                Self::wait_for_pod("tless", "tless.workflows/name=word-count-reducer");
            }
        }
    }

    fn delete_workflow(workflow: &AvailableWorkflow, baseline: &EvalBaseline) {
        // Common deploy mechanism
        let mut workflow_yaml = Workflows::get_root();
        workflow_yaml.push(format!("{workflow}"));
        workflow_yaml.push("knative");
        workflow_yaml.push("workflow.yaml");
        let templated_yaml = Self::template_yaml(
            workflow_yaml,
            BTreeMap::from([(
                "RUNTIME_CLASS_NAME",
                match baseline {
                    EvalBaseline::Knative => "kata-qemu",
                    EvalBaseline::CcKnative | EvalBaseline::TlessKnative => "kata-qemu-sev",
                    _ => panic!("woops"),
                },
                ),
                ("TLESS_VERSION", &Env::get_version().unwrap()),
                ("TLESS_MODE",
                match baseline {
                    EvalBaseline::Knative | EvalBaseline::CcKnative => "off",
                    EvalBaseline::TlessKnative => "on",
                    _ => panic!("woops"),
                }),
            ]),
        );

        let mut kubectl = Command::new(Self::get_kubectl_cmd())
            .arg("delete")
            .arg("--wait=true")
            .arg("--cascade=foreground")
            .arg("-f")
            .arg("-")
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("invrs(eval): failed to start kubectl apply");

        kubectl
            .stdin
            .as_mut()
            .expect("invrs(eval): failed to open stdin for kubectl")
            .write_all(templated_yaml.as_bytes())
            .expect("invrs(eval): failed to feed kubectl through stdin");

        kubectl
            .wait_with_output()
            .expect("invrs(eval): failed to run kubectl command");
    }

    async fn wait_for_scale_to_zero() {
        loop {
            let output = Self::run_kubectl_cmd(&format!("-n tless get pods -o jsonpath={{..status.conditions[?(@.type==\"Ready\")].status}}"));
            debug!("tlessctl: waiting for a scale-down: out: {output}");
            let values: Vec<&str> = output.split_whitespace().collect();

            if values.len() == 1 {
                break;
            }

            thread::sleep(time::Duration::from_secs(2));
        }
    }

    /// Run workflow once, and return result depending on the experiment
    async fn run_workflow_once(workflow: &AvailableWorkflow, exp: &EvalExperiment) -> ExecutionResult {
        let mut exp_result = ExecutionResult {
            start_time: Utc::now(),
            end_time: Utc::now(),
            iter: 0,
        };

        // Common trigger mechanism
        let mut trigger_cmd = Workflows::get_root();
        trigger_cmd.push(format!("{workflow}"));
        trigger_cmd.push("knative");
        trigger_cmd.push("curl_cmd.sh");
        let output = Command::new(trigger_cmd.clone())
            .output()
            .expect("invrs(eval): failed to execute trigger command");

        match output.status.code() {
                Some(0) => {
                    debug!("{trigger_cmd:?}: executed succesfully");
                }
                Some(code) => {
                    let stderr = str::from_utf8(&output.stderr).unwrap_or("tlessctl(eval): failed to get stderr");
                    panic!("{trigger_cmd:?}: exited with error (code: {code}): {stderr}");
                }
                None => {
                    let stderr = str::from_utf8(&output.stderr).unwrap_or("tlessctl(eval): failed to get stderr");
                    panic!("{trigger_cmd:?}: failed: {stderr}");
                }
            };

        // Specific per-workflow completion detection
        match workflow {
            AvailableWorkflow::Finra => {
                let result_key = format!("{workflow}/outputs/merge/results.txt");

                match S3::wait_for_key(EVAL_BUCKET_NAME, result_key.as_str()).await {
                    Some(time) => {
                        exp_result.end_time = time;
                        S3::clear_object(EVAL_BUCKET_NAME, result_key.as_str()).await;

                        // For FINRA we also need to delete two other files
                        // that we await on throughout workflow execution
                        S3::clear_object(EVAL_BUCKET_NAME, "finra/outputs/fetch-public/trades")
                            .await;
                        S3::clear_object(EVAL_BUCKET_NAME, "finra/outputs/fetch-private/portfolio")
                            .await;
                    }
                    None => error!("invrs(eval): timed-out waiting for FINRA workload to finish"),
                }
            }
            AvailableWorkflow::MlTraining => {
                let result_key = format!("{workflow}/outputs/done.txt");

                match S3::wait_for_key(EVAL_BUCKET_NAME, result_key.as_str()).await {
                    Some(time) => {
                        exp_result.end_time = time;
                        S3::clear_object(EVAL_BUCKET_NAME, result_key.as_str()).await;
                    }
                    None => {
                        error!("invrs(eval): timed-out waiting for ML training workload to finish")
                    }
                }
            }
            AvailableWorkflow::MlInference => {
                // ML Inference finishes off in a scale-out, so we need to
                // wait for as many functions as we have invoked

                match S3::wait_for_key(
                    EVAL_BUCKET_NAME,
                    format!("{workflow}/outputs/predict/done.txt").as_str(),
                )
                .await
                {
                    Some(time) => {
                        exp_result.end_time = time;
                        // Remove all the outputs directory
                        S3::clear_dir(
                            EVAL_BUCKET_NAME.to_string(),
                            "ml-inference/outputs".to_string(),
                        )
                        .await;
                    }
                    None => {
                        error!("invrs(eval): timed-out waiting for ML training workload to finish")
                    }
                }
            }
            AvailableWorkflow::WordCount => {
                // First wait for the result key
                let result_key = format!("{workflow}/outputs/aggregated-results.txt");

                match S3::wait_for_key(EVAL_BUCKET_NAME, result_key.as_str()).await {
                    Some(time) => {
                        // If succesful, remove the result key
                        exp_result.end_time = time;
                        S3::clear_object(EVAL_BUCKET_NAME, result_key.as_str()).await;
                    }
                    None => {
                        error!("invrs(eval): timed-out waiting for Word Count workload to finish")
                    }
                }
            }
        }

        // Common-clean-up
        S3::clear_dir(EVAL_BUCKET_NAME.to_string(), "{workflow}/exec-tokens".to_string()).await;

        // Per-experiment, per-workflow clean-up
        match exp {
            EvalExperiment::E2eLatencyCold => {
                debug!("tlesssctl: {exp}: waiting for scale-to-zero...");
                Self::wait_for_scale_to_zero().await;
            },
            _ => debug!("tlessctl: {exp}: noting to clean-up after single execution"),
        }

        // Cautionary sleep between runs
        thread::sleep(time::Duration::from_secs(5));

        return exp_result;
    }

    async fn run_knative_experiment(exp: &EvalExperiment, args: &EvalRunArgs, args_offset: usize) {
        let baseline = args.baseline[args_offset].clone();

        // First, deploy the common services
        let mut k8s_common_path = Workflows::get_root();
        k8s_common_path.push("k8s_common.yaml");
        Self::run_kubectl_cmd(&format!("apply -f {}", k8s_common_path.display()));

        // Wait for the MinIO pod to be ready
        Self::wait_for_pod("tless", "tless.workflows/name=minio");

        // Get the MinIO URL
        let minio_url = Self::run_kubectl_cmd("-n tless get services -o jsonpath={.items[?(@.metadata.name==\"minio\")].spec.clusterIP}");
        unsafe {
            env::set_var("MINIO_URL", minio_url);
        }

        // Upload the state for all workflows
        // TODO: add progress bar
        // TODO: consider re-using between baselines
        // Workflows::upload_workflow_state(&AvailableWorkflow::MlInference, EVAL_BUCKET_NAME, true).await;
        let pb = Self::get_progress_bar(
            AvailableWorkflow::iter_variants().len().try_into().unwrap(), exp, &baseline, "state");
        for workflow in AvailableWorkflow::iter_variants() {
            Workflows::upload_workflow_state(workflow, EVAL_BUCKET_NAME, true).await;
            pb.inc(1);
        }
        pb.finish();

        // Execute each workload individually
        // for workflow in vec![&AvailableWorkflow::MlInference] {
        for workflow in AvailableWorkflow::iter_variants() {
            // Initialise result file
            Self::init_data_file(workflow, &exp, &baseline);

            // Prepare progress bar for each different experiment
            let pb = Self::get_progress_bar(args.num_repeats.into(), exp, &baseline, format!("{workflow}").as_str());

            // Deploy workflow
            Self::deploy_workflow(workflow, &baseline);

            // TODO: FIXME: consider differntiating between cold and warm starts!

            // Do warm-up rounds
            for _ in 0..args.num_warmup_repeats {
                Self::run_workflow_once(workflow, exp).await;
                S3::clear_dir(EVAL_BUCKET_NAME.to_string(), format!("{workflow}/exec-tokens")).await;
            }

            // Do actual experiment
            for i in 0..args.num_repeats {
                let mut result = Self::run_workflow_once(workflow, exp).await;
                S3::clear_dir(EVAL_BUCKET_NAME.to_string(), format!("{workflow}/exec-tokens")).await;
                result.iter = i;
                Self::write_result_to_file(workflow, &exp, &baseline, &result);

                pb.inc(1);
            }

            // Delete workflow
            Self::delete_workflow(workflow, &baseline);

            // Finish progress bar
            pb.finish();
        }

        // Experiment-wide clean-up
        let mut k8s_common_path = Workflows::get_root();
        k8s_common_path.push("k8s_common.yaml");
        Self::run_kubectl_cmd(&format!("delete -f {}", k8s_common_path.display()));
    }

    // ------------------------------------------------------------------------
    // Run with Faasm Functions
    // ------------------------------------------------------------------------

    fn run_faasmctl_cmd(cmd: &str) -> String {
        debug!("invrs(eval): executing faasmctl command: {cmd}");
        // let args: Vec<&str> = cmd.split_whitespace().collect();
        // Need to use shell_words to properly preserve double quotes
        let args = shell_words::split(cmd).unwrap();

        let output = Command::new("faasmctl")
            .args(&args[0..])
            .output()
            .expect("invrs(eval): failed to execute faasmctl command");

        let stderr = String::from_utf8(output.stderr)
            .expect("invrs(eval): failed to convert faasmctl command output to string");
        debug!("faasmctl stderr: {stderr}");

        let stdout = String::from_utf8(output.stdout)
            .expect("invrs(eval): failed to convert faasmctl command output to string");
        debug!("faasmctl stdout: {stdout}");
        stdout
    }

    fn upload_wasm() {
        // Upload state for different workflows from the experiments container
        let docker_tag = Docker::get_docker_tag(&DockerContainer::Experiments);

        for workflow in AvailableWorkflow::iter_variants() {
            let ctr_path = format!("/usr/local/faasm/wasm/{workflow}");

            Self::run_faasmctl_cmd(
                &format!("upload.workflow {workflow} {docker_tag}:{ctr_path}").to_string(),
            );
        }
    }

    fn epoch_ts_to_datetime(epoch_str: &str) -> DateTime<Utc> {
        let epoch_seconds: f64 = epoch_str.parse().unwrap();
        let secs = epoch_seconds as i64;
        let nanos = ((epoch_seconds - secs as f64) * 1_000_000_000.0) as u32;

        Utc.timestamp_opt(secs, nanos).single().unwrap()
    }

    async fn run_faasm_experiment(exp: &EvalExperiment, args: &EvalRunArgs, args_offset: usize) {
        let baseline = args.baseline[args_offset].clone();

        // First, work out the WASM VM we need
        let wasm_vm = match baseline {
            EvalBaseline::Faasm => "wamr",
            EvalBaseline::SgxFaasm | EvalBaseline::TlessFaasm => "sgx",
            _ => panic!("invrs(eval): should not be here"),
        };

        let tless_enabled = match baseline {
            EvalBaseline::Faasm | EvalBaseline::SgxFaasm => "off",
            EvalBaseline::TlessFaasm => "on",
            _ => panic!("invrs(eval): should not be here"),
        };

        env::set_var("FAASM_WASM_VM", wasm_vm);
        env::set_var("TLESS_ENABLED", tless_enabled);
        // TODO: uncomment when deploying on k8s
        // Self::run_faasmctl_cmd("deploy.k8s --workers=4");

        // Second, work-out the MinIO URL
        let mut minio_url = Self::run_faasmctl_cmd("s3.get-url");
        minio_url = minio_url.strip_suffix("\n").unwrap().to_string();
        unsafe {
            env::set_var("MINIO_URL", minio_url);
        }

        async fn cleanup_single_execution(workflow: &AvailableWorkflow, exp: &EvalExperiment) {
            S3::clear_dir(EVAL_BUCKET_NAME.to_string(), format!("{workflow}/exec-tokens")).await;

            match exp {
                EvalExperiment::E2eLatencyCold => {
                    debug!("Flushing Faasm workers and sleeping...");
                    Eval::run_faasmctl_cmd("flush.workers");
                    thread::sleep(time::Duration::from_secs(2));
                },
                _ => debug!("nothing to do"),
            }
        }

        // Upload the state for all workflows
        // TODO: undo me
        let pb = Self::get_progress_bar(
            AvailableWorkflow::iter_variants().len().try_into().unwrap(), exp, &baseline, "state");
        // for workflow in vec![&AvailableWorkflow::WordCount] {
        for workflow in AvailableWorkflow::iter_variants() {
            Workflows::upload_workflow_state(workflow, EVAL_BUCKET_NAME, true).await;
            pb.inc(1);
        }
        pb.finish();

        // Upload the WASM files for all workflows
        // TODO: add progress bar
        // Self::upload_wasm();

        // Invoke each workflow
        // UNDO ME
        // for workflow in vec![&AvailableWorkflow::WordCount] {
        for workflow in AvailableWorkflow::iter_variants() {
            let faasm_cmdline = Workflows::get_faasm_cmdline(workflow);

            // Initialise result file
            Self::init_data_file(workflow, &exp, &baseline);

            // Prepare progress bar for each different experiment
            let pb = Self::get_progress_bar(args.num_repeats.into(), exp, &baseline, format!("{workflow}").as_str());

            let faasmctl_cmd = format!(
                "invoke {workflow} driver --cmdline \"{faasm_cmdline}\" --output-format start-end-ts"
            );
            // Do warm-up rounds
            for _ in 0..args.num_warmup_repeats {
                Self::run_faasmctl_cmd(&faasmctl_cmd);
                cleanup_single_execution(workflow, exp).await;
            }

            // Do actual experiment
            for i in 0..args.num_repeats {
                let mut output = Self::run_faasmctl_cmd(&faasmctl_cmd);
                output = output.strip_suffix("\n").unwrap().to_string();

                let ts = output.split(",").collect::<Vec<&str>>();
                let result = ExecutionResult {
                    start_time: Self::epoch_ts_to_datetime(ts[0]),
                    end_time: Self::epoch_ts_to_datetime(ts[1]),
                    iter: i,
                };

                Self::write_result_to_file(workflow, &exp, &baseline, &result);

                // Clean-up
                cleanup_single_execution(workflow, exp).await;

                pb.inc(1);
            }

            // Finish progress bar
            pb.finish();
        }
    }

    pub async fn run(exp: &EvalExperiment, args: &EvalRunArgs) {
        for i in 0..args.baseline.len() {
            match args.baseline[i] {
                EvalBaseline::Knative | EvalBaseline::CcKnative | EvalBaseline::TlessKnative => {
                    Self::run_knative_experiment(exp, args, i).await;
                }
                EvalBaseline::Faasm | EvalBaseline::SgxFaasm | EvalBaseline::TlessFaasm => {
                    Self::run_faasm_experiment(exp, args, i).await;
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // Plotting Functions
    // ------------------------------------------------------------------------

    fn is_faasm_baseline(baseline: &EvalBaseline) -> bool {
        match baseline {
            EvalBaseline::Knative | EvalBaseline::CcKnative | EvalBaseline::TlessKnative => false,
            EvalBaseline::Faasm | EvalBaseline::SgxFaasm | EvalBaseline::TlessFaasm => true,
        }
    }

    fn plot_e2e_latency(exp: &EvalExperiment, data_files: &Vec<PathBuf>) {
        #[derive(Debug, Deserialize)]
        #[serde(rename_all = "PascalCase")]
        struct Record {
            #[allow(dead_code)]
            run: u32,
            time_ms: u64,
        }

        // Initialize the structure to hold the data
        let mut data = BTreeMap::<AvailableWorkflow, BTreeMap<EvalBaseline, f64>>::new();
        for workflow in AvailableWorkflow::iter_variants() {
            let mut inner_map = BTreeMap::<EvalBaseline, f64>::new();
            for baseline in EvalBaseline::iter_variants() {
                inner_map.insert(baseline.clone(), 0.0);
            }
            data.insert(workflow.clone(), inner_map);
        }

        let num_workflows = AvailableWorkflow::iter_variants().len();
        let num_baselines = EvalBaseline::iter_variants().len();
        let mut y_max = 0.0;
        // Each bar has width 1 and we add padding bars between workflows
        let x_max = num_baselines * num_workflows + num_workflows + 1;

        // Collect data
        for csv_file in data_files {
            let file_name = csv_file
                .file_name()
                .and_then(|f| f.to_str())
                .unwrap_or_default();
            let file_name_len = file_name.len();
            let file_name_no_ext = &file_name[0..file_name_len - 4];

            let wflow: AvailableWorkflow = file_name_no_ext.split("_").collect::<Vec<&str>>()[1]
                .parse()
                .unwrap();
            let baseline: EvalBaseline = file_name_no_ext.split("_").collect::<Vec<&str>>()[0]
                .parse()
                .unwrap();

            // Open the CSV and deserialize records
            let mut reader = ReaderBuilder::new()
                .has_headers(true)
                .from_path(csv_file)
                .unwrap();
            let mut total_time = 0;
            let mut count = 0;

            for result in reader.deserialize() {
                let record: Record = result.unwrap();
                total_time += record.time_ms;
                count += 1;
            }

            let average_time = data.get_mut(&wflow).unwrap().get_mut(&baseline).unwrap();
            *average_time = total_time as f64 / count as f64;

            if *average_time > y_max {
                y_max = *average_time;
            }
        }

        let mut plot_path = env::current_dir().expect("invrs: failed to get current directory");
        plot_path.push("eval");
        plot_path.push(format!("{exp}"));
        plot_path.push("plots");
        plot_path.push(format!("{}.svg", exp.to_string().replace("-", "_")));

        // Plot data
        let root = SVGBackend::new(&plot_path, (800, 300)).into_drawing_area();
        root.fill(&WHITE).unwrap();

        let mut chart = ChartBuilder::on(&root)
            .x_label_area_size(40)
            .y_label_area_size(40)
            .margin(10)
            .margin_top(40)
            .build_cartesian_2d(0..x_max, 0f64..5f64)
            .unwrap();

        chart
            .configure_mesh()
            // .y_desc("Slowdown vs Non-Confidential")
            .y_label_style(("sans-serif", 20).into_font()) // Set y-axis label font and size
            .x_desc("")
            // .x_labels(0)
            .x_label_formatter(&|_| format!(""))
            .y_labels(10)
            .disable_x_mesh()
            .disable_x_axis()
            .y_label_formatter(&|y| format!("{:.0}", y))
            /*
            .light_line_style(ShapeStyle {
                color: RGBColor(200, 200, 200).to_rgba().mix(0.5),
                filled: true,
                stroke_width: 1,
            })
            */
            .draw()
            .unwrap();

        // Manually draw the y-axis label with a custom font and size
        root.draw(&Text::new(
            "Slowdown (vs non-confidential)",
            (5, 260),
            ("sans-serif", 20)
                .into_font()
                .transform(FontTransform::Rotate270)
                .color(&BLACK),
        ))
        .unwrap();

        // Draw bars
        for (w_idx, (workflow, workflow_data)) in data.iter().enumerate() {
            let x_orig = w_idx * (num_baselines + 1);

            // Work-out the slowest value for each set of baselines
            let y_faasm : f64 = *workflow_data.get(&EvalBaseline::Faasm).unwrap();
            let y_knative : f64 = *workflow_data.get(&EvalBaseline::Knative).unwrap();

            chart
                .draw_series((0..).zip(workflow_data.iter()).map(|(x, (baseline, y))| {
                    // Bar style
                    let bar_style = ShapeStyle {
                        color: baseline.get_color().into(),
                        filled: true,
                        stroke_width: 2,
                    };

                    let this_y;
                    if Self::is_faasm_baseline(baseline) {
                        this_y = (y / y_faasm) as f64;
                    } else {
                        this_y = (y / y_knative) as f64;
                    }

                    let mut bar =
                        Rectangle::new([(x_orig + x, 0 as f64), (x_orig + x + 1, this_y as f64)], bar_style);
                    bar.set_margin(0, 0, 2, 2);
                    bar
                }))
                .unwrap();

            for (x, (baseline, y)) in (0..).zip(workflow_data.iter()) {
                let this_y;
                if Self::is_faasm_baseline(baseline) {
                    this_y = (y / y_faasm) as f64;
                } else {
                    this_y = (y / y_knative) as f64;
                }

                // Add text
                let y_offset = match this_y > 5.0 {
                    true => - 0.1,
                    false => 0.25,
                };
                chart.plotting_area().draw(&Text::new(
                    format!("{:.1}", this_y),
                    (x_orig + x, (this_y + y_offset) as f64),
                    ("sans-serif", 15).into_font(),
                ))
                .unwrap();
            }


            // Add label for the workflow
            let x_workflow_label = x_orig + num_baselines / 2 - 1;
            let label_px_coordinate = chart
                .plotting_area()
                .map_coordinate(&(x_workflow_label, - 0.25));
            root.draw(&Text::new(
                format!("{workflow}"),
                label_px_coordinate,
                ("sans-serif", 20).into_font(),
            ))
            .unwrap();
        }

        // Add solid frames
        // TODO: we could add whitespaces in the horizontal lines
        chart
            .plotting_area()
            .draw(&PathElement::new(
                vec![
                    (0, 100 as f64),
                    (x_max, 100 as f64),
                ],
                &BLACK,
            ))
            .unwrap();
        chart
            .plotting_area()
            .draw(&PathElement::new(
                vec![
                    (x_max, 0 as f64),
                    (x_max, 100 as f64),
                ],
                &BLACK,
            ))
            .unwrap();
        chart
            .plotting_area()
            .draw(&PathElement::new(
                vec![
                    (0, 0 as f64),
                    (x_max, 0 as f64),
                ],
                &BLACK,
            ))
            .unwrap();

        // Manually draw the legend outside the grid, above the chart
        let legend_x_start = 50;
        let legend_y_pos = 6; // Position above the chart

        for (idx, baseline) in EvalBaseline::iter_variants().enumerate() {
            // Calculate position for each legend item
            let x_pos = legend_x_start + idx as i32 * 120;
            let y_pos = legend_y_pos;

            // Draw the color box (Rectangle)
            root.draw(&Rectangle::new(
                [(x_pos, y_pos), (x_pos + 20, y_pos + 20)],
                baseline.get_color().filled(),
            ))
            .unwrap();

            let mut label = format!("{baseline}");
            if baseline == &EvalBaseline::CcKnative {
                label = "sev-knative".to_string();
            }

            // Draw the baseline label (Text)
            root.draw(&Text::new(
                label,
                (x_pos + 30, y_pos + 5), // Adjust text position
                ("sans-serif", 20).into_font(),
            ))
            .unwrap();
        }

        root.present().unwrap();
    }

    pub fn plot(exp: &EvalExperiment) {
        // First, get all the data files
        let data_files = Self::get_all_data_files(exp);

        match exp {
            EvalExperiment::E2eLatency => {
                Self::plot_e2e_latency(&exp, &data_files);
            }
            EvalExperiment::E2eLatencyCold => {
                Self::plot_e2e_latency(&exp, &data_files);
            }
        }
    }
}
