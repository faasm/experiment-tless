#!/bin/bash

THIS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]:-${(%):-%x}}" )" >/dev/null 2>&1 && pwd )"
PROJ_ROOT="${THIS_DIR}/.."

pushd ${PROJ_ROOT}>>/dev/null

# ----------------------------
# Environment vars
# ----------------------------

export VERSION=$(cat ${PROJ_ROOT}/VERSION)

docker run \
    --rm -it \
    --name tless-build \
    -v ${PROJ_ROOT}/workflows:/code/examples/workflows \
    -w /code/examples \
    ghcr.io/coco-serverless/tless-experiments:0.4.0 \
    bash

popd >> /dev/null
