## Word Count

Workflow based on the MapReduce [example](https://github.com/ddps-lab/serverless-faas-workbench/tree/master/aws/cpu-memory/mapreduce) part of the FunctionBench paper.

![workflow diagram](./serverless_workflows_word_count.png)

Assuming you have deployed one of the baselines of choice, you can run a execute
run of the workflow following these steps.

## Run the Workflow (Faasm)

To run the workflow, you must first upload the wikipedia dump to S3:

```bash
# Clean bucket first
faasmctl s3.clear-bucket --bucket ${BUCKET_NAME}

# Upload all data files in the directory
faasmctl s3.upload-dir \
  --bucket ${BUCKET_NAME} \
  --host-path ${PROJ_DIR}/datasets/word-count/few-files/ \
  --s3-path word-count/few-files
```

Second, upload the WASM files for each stage in the workflow:

```bash
faasmctl upload.workflow \
  word-count \
  faasm.azurecr.io/tless-experiments:$(cat ${PROJ_DIR}/VERSION):/usr/local/faasm/wasm/word-count
```

Lastly, you may invoke the driver function to trigger workflow execution:

```bash
faasmctl invoke word-count driver --cmdline "word-count/few-files"
```

> [!WARNING]
> To use with Faasm, you must make sure that we have STDOUT_CAPTURE disabled

## Run the Workflow (Knative)

First, deploy the workflow to the k8s cluster with bare-metal access to SEV nodes:

```bash
export RUNTIME_CLASS_NAME=kata-qemu-sev
export TLESS_VERSION=$(cat ${PROJ_ROOT}/VERSION)

kubectl apply -f ${PROJ_ROOT}/workflows/k8s_common.yaml
envsubst < ${PROJ_ROOT}/workflows/word-count/knative/workflow.yaml | kubectl apply -f -
```

To run the workflow, you must first upload the wikipedia dump to S3:

```bash
export MINIO_URL=$(kubectl -n tless get services -o jsonpath='{.items[?(@.metadata.name=="minio")].spec.clusterIP}')

# Clean bucket first
invrs s3 clear-bucket --bucket-name ${BUCKET_NAME}

# Upload all data files in the directory
invrs s3 upload-dir \
  --bucket-name ${BUCKET_NAME} \
  --host-path ${PROJ_DIR}/datasets/word-count/few-files/ \
  --s3-path word-count/few-files
```

then you may execute the workflow by running:

```bash
${PROJ_ROOT}/workflows/word-count/knative/curl_cmd.sh
```

## Stages Explained

0. Driver: orchestrates function execution (needed in Faasm, not in Knative)
1. Splitter: takes as an input an S3 path. Chains to one `mapper` function per
  key (i.e. file) in the S3 path.
2. Mapper: takes as an input an S3 path, and as an output writes to S3 a
  serialised map of the appearences of different programming languages in the
  wikipedia dump.
3. Reducer: once all mapper functions are done, iterates over the results S3
  dir and accumulates all results.
