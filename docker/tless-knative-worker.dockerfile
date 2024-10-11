# Ligthweight container image to use as worker runtime in CC-Knative
ARG TLESS_VERSION
FROM ghcr.io/coco-serverless/tless-experiments:${TLESS_VERSION:-d34d} AS build

FROM ubuntu:22.04
LABEL org.opencontainers.image.source=https://github.com/faasm/experiment-tless

# Explicitly copy each workflow separately to minimise space
COPY --from=build \
    /code/examples/workflows/word-count/knative/target/ \
    /code/examples/workflows/word-count/knative/target
COPY --from=build \
    /code/examples/workflows/build-native/word-count/ \
    /code/examples/workflows/build-native/word-count

# Copy libraries we need at runtime
COPY --from=build /usr/local/lib/libaws-cpp-sdk-s3.so /usr/local/lib/
COPY --from=build /usr/local/lib/libaws-cpp-sdk-core.so /usr/local/lib/
COPY --from=build /lib/x86_64-linux-gnu/ /lib/x86_64-linux-gnu/
