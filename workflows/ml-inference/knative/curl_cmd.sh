#!/bin/bash

THIS_RUN_MAGIC=${RANDOM}
NUM_INF_FUNCS=8

${COCO_SOURCE:-/usr/local}/bin/kubectl run curl --image=curlimages/curl --rm=true --restart=Never -i -- -X POST -v \
   -H "content-type: application/json"  \
   -H "ce-specversion: 1.0" \
   -H "ce-source: cli-partition" \
   -H "ce-type: http://pre-inf-to-predict-kn-channel.tless.svc.cluster.local" \
   -H "ce-id: 1" \
   -d '{"model-dir": "ml-inference/model", "data-dir": "ml-inference/images-inference-1k", "num-inf-funcs": '"${NUM_INF_FUNCS}"', "run-magic": '"${THIS_RUN_MAGIC}}"'' \
   http://ingress-to-partition-kn-channel.tless.svc.cluster.local &

${COCO_SOURCE:-/usr/local}/bin/kubectl run curl2 --image=curlimages/curl --rm=true --restart=Never -i -- -X POST -v \
   -H "content-type: application/json"  \
   -H "ce-specversion: 1.0" \
   -H "ce-source: cli-load" \
   -H "ce-type: http://pre-inf-to-predict-kn-channel.tless.svc.cluster.local" \
   -H "ce-id: 1" \
   -d '{"model-dir": "ml-inference/model", "data-dir": "ml-inference/images-inference-1k", "num-inf-funcs": '"${NUM_INF_FUNCS}"', "run-magic": '"${THIS_RUN_MAGIC}}"'' \
   http://ingress-to-load-kn-channel.tless.svc.cluster.local

# curl -X POST -v \
#    -H "content-type: application/json"  \
#    -H "ce-specversion: 1.0" \
#    -H "ce-type: http://all-to-inference-kn-channel.tless.svc.cluster.local" \
#    -H "ce-id: 1" \
#    -H "ce-source: pre-inf" \
#    -d '{"model-dir": "ml-inference/model", "data-dir": "ml-inference/images-inference-1k", "num-inf-funcs": 12, "run-magic": '"${THIS_RUN_MAGIC}}"'' \
#    http://localhost:8080
