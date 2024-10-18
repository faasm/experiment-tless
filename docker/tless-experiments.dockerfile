# We inherit from the examples repo because it is likely that we want to use
# off-the-shelve examples like tensorflow
FROM faasm.azurecr.io/examples-build:0.6.0_0.4.0

# Install rust
RUN rm -rf /root/.rustup \
    && curl --proto '=https' --tlsv1.3 https://sh.rustup.rs -sSf | sh -s -- -y

# Prepare repository structure
RUN rm -rf /code \
    && mkdir -p /code \
    && cd /code \
    # Checkout to examples repo to a specific commit
    && git clone https://github.com/faasm/examples /code/faasm-examples \
    && cd /code/faasm-examples \
    && git checkout eef1e60e96e5446d256cb6a12585ecdaa7617249 \
    && git submodule update --init -f cpp \
    && git clone https://github.com/faasm/experiment-tless /code/experiment-tless \
    && cp -r /code/experiment-tless/workflows /code/examples/

# Build specific libraries we need
RUN cd /code/faasm-examples/cpp \
    # Build specific CPP libs
    && ./bin/inv_wrapper.sh libfaasm --clean \
    && git submodule update --init ./third-party/zlib \
    && ./bin/inv_wrapper.sh zlib \
    && cd /code/faasm-examples \
    && git submodule update --init ./examples/opencv \
    && ./bin/inv_wrapper.sh opencv opencv --native

# Build workflow code (WASM for Faasm + Native for Knative)
ENV PATH=${PATH}:/root/.cargo/bin
RUN cd /code/examples \
    # Install faasmtools
    && ./bin/create_venv.sh \
    && source ./venv/bin/activate \
    && python3 ./workflows/build.py
