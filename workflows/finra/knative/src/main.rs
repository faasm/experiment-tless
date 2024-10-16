use cloudevents::binding::reqwest::RequestBuilderExt;
use cloudevents::binding::warp::{filter, reply};
use cloudevents::{AttributesReader, AttributesWriter, Event};
use once_cell::sync::Lazy;
use serde_json::{json, Value};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::{env, fs, io::BufRead, io::BufReader};
use tokio::task::JoinHandle;
use uuid::Uuid;
use warp::Filter;

static BINARY_DIR: &str = "/code/faasm-examples/workflows/build-native/finra";
static INVOCATION_COUNTER: Lazy<Arc<Mutex<i64>>> = Lazy::new(|| Arc::new(Mutex::new(0)));
static WORKFLOW_NAME: &str = "finra(driver)";

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
        "cli" => {
            println!("{WORKFLOW_NAME}: executing 'splitter' from cli: {event}");

            let fetch_public = Command::new(format!("{}/finra_fetch-public", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", "tless")
                .env("S3_HOST", "minio")
                .env("S3_PASSWORD", "minio123")
                .env("S3_PORT", "9000")
                .env("S3_USER", "minio")
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .arg("finra/yfinance.csv")
                .spawn()
                .expect("finra(driver): failed executing command");

            let fetch_private = Command::new(format!("{}/finra_fetch-private", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", "tless")
                .env("S3_HOST", "minio")
                .env("S3_PASSWORD", "minio123")
                .env("S3_PORT", "9000")
                .env("S3_USER", "minio")
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .spawn()
                .expect("finra(driver): failed executing command");

            "splitter"
        }
        "splitter" => {
            println!("tless(driver): executing 'mapper' from 'splitter': {event}");

            let json_file = get_json_from_event(&event);
            let s3_file = json_file
                .get("input-file")
                .and_then(Value::as_str)
                .expect("tless(driver): error getting 'input-file' from CE");

            let mapper_id: i64 = get_json_from_event(&event)
                .get("mapper-id")
                .and_then(Value::as_i64)
                .expect("tless(driver): error getting 'mapper-id' from CE");

            // Simulate actual function execution by a sleep
            Command::new(format!("{}/word-count_mapper", BINARY_DIR))
                .current_dir(BINARY_DIR)
                .env("LD_LIBRARY_PATH", "/usr/local/lib")
                .env("S3_BUCKET", "tless")
                .env("S3_HOST", "minio")
                .env("S3_PASSWORD", "minio123")
                .env("S3_PORT", "9000")
                .env("S3_USER", "minio")
                .arg(mapper_id.to_string())
                .arg(s3_file)
                .stdout(Stdio::inherit())
                .stderr(Stdio::inherit())
                .output()
                .expect("tless(driver): failed executing command");

            "mapper"
        }
        "mapper" => {
            println!("tless(driver): executing 'reducer' from 'mapper': {event}");

            let fan_out_scale: i64 = get_json_from_event(&event)
                .get("scale-factor")
                .and_then(Value::as_i64)
                .expect("tless(driver): error: cannot find 'scale-factor' in CE");

            // Increment an atomic counter, and only execute the reducer
            // function when all fan-in functions have executed
            let mut count = INVOCATION_COUNTER.lock().unwrap();
            *count += 1;
            println!("tless(driver): counted {}/{}", *count, fan_out_scale);

            if *count == fan_out_scale {
                println!("tless(driver): done!");

                // Execute the function only after enough POST requests have
                // been received
                Command::new(format!("{}/word-count_reducer", BINARY_DIR))
                    .current_dir(BINARY_DIR)
                    .env("LD_LIBRARY_PATH", "/usr/local/lib")
                    .env("S3_BUCKET", "tless")
                    .env("S3_HOST", "minio")
                    .env("S3_PASSWORD", "minio123")
                    .env("S3_PORT", "9000")
                    .env("S3_USER", "minio")
                    .arg("word-count/outputs/mapper-")
                    .stdout(Stdio::inherit())
                    .stderr(Stdio::inherit())
                    .output()
                    .expect("tless(reducer): failed executing command");

                // Reset counter for next (warm) execution
                println!("tless(reducer): resetting counter to 0");
                *count = 0;
            }

            "reducer"
        }
        _ => panic!(
            "cloudevent: error: unrecognised source: {:}",
            event.source()
        ),
    });

    // -----
    // Post-process
    // -----

    match event.source().as_str() {
        // Process the output of the 'splitter' function and chain to 'mapper'
        "splitter" => {
            // Read the output file to work-out the scale-out pattern and the
            // files to chain-to
            let mut lines: Vec<String> = Vec::new();
            let file = fs::OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .open(format!("{}/{}", BINARY_DIR, "output_splitter.txt"))
                .unwrap();
            let reader = BufReader::new(file);

            for line in reader.lines() {
                lines.push(line.unwrap());
            }

            // Store the destinattion channel
            let dst = event.ty();

            let mut scaled_event = event.clone();

            // Write the new destination channel for the 'mapper' function
            scaled_event.set_type("http://mapper-to-reducer-kn-channel.tless.svc.cluster.local");

            println!("cloudevent(s1): fanning out by a factor of {}", lines.len());

            // JobSink executes one event per different CloudEvent id. So,
            // to make sure we can re-run the whole workflow without
            // re-deploying it, we generate random event ids
            for i in 1..lines.len() {
                scaled_event.set_id(Uuid::new_v4().to_string());
                scaled_event.set_data(
                    "aplication/json",
                    json!({"scale-factor": lines.len(), "input-file": lines[i], "mapper-id": i}),
                );

                println!(
                    "cloudevent(s1): posting to {dst} event {i}/{}: {scaled_event}",
                    lines.len()
                );
                post_event(dst.to_string(), scaled_event.clone());
            }

            // Return the last event through the HTTP respnse
            scaled_event.set_id(Uuid::new_v4().to_string());
            scaled_event.set_data(
                "aplication/json",
                json!({"scale-factor": lines.len(), "input-file": lines[0], "mapper-id": 0}),
            );
            scaled_event
        }
        // Process the output of the 'mapper' function and chain to 'reducer'
        "mapper" => {
            // We still need to POST the event manually but we need to do
            // it outside this method to be able to await on it (this method,
            // itself, is being await-ed on when called in a server loop)

            event
        }
        // Process the output of the 'redcuer' function
        "reducer" => {
            // Nothing to do after "reducer" as it is the last step in the chain

            event
        }
        _ => panic!(
            "cloudevent: error: unrecognised destination: {:}",
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

            let processed_event = process_event(event);

            // After executing step-two, we just need to post a clone of the
            // event to the type (i.e. destination) provided in it. Given that
            // step-two runs in a JobSink, the pod will terminate on exit, so
            // we need to make sure that the POST is sent before we move on
            println!(
                "cloudevent(s2): posting to {} event: {processed_event}",
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
