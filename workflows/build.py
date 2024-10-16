from faasmtools.compile_util import wasm_cmake, wasm_copy_upload
from os import makedirs
from os.path import dirname, exists, join, realpath
from subprocess import run
from sys import argv

WORKFLOWS_ROOT = dirname(realpath(__file__))

WORKFLOWS = {
    # "finra": ["driver", "fetch-public", "fetch-private", "audit", "merge"],
    # "ml-training": ["driver", "partition", "pca", "rf", "validation"],
    # "ml-inference": ["driver", "partition", "load", "predict"],
    "word-count": ["driver", "splitter", "mapper", "reducer"],
}


def _copy_built_function(build_dir, wflow, func):
    exe_name = "{}_{}.{}".format(wflow, func, "wasm")
    src_file = join(build_dir, wflow, exe_name)
    wasm_copy_upload(wflow, func, src_file)


def compile(wasm=False, native=False, debug=False):
    """
    Compile a function to test a sample library
    """
    build_dir = join(WORKFLOWS_ROOT, "build-wasm" if wasm else "build-native")

    if not exists(build_dir):
        makedirs(build_dir)

    for wflow in WORKFLOWS:
        for function in WORKFLOWS[wflow]:
            # Build the function (gets written to the build dir)
            if wasm:
                wasm_cmake(
                    WORKFLOWS_ROOT,
                    build_dir,
                    "{}_{}".format(wflow, function),
                    clean=False,
                    debug=False,
                    is_threads=False,
                )

                # Copy into place in /usr/local/faasm/wasm/<user>/<func>
                _copy_built_function(build_dir, wflow, function)

            if native:
                cmake_cmd = [
                    "cmake",
                    "-GNinja",
                    "-DCMAKE_BUILD_TYPE={}".format("Debug" if debug else "Release"),
                    "-DCMAKE_C_COMPILER=/usr/bin/clang-17",
                    "-DCMAKE_CXX_COMPILER=/usr/bin/clang++-17",
                    WORKFLOWS_ROOT,
                ]
                cmake_cmd = " ".join(cmake_cmd)

                run(cmake_cmd, shell=True, check=True, cwd=build_dir)
                run(f"ninja {wflow}_{function}", shell=True, check=True, cwd=build_dir)


def compile_driver(debug=False):
    """
    Compile the driver function to enable Knative chaining (written in Rust)
    """
    for workflow in list(WORKFLOWS.keys()):
        build_dir = join(WORKFLOWS_ROOT, workflow, "knative")
        cargo_cmd = "cargo build --{}".format("debug" if debug else "release")
        run(cargo_cmd, shell=True, check=True, cwd=build_dir)


if __name__ == "__main__":
    debug = False
    if len(argv) == 2 and argv[1] == "--debug":
        debug = True

    # First, build the workflows
    compile(wasm=True, debug=debug)
    compile(native=True, debug=debug)

    # Second, build the driver function for Knative
    compile_driver(debug=debug)
