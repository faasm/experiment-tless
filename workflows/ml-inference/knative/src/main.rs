use cloudevents::binding::reqwest::RequestBuilderExt;
use cloudevents::binding::warp::{filter, reply};
use cloudevents::{AttributesReader, AttributesWriter, Event};
use futures_util::StreamExt;
use minio::s3::args::*;
use minio::s3::client::ClientBuilder;
use minio::s3::creds::StaticProvider;
use minio::s3::error::Error;
use minio::s3::http::BaseUrl;
use minio::s3::types::ToStream;
use serde_json::{json, Value};
use std::process::{Command, Stdio};
use std::{env, fs, thread, time};
use tokio::task::JoinHandle;
use warp::Filter;

static BINARY_DIR: &str = "/workflows/build-native/ml-inference";
static WORKFLOW_NAME: &str = "ml-inference(driver)";

struct S3Data {
    data: &'static str,
}

impl S3Data {
    const HOST: S3Data = S3Data { data: "minio" };
    const PORT: S3Data = S3Data { data: "9000" };
    const USER: S3Data = S3Data { data: "minio" };
    const PASSWORD: S3Data = S3Data { data: "minio123" };
    const BUCKET: S3Data = S3Data { data: "tless" };
}

pub fn get_tless_mode() -> String {
    match env::var("TLESS_MODE") {
        Ok(value) => {
            match value.as_str() {
                "on" => "on".to_string(),
                _ => "off".to_string(),
            }
        }
        _ => "off".to_string(),
    }
}

pub async fn get_num_keys(prefix: &str) -> i64 {
    let base_url = format!("http://{}:{}", S3Data::HOST.data, S3Data::PORT.data)
        .parse::<BaseUrl>()
        .unwrap();

    let static_provider = StaticProvider::new(S3Data::USER.data, S3Data::PASSWORD.data, None);
    let client = ClientBuilder::new(base_url.clone())
        .provider(Some(Box::new(static_provider)))
        .build()
        .unwrap();

    let mut objects = client
        .list_objects(&S3Data::BUCKET.data)
        .recursive(true)
        .prefix(Some(prefix.to_string()))
        .to_stream()
        .await;

    let mut num_keys = 0;
    while let Some(result) = objects.next().await {
        match result {
            Ok(resp) => {
                for _ in resp.contents {
                    num_keys += 1;
                }
            }
            Err(e) => panic!(
                "ml-inference(driver): error listing keys with prefix: {prefix}: {}",
                e
            ),
        }
    }

    num_keys
}

pub async fn add_key_str(key: &str, content: &str) {
    let base_url = format!("http://{}:{}", S3Data::HOST.data, S3Data::PORT.data)
        .parse::<BaseUrl>()
        .unwrap();

    let static_provider = StaticProvider::new(S3Data::USER.data, S3Data::PASSWORD.data, None);
    let client = ClientBuilder::new(base_url.clone())
        .provider(Some(Box::new(static_provider)))
        .build()
        .unwrap();

    client
        .put_object_content(&S3Data::BUCKET.data, key, content.to_string())
        .send()
        .await
        .unwrap();
}

pub async fn wait_for_key(key_name: &str) {
    let base_url = format!("http://{}:{}", S3Data::HOST.data, S3Data::PORT.data)
        .parse::<BaseUrl>()
        .unwrap();

    let static_provider = StaticProvider::new(S3Data::USER.data, S3Data::PASSWORD.data, None);
    let client = ClientBuilder::new(base_url.clone())
        .provider(Some(Box::new(static_provider)))
        .build()
        .unwrap();

    // Return fast if the bucket does not exist
    let exists: bool = client
        .bucket_exists(&BucketExistsArgs::new(&S3Data::BUCKET.data).unwrap())
        .await
        .unwrap();

    if !exists {
        panic!(
            "{WORKFLOW_NAME}: waiting for key ({key_name}) in non-existant bucket: {}",
            S3Data::BUCKET.data
        );
    }

    // Loop until the object appears
    loop {
        let mut objects = client
            .list_objects(&S3Data::BUCKET.data)
            .recursive(true)
            .prefix(Some(key_name.to_string()))
            .to_stream()
            .await;

        while let Some(result) = objects.next().await {
            match result {
                Ok(_) => return,
                Err(e) => match e {
                    Error::S3Error(s3_error) => match s3_error.code.as_str() {
                        _ => panic!("{WORKFLOW_NAME}: error: {}", s3_error.message),
                    },
                    _ => panic!("{WORKFLOW_NAME}: error: {}", e),
                },
            }
        }

        thread::sleep(time::Duration::from_secs(2));
    }
}

// We must wait for the POST event to go through before we can return, as
// otherwise the chain may not make progress
pub fn post_event(dest: String, event: Event) -> JoinHandle<()> {
    tokio::spawn(async {
        reqwest::Client::new()
            .post(dest)
            .event(event)
            .map_err(|e| e.to_string())
            .unwrap()
            .header("Access-Control-Allow-Origin", "*")
            .send()
            .await
            .map_err(|e| e.to_string())
            .unwrap();
    })
}

pub fn get_json_from_event(event: &Event) -> Value {
    match event.data() {
        Some(cloudevents::Data::Json(json)) => Some(json.clone()),
        Some(cloudevents::Data::String(text)) => serde_json::from_str(&text).ok(),
        Some(cloudevents::Data::Binary(bytes)) => serde_json::from_slice(bytes).ok(),
        _ => panic!("tless(driver): error: must be json data"),
    }
    .unwrap()
}

// This function is a general wrapper that takes a cloud event as an input,
// decides what function to execute, and outputs another cloud event
pub fn process_event(mut event: Event) -> Event {
    // -----
    // Pre-process and function invocation
    // -----

    event.set_source(match event.source().as_str() {
        "cli-partition" => {
            let func_name = "partition";
            println!("{WORKFLOW_NAME}: executing '{func_name}' from cli: {event}");

            let json = get_json_from_event(&event);
            let data_dir = json
                .get("data-dir")
                .and_then(Value::as_str)
                .expect("ml-inference(driver): error: cannot find 'data-dir' in CE");

            let num_inf_funcs: i64 = get_json_from_event(&event)
                .get("num-inf-funcs")
                .and_then(Value::as_i64)
                .expect("ml-inference(driver): error: cannot find 'num-inf-funcs' in CE");

            match Command::new(format!("{}/ml-inference_{func_name}", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", S3Data::BUCKET.data)
                .env("S3_HOST", S3Data::HOST.data)
                .env("S3_PASSWORD", S3Data::PASSWORD.data)
                .env("S3_PORT", S3Data::PORT.data)
                .env("S3_USER", S3Data::USER.data)
                .env("TLESS_MODE", get_tless_mode())
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .arg(data_dir)
                .arg(num_inf_funcs.to_string())
                .output()
                .expect("ml-training(driver): error: spawning partition command")
                .status
                .code()
            {
                Some(0) => {
                    println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully")
                }
                Some(code) => {
                    panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}")
                }
                None => {
                    panic!("{WORKFLOW_NAME}: '{func_name}' failed")
                }
            }

            "pre-inf"
        }
        "cli-load" => {
            let func_name = "load";
            println!("{WORKFLOW_NAME}: executing '{func_name}' from partition: {event}");

            let json = get_json_from_event(&event);
            let model_dir = json
                .get("model-dir")
                .and_then(Value::as_str)
                .expect("ml-inference(driver): error: cannot find 'model-dir' in CE");

            match Command::new(format!("{}/ml-inference_{func_name}", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", S3Data::BUCKET.data)
                .env("S3_HOST", S3Data::HOST.data)
                .env("S3_PASSWORD", S3Data::PASSWORD.data)
                .env("S3_PORT", S3Data::PORT.data)
                .env("S3_USER", S3Data::USER.data)
                .env("TLESS_MODE", get_tless_mode())
                .arg(model_dir)
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .output()
                .expect("ml-inference(driver): failed spawning 'load' command")
                .status
                .code()
            {
                Some(0) => {
                    println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully")
                }
                Some(code) => {
                    panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}")
                }
                None => {
                    panic!("{WORKFLOW_NAME}: '{func_name}' failed")
                }
            }

            "pre-inf"
        }
        "pre-inf" => {
            let func_name = "predict";
            println!("{WORKFLOW_NAME}: executing '{func_name}' from 'pca': {event}");

            let inf_id: i64 = get_json_from_event(&event)
                .get("inf-id")
                .and_then(Value::as_i64)
                .expect("ml-inference(driver): error: cannot find 'inf-id' in CE");

            // Execute the function only after enough POST requests have
            // been received
            match Command::new(format!("{}/ml-inference_{func_name}", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", S3Data::BUCKET.data)
                .env("S3_HOST", S3Data::HOST.data)
                .env("S3_PASSWORD", S3Data::PASSWORD.data)
                .env("S3_PORT", S3Data::PORT.data)
                .env("S3_USER", S3Data::USER.data)
                .env("TLESS_MODE", get_tless_mode())
                .arg(inf_id.to_string())
                .arg("ml-inference/outputs/load/rf-")
                .arg(format!("ml-inference/outputs/partition/inf-{inf_id}"))
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .output()
                .expect("ml-inference(driver): failed executing 'predict' command")
                .status
                .code()
            {
                Some(0) => {
                    println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully")
                }
                Some(code) => {
                    panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}")
                }
                None => {
                    panic!("{WORKFLOW_NAME}: '{func_name}' failed")
                }
            }

            "predict"
        }
        _ => panic!(
            "{WORKFLOW_NAME}: error: unrecognised source: {:}",
            event.source()
        ),
    });

    // -----
    // Post-process
    // -----

    match event.source().as_str() {
        // Process the output of the 'partition' or 'load' functions and chain to 'predict'
        "pre-inf" => {
            // It is important that both partition and load use the same magic
            // to trigger the same job
            let run_magic: i64 = get_json_from_event(&event)
                .get("run-magic")
                .and_then(Value::as_i64)
                .expect("ml-training(driver): error: cannot find 'run-magic' in CE");

            let num_inf_funcs: i64 = get_json_from_event(&event)
                .get("num-inf-funcs")
                .and_then(Value::as_i64)
                .expect("ml-training(driver): error: cannot find 'num_inf_funcs' in CE");

            let json = get_json_from_event(&event);
            let model_dir = json
                .get("model-dir")
                .and_then(Value::as_str)
                .expect("ml-inference(driver): error: cannot find 'model-dir' in CE");

            let json_2 = get_json_from_event(&event);
            let data_dir = json_2
                .get("data-dir")
                .and_then(Value::as_str)
                .expect("ml-inference(driver): error: cannot find 'data-dir' in CE");

            // Predict messages willl go to void
            let mut scaled_event = event.clone();
            scaled_event.set_type("http://predict-to-void-kn-channel.tless.svc.cluster.local");

            for i in 1..num_inf_funcs {
                scaled_event.set_id((run_magic + i).to_string());
                scaled_event.set_data(
                    "aplication/json",
                    json!({
                        "inf-id": i,
                        "num-inf-funcs": num_inf_funcs,
                        "run-magic": run_magic,
                        "model-dir": model_dir,
                        "data-dir": data_dir,
                    }),
                );

                println!(
                    "{WORKFLOW_NAME}: posting to {} event {i}/{num_inf_funcs}: {scaled_event}",
                    event.ty(),
                );
                post_event(event.ty().to_string(), scaled_event.clone());

                // Be gentle when scaling-up, as otherwise SEV will take too
                // long
                println!("{WORKFLOW_NAME}: sleeping for a bit...");
                thread::sleep(time::Duration::from_secs(3));
            }

            // Update the event for the zero-th id (the one we return as part
            // of the method)
            scaled_event.set_id((run_magic + 0).to_string());
            scaled_event.set_data(
                "aplication/json",
                json!({
                    "inf-id": 0,
                    "num-inf-funcs": num_inf_funcs,
                    "run-magic": run_magic,
                    "model-dir": model_dir,
                    "data-dir": data_dir,
                }),
            );

            scaled_event
        }
        "predict" => {
            // Predict is the last function so we don't need to do anything
            event
        }
        _ => panic!(
            "{WORKFLOW_NAME}: error: unrecognised destination: {:}",
            event.source()
        ),
    }
}

#[tokio::main]
async fn main() {
    match env::var("CE_FROM_FILE") {
        Ok(value) => {
            assert!(value == "on");

            // This filepath is hard-coded in the JobSink specification:
            // https://knative.dev/docs/eventing/sinks/job-sink
            let file_contents = fs::read_to_string("/etc/jobsink-event/event").unwrap();
            let json_value = serde_json::from_str(&file_contents).unwrap();
            let event: Event = serde_json::from_value(json_value).unwrap();

            // Each inference job will be triggered by both partition and load
            // with the same (source, id) pair (triggering one job) which means
            // that the other one may not have finished yet. To this extent,
            // we wait here for both to finish before executing
            let keys_to_wait = vec![
                "ml-training/outputs/partition/done.txt",
                "ml-training/outputs/load/done.txt",
            ];
            for key_to_wait in keys_to_wait {
                println!("{WORKFLOW_NAME}: audit: waiting for key {key_to_wait}");
                wait_for_key(key_to_wait).await;
            }

            process_event(event.clone());

            let num_inf_funcs: i64 = get_json_from_event(&event)
                .get("num-inf-funcs")
                .and_then(Value::as_i64)
                .expect("ml-inference(driver): error: cannot find 'num-inf-funcs' in CE");

            // After executing the predict function (only JobSink in this
            // workflow) we are done so we need to check if all other jobs have
            // finished, and if so, right to a key letting know we are done
            let num_keys = get_num_keys("ml-inference/outputs/predict-").await;

            println!(
                "{WORKFLOW_NAME}: queried number of keys (got: {num_keys} - want: {num_inf_funcs})"
            );
            if num_keys == num_inf_funcs {
                println!("{WORKFLOW_NAME}: done!");
                add_key_str("ml-inference/outputs/predict/done.txt", "done!").await;
            }

            // We are also done, so we do not need to process the event
        }
        Err(env::VarError::NotPresent) => {
            let routes = warp::any()
                // Extract event from request
                .and(filter::to_event())
                // Return the post-processed event
                .map(|event| reply::from_event(process_event(event)));

            warp::serve(routes).run(([127, 0, 0, 1], 8080)).await;
        }
        Err(e) => println!("Failed to read env. var: {}", e),
    };
}
