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
    && git clone https://github.com/faasm/examples /code/examples \
    && cd /code/examples \
    && git checkout 5cdd79642fba2dcb5cab634d2c53a43dcddf885d \
    && git submodule update --init -f cpp \
    && git clone https://github.com/faasm/experiment-tless /code/experiment-tless \
    && cp -r /code/experiment-tless/workflows /code/examples/

# Build workflow code (WASM for Faasm + Native for Knative)
ENV PATH=${PATH}:/root/.cargo/bin
RUN cd /code/examples \
    # Install faasmtools
    && ./bin/create_venv.sh \
    && source ./venv/bin/activate \
    && python3 ./workflows/build.py
