# We inherit from the examples repo because it is likely that we want to use
# off-the-shelve examples like tensorflow
FROM faasm.azurecr.io/examples-build:0.6.0_0.4.0

# Install rust
RUN curl --proto '=https' --tlsv1.3 https://sh.rustup.rs -sSf | sh -s -- -y

# Prepare repository structure
RUN rm -rf /code \
    && mkdir -p /code \
    && cd /code \
    # Checkout to examples repo to a specific commit
    && git clone https://github.com/faasm/examples /code/faasm-examples \
    && cd /code/faasm-examples \
    && git checkout 268a78df08955ceaf6307ba9b0a82bd2703b4ec7 \
    && git submodule update --init -f cpp \
    && git clone -b workflows-knative https://github.com/faasm/experiment-tless /code/experiment-tless \
    && cp -r /code/experiment-tless/workflows /code/faasm-examples/

# Build specific libraries we need
RUN cd /code/faasm-examples/cpp \
    # Build specific CPP libs
    && ./bin/inv_wrapper.sh libfaasm --clean \
    && git submodule update --init ./third-party/zlib \
    && ./bin/inv_wrapper.sh zlib \
    # Build specific examples (TODO: build native versions too)
    && cd /code/faasm-examples \
    && git submodule update --init ./examples/opencv \
    && ./bin/inv_wrapper.sh opencv opencv --native

# Build workflow code (WASM for Faasm + Native for Knative)
ENV PATH=${PATH}:/root/.cargo/bin
RUN cd /code/faasm-examples \
    # Install faasmtools
    && ./bin/create_venv.sh \
    && source ./venv/bin/activate \
    && python3 ./workflows/build.py

##########
# DELETE ALL THE REST
#########

# Temporary workaround to, increasingly, patch workloads. We must manually
# select the directories to overwrite, to minimize build times (which are,
# alas, often)
ARG TMP_VER=unknown
RUN cd /code/experiment-tless/ \
    && git pull origin workflows-knative \
    && rm -rf /code/faasm-examples/workflows/libs \
    && rm -rf /code/faasm-examples/workflows/word-count \
    && cp -r /code/experiment-tless/workflows/libs /code/faasm-examples/workflows/libs \
    && cp -r /code/experiment-tless/workflows/word-count /code/faasm-examples/workflows/word-count

# Build workflow code (WASM for Faasm + Native for Knative)
ENV PATH=${PATH}:/root/.cargo/bin
RUN cd /code/faasm-examples \
    && source ./bin/workon.sh \
    && source ./venv/bin/activate \
    && python3 ./workflows/build.py
