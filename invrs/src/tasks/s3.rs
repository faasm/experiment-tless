use crate::env::Env;
use bytes::Bytes;
use chrono::{DateTime, Utc};
use futures_util::StreamExt;
use log::{debug, error, info, warn};
use minio::s3::args::*;
use minio::s3::builders::ObjectContent;
use minio::s3::client::{Client, ClientBuilder};
use minio::s3::creds::StaticProvider;
use minio::s3::error::Error;
use minio::s3::http::BaseUrl;
use minio::s3::types::{S3Api, ToStream};
use std::path::{Path, PathBuf};
use std::{
    env, fs,
    io::{Read, Write},
    thread, time,
};

#[derive(Debug)]
pub struct S3 {}

impl S3 {
    fn init_s3_client() -> Client {
        let minio_url: &str = match env::var("MINIO_URL") {
            Ok(value) => &value.clone(),
            Err(env::VarError::NotPresent) => "localhost",
            Err(e) => panic!("invrs(s3): failed to read env. var: {}", e),
        };
        let minio_port: &str = match env::var("MINIO_PORT") {
            Ok(value) => &value.clone(),
            Err(env::VarError::NotPresent) => "9000",
            Err(e) => panic!("invrs(s3): failed to read env. var: {}", e),
        };

        let base_url = format!("http://{minio_url}:{minio_port}")
            .parse::<BaseUrl>()
            .unwrap();

        let static_provider = StaticProvider::new("minio", "minio123", None);

        ClientBuilder::new(base_url.clone())
            .provider(Some(Box::new(static_provider)))
            .build()
            .unwrap()
    }

    pub fn get_datasets_root() -> PathBuf {
        let mut path = env::current_dir().expect("invrs: failed to get current directory");
        path.push("datasets");
        path
    }

    pub async fn clear_bucket(bucket_name: String) {
        debug!("invrs(s3): removing s3 bucket: {bucket_name}");

        // First, remove all objects in the bucket
        let client = Self::init_s3_client();

        // Return fast if the bucket does not exist
        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            debug!("invrs(s3): skipping non-existant bucket: {bucket_name}");
            return;
        }

        let mut objects = client
            .list_objects(&bucket_name)
            .recursive(true)
            .to_stream()
            .await;

        while let Some(result) = objects.next().await {
            match result {
                Ok(resp) => {
                    for item in resp.contents {
                        client
                            .remove_object(&bucket_name, item.name.as_str())
                            .send()
                            .await
                            .unwrap();
                    }
                }
                Err(e) => error!("invrs(s3): error: {:?}", e),
            }
        }

        client
            .remove_bucket(&RemoveBucketArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();
    }

    pub async fn clear_dir(bucket_name: String, prefix: String) {
        debug!("invrs(s3): clearing s3 dir: {bucket_name}/{prefix}");

        // First, remove all objects in the bucket
        let client = Self::init_s3_client();

        // Return fast if the bucket does not exist
        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            debug!("invrs(s3): warning: bucket does not exist: {bucket_name}");
            return;
        }

        let mut objects = client
            .list_objects(&bucket_name)
            .recursive(true)
            .prefix(Some(prefix))
            .to_stream()
            .await;

        while let Some(result) = objects.next().await {
            match result {
                Ok(resp) => {
                    for item in resp.contents {
                        client
                            .remove_object(&bucket_name, item.name.as_str())
                            .send()
                            .await
                            .unwrap();
                    }
                }
                Err(e) => error!("invrs(s3): error: {:?}", e),
            }
        }
    }

    pub async fn clear_object(bucket_name: &str, path: &str) {
        debug!("invrs(s3): clearing s3 key: {bucket_name}/{path}");
        Self::init_s3_client()
            .remove_object(&bucket_name, path)
            .send()
            .await
            .unwrap();
    }

    pub async fn get_dir(bucket_name: &str, s3_path: &str, host_path: &str) {
        let client = Self::init_s3_client();

        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            warn!("tlessctl(s3): warning: bucket does not exist: {bucket_name}");
            return;
        }

        let host_path_rs = Path::new(host_path);
        if !host_path_rs.exists() {
            fs::create_dir_all(host_path_rs).unwrap();
        }

        let mut objects = Self::init_s3_client()
            .list_objects(&bucket_name)
            .recursive(true)
            .prefix(Some(s3_path.to_string()))
            .to_stream()
            .await;

        while let Some(result) = objects.next().await {
            match result {
                Ok(resp) => {
                    for item in resp.contents {
                        let host_file_name = item.name.rsplit('/').next().unwrap_or(&item.name);

                        let (mut object, _) = client
                            .get_object(bucket_name, &item.name)
                            .send()
                            .await
                            .unwrap()
                            .content
                            .to_stream()
                            .await
                            .unwrap();

                        let mut content = Vec::new();
                        while let Some(chunk) = object.next().await {
                            let chunk = chunk.expect("Failed to read chunk");
                            content.extend_from_slice(&chunk);
                        }

                        let host_file_path = format!("{host_path}/{host_file_name}");
                        println!("tlessctl(s3): serializing {s3_path} to {host_path}");

                        let mut file = fs::File::create(Path::new(&host_file_path)).unwrap();
                        file.write_all(&content).unwrap();
                    }
                }
                Err(e) => error!("invrs(s3): error: {:?}", e),
            }
        }
    }

    pub async fn get_key(bucket_name: &str, key_name: &str) -> String {
        let client = Self::init_s3_client();

        // Return fast if the bucket does not exist
        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            warn!("invrs(s3): warning: bucket does not exist: {bucket_name}");
            return "".to_string();
        }

        // Loop until the object appears, and return its last modified date
        let (mut object, _) = client
            .get_object(bucket_name, key_name)
            .send()
            .await
            .unwrap()
            .content
            .to_stream()
            .await
            .unwrap();

        let mut content = Vec::new();
        while let Some(chunk) = object.next().await {
            let chunk = chunk.expect("Failed to read chunk");
            content.extend_from_slice(&chunk);
        }

        String::from_utf8(content).expect("tlessctl(s3): error converting object to string")
    }

    /// Wait for a key to be ready, and return when it was last modified
    pub async fn wait_for_key(bucket_name: &str, key_name: &str) -> Option<DateTime<Utc>> {
        let client = Self::init_s3_client();

        // Return fast if the bucket does not exist
        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            debug!("invrs(s3): warning: bucket does not exist: {bucket_name}");
            return None;
        }

        // Loop until the object appears, and return its last modified date
        loop {
            let mut objects = client
                .list_objects(&bucket_name)
                .recursive(true)
                .prefix(Some(key_name.to_string()))
                .to_stream()
                .await;

            while let Some(result) = objects.next().await {
                match result {
                    Ok(resp) => {
                        for item in resp.contents {
                            debug!(
                                "item: {} (last: {})",
                                item.name,
                                item.last_modified.unwrap()
                            );
                            return item.last_modified;
                        }
                    }
                    Err(e) => match e {
                        Error::S3Error(s3_error) => match s3_error.code.as_str() {
                            _ => panic!("invrs(s3): error: {}", s3_error.message),
                        },
                        _ => panic!("invrs(s3): error: {}", e),
                    },
                }
            }

            debug!("invrs(s3): waiting for key ({key_name})...");
            thread::sleep(time::Duration::from_secs(2));
        }
    }

    pub async fn list_buckets() {
        let buckets = Self::init_s3_client().list_buckets().send().await.unwrap();

        info!(
            "invrs(s3): found a total of {} buckets",
            buckets.buckets.len()
        );
        for bucket in &buckets.buckets {
            info!("- {}", bucket.name);
        }
    }

    pub async fn list_keys(bucket_name: String, prefix: &Option<String>) {
        debug!(
            "{}(s3): listing keys in bucket {bucket_name}",
            Env::SYS_NAME
        );

        let mut objects = Self::init_s3_client()
            .list_objects(&bucket_name)
            .recursive(true)
            .prefix(prefix.clone())
            .to_stream()
            .await;

        while let Some(result) = objects.next().await {
            match result {
                Ok(resp) => {
                    for item in resp.contents {
                        info!("- {:?}", item.name);
                    }
                }
                Err(e) => error!("invrs(s3): error: {:?}", e),
            }
        }
    }

    pub async fn upload_bytes(bucket_name: &str, s3_path: &str, bytes: &[u8]) {
        debug!(
            "tlessctl(s3): uploading {} bytes to {bucket_name}/{s3_path}",
            bytes.len()
        );

        let client = Self::init_s3_client();

        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            client
                .make_bucket(&MakeBucketArgs::new(&bucket_name).unwrap())
                .await
                .unwrap();
        }

        let content = ObjectContent::from(Bytes::copy_from_slice(bytes));
        client
            .put_object_content(&bucket_name, &s3_path, content)
            .send()
            .await
            .unwrap();
    }

    pub async fn upload_dir(bucket_name: String, host_path: String, s3_path: String) {
        debug!("invrs(s3): uploading {host_path} to {bucket_name}/{s3_path}");

        let client = Self::init_s3_client();

        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            client
                .make_bucket(&MakeBucketArgs::new(&bucket_name).unwrap())
                .await
                .unwrap();
        }

        // Iterate over the host directory and upload each file therein
        let path = Path::new(&host_path);
        for entry in fs::read_dir(path).unwrap() {
            let host_file_path: &Path = &entry.unwrap().path();
            let content = ObjectContent::from(host_file_path);
            let s3_file_path = format!(
                "{}/{}",
                s3_path,
                host_file_path.file_name().expect("").to_string_lossy()
            );

            client
                .put_object_content(&bucket_name, &s3_file_path, content)
                .send()
                .await
                .unwrap();
        }
    }

    pub async fn upload_file(bucket_name: &str, host_path: &str, s3_path: &str) {
        debug!("invrs(s3): uploading {host_path} to {s3_path}");

        let client = Self::init_s3_client();

        let exists: bool = client
            .bucket_exists(&BucketExistsArgs::new(&bucket_name).unwrap())
            .await
            .unwrap();

        if !exists {
            client
                .make_bucket(&MakeBucketArgs::new(&bucket_name).unwrap())
                .await
                .unwrap();
        }

        // Load file to byte array
        let mut file = fs::File::open(host_path).unwrap();
        let mut file_contents = Vec::new();
        file.read_to_end(&mut file_contents).unwrap();

        // Upload it to S3
        let content = ObjectContent::from(file_contents);
        client
            .put_object_content(&bucket_name, &s3_path, content)
            .send()
            .await
            .unwrap();
    }
}
