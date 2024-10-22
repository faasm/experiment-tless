use edag_verify_core::{CertificateChain, DagGraph, VerificationResult, VerifyApi};
use k256::{ecdsa::VerifyingKey, EncodedPoint};
use risc0_zkvm::guest::env;

/// The guest verifies that a message (i.e. a tuple of HW attestation evidence
/// and function name) has been signed by a public key. We commit, in the
/// journal, three different things:
/// The hash digest of the
fn main() {
    // Inputs:
    // 1. DAG
    // 2. CertificateChain
    // Shared Data:
    // 1. Keychain

    let (dag, cert_chain, encoded_verifying_key, num_chains, skip_verify): (DagGraph, CertificateChain, EncodedPoint, i32, bool) =
        env::read();
    let verifying_key = VerifyingKey::from_encoded_point(&encoded_verifying_key).unwrap();

    // 1. Calculate the SHA256 digest of the input DAG and commit it to the
    // journal
    let dag_digest = dag.sha256_hex();

    // 2. Verify that the signature in the certificate chain correspnds to
    // one of our well-known signing keys, and that the signed body corresponds
    // to the body of the certificate chain
    let valid_cert = cert_chain.verify_signature(&verifying_key);

    // 3. Lastly, validate that the certificate chain defines a valid path
    // in the input dag
    let dag_preserved = match valid_cert {
        true => VerifyApi::is_dag_preserved(&dag, &cert_chain),
        false => false,
    };

    // FIXME: for the sake of the micro-benchmark, we re-do the signature
    // verification and the pattern matching a number of times
    for _ in 1..num_chains {
        if !skip_verify {
            cert_chain.verify_signature(&verifying_key);
        }
        VerifyApi::is_dag_preserved(&dag, &cert_chain);
    }

    let result = VerificationResult::new(dag_digest, encoded_verifying_key, valid_cert, dag_preserved);

    // Commit to the journal the verifying key and message that was signed.
    env::commit(&result);
}
