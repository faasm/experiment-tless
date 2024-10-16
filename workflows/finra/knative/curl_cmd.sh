#!/bin/bash

${COCO_SOURCE:-/usr/local}/bin/kubectl run curl --image=curlimages/curl --rm=true --restart=Never -ti -- -X POST -v \
   -H "content-type: application/json"  \
   -H "ce-specversion: 1.0" \
   -H "ce-source: cli" \
   -H "ce-type: http://splitter-to-mapper-kn-channel.tless.svc.cluster.local" \
   -H "ce-id: 1" \
   -d '{"foo":"bar"}' \
   http://ingress-to-splitter-kn-channel.tless.svc.cluster.local

# curl -X POST -v \
#    -H "content-type: application/json"  \
#    -H "ce-specversion: 1.0" \
#    -H "ce-source: splitter" \
#    -H "ce-type: http://splitter-to-mapper.tless.svc.cluster.local" \
#    -H "ce-id: 1" \
#    -d '{"foo":"bar", "input-file": "foo-bar", "mapper-id": 4}' \
#    http://localhost:8080
