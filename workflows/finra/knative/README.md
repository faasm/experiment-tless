## CloudEvents Handler

Knative uses CloudEvents to handle function chaining. In this directory we
implement a handler function that, given a chaining plan, orchestrates the
function execution.

For each function, we need:
* The binary to execute: TLESS_FUNCTION_BINARY
