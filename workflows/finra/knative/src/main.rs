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
use once_cell::sync::Lazy;
use serde_json::{json, Value};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::{env, fs, thread, time};
use tokio::task::JoinHandle;
use warp::Filter;

static BINARY_DIR: &str = "/workflows/build-native/finra";
static MERGE_INVOCATION_COUNTER: Lazy<Arc<Mutex<i64>>> = Lazy::new(|| Arc::new(Mutex::new(0)));
static WORKFLOW_NAME: &str = "finra(driver)";

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
        panic!("{WORKFLOW_NAME}: waiting for key ({key_name}) in non-existant bucket: {}", S3Data::BUCKET.data);
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
                Ok(_) => { return }
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
        "cli-fetch-public" => {
            let func_name = "fetch-public";
            println!("{WORKFLOW_NAME}: executing '{func_name}' from cli: {event}");

            match Command::new(format!("{}/finra_{func_name}", BINARY_DIR))
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
                .arg("finra/yfinance.csv")
                .output()
                .expect("finra(driver): error: spawning executing fetch-public command")
                .status
                .code() {
                Some(0) => { println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully") },
                Some(code) => { panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}") },
                None => { panic!("{WORKFLOW_NAME}: '{func_name}' failed") },
            }

            "audit"
        }
        "cli-fetch-private" => {
            let func_name = "fetch-private";
            println!("{WORKFLOW_NAME}: executing 'fetch-private' from cli: {event}");

            match Command::new(format!("{}/finra_{func_name}", BINARY_DIR))
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
                .output()
                .expect("finra(driver): failed executing command")
                .status
                .code() {
                Some(0) => { println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully") },
                Some(code) => { panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}") },
                None => { panic!("{WORKFLOW_NAME}: '{func_name}' failed") },
            }

            "audit"
        }
        "audit" => {
            let func_name = "audit";
            println!("{WORKFLOW_NAME}: executing '{func_name}' from 'fetch-*': {event}");

            // We must spawn N audit functions after both fetch-public and
            // fetch-private have finished executing. The way we do this is
            // that we trigger two times N cloud events, pairwise identical.
            // This means that they will be picked-up by the same job, but each
            // job will receive two of them.

            let audit_id: i64 = get_json_from_event(&event)
                .get("audit-id")
                .and_then(Value::as_i64)
                .expect("finra(driver): error: cannot find 'audit-id' in CE");

            // Execute the function only after enough POST requests have
            // been received
            match Command::new(format!("{}/finra_{func_name}", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", S3Data::BUCKET.data)
                .env("S3_HOST", S3Data::HOST.data)
                .env("S3_PASSWORD", S3Data::PASSWORD.data)
                .env("S3_PORT", S3Data::PORT.data)
                .env("S3_USER", S3Data::USER.data)
                .env("TLESS_MODE", get_tless_mode())
                .arg(audit_id.to_string())
                .arg("finra/outputs/fetch-public/trades")
                .arg("finra/outputs/fetch-private/portfolio")
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .output()
                .expect("finra(driver): failed executing 'audit' command")
                .status
                .code() {
                Some(0) => { println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully") },
                Some(code) => { panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}") },
                None => { panic!("{WORKFLOW_NAME}: '{func_name}' failed") },
            }

            "merge"
        }
        "merge" => {
            let func_name = "merge";
            println!("{WORKFLOW_NAME}: executing '{func_name}' from 'audit': {event}");

            let num_audit: i64 = get_json_from_event(&event)
                .get("num-audit")
                .and_then(Value::as_i64)
                .expect("finra(driver): error: cannot find 'num-audit' in CE");

            let mut count = MERGE_INVOCATION_COUNTER.lock().unwrap();
            *count += 1;
            println!("${WORKFLOW_NAME}: counted {}/{}", *count, num_audit);

            if *count == num_audit {
                println!("${WORKFLOW_NAME}: done!");

                match Command::new(format!("{}/finra_{func_name}", BINARY_DIR))
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
                    .output()
                    .expect("finra(driver): failed executing 'merge' command")
                    .status
                    .code() {
                    Some(0) => { println!("{WORKFLOW_NAME}: '{func_name}' executed succesfully") },
                    Some(code) => { panic!("{WORKFLOW_NAME}: '{func_name}' failed with ec: {code}") },
                    None => { panic!("{WORKFLOW_NAME}: '{func_name}' failed") },
                }

                // Reset counter for next (warm) execution
                println!("${WORKFLOW_NAME}: resetting counter to 0");
                *count = 0;
            }

            "done"
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
        // Process the output of the 'fetch-public/private' and chain to 'audit'
        "audit" => {
            let run_magic: i64 = get_json_from_event(&event)
                .get("run-magic")
                .and_then(Value::as_i64)
                .expect("finra(driver): error: cannot find 'run-magic' in CE");

            let num_audit: i64 = get_json_from_event(&event)
                .get("num-audit")
                .and_then(Value::as_i64)
                .expect("finra(driver): error: cannot find 'num-audit' in CE");

            // JobSink executes one event per different CloudEvent id, so we
            // include one magic per run, and spawn two cloud events with the
            // same magic
            let mut scaled_event = event.clone();
            scaled_event.set_type("http://audit-to-merge-kn-channel.tless.svc.cluster.local");

            for i in 1..num_audit {
                scaled_event.set_id((run_magic + i).to_string());
                scaled_event.set_data(
                    "aplication/json",
                    json!({"audit-id": i, "num-audit": num_audit}),
                );

                println!(
                    "{WORKFLOW_NAME}: posting to {} event {i}/{num_audit}: {scaled_event}",
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
                json!({"audit-id": 0, "num-audit": num_audit}),
            );

            scaled_event
        }
        // Process the output of the 'audit' function and chain to 'merge'
        "merge" => {
            // The event already contains the mapper-id as well as the num-audit
            // functions, so we can now safely propagate it with the new source
            event
        }
        // Process the output of the 'merge' function
        "done" => {
            // Nothing to do after "merge" as it is the last step in the chain
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

            // The 'audit' function needs both the 'fetch-public' and
            // 'fetch-private' functions to execute before it can start. At
            // the same time, it is a JobSink, so it is invoked only once,
            // and not as a long-running HTTP server. As a consequence, we
            // cannot use our trick with the static shared counter. Instead,
            // we make sure both 'fetch-public' and 'fetch-private' only
            // trigger one job (using the same id), and here we wait for
            // both of them to finish
            let keys_to_wait = vec!["finra/outputs/fetch-public/trades", "finra/outputs/fetch-private/portfolio"];
            for key_to_wait in keys_to_wait {
                println!("{WORKFLOW_NAME}: audit: waiting for key {key_to_wait}");
                wait_for_key(key_to_wait).await;
            }

            let processed_event = process_event(event);

            // After executing step-two, we just need to post a clone of the
            // event to the type (i.e. destination) provided in it. Given that
            // step-two runs in a JobSink, the pod will terminate on exit, so
            // we need to make sure that the POST is sent before we move on
            println!(
                "{WORKFLOW_NAME}: posting to {} event: {processed_event}",
                processed_event.ty()
            );
            post_event(processed_event.ty().to_string(), processed_event.clone())
                .await
                .unwrap();
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
