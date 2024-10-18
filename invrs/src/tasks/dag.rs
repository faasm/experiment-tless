use crate::tasks::s3::S3;
use aes_gcm::aead::{Aead, AeadCore, KeyInit, OsRng};
use aes_gcm::{Aes256Gcm, Key};
use rabe;

// FIXME(tless-prod): symmetric key is currently hardcoded. In production it
// would be given to the user upon registration
static DEMO_SYM_KEY: [u8; 32] = [
    0xf0, 0x0d, 0x48, 0x2e, 0xca, 0x21, 0xfb, 0x13, 0xec, 0xf0, 0x01, 0x48, 0xba, 0x60, 0x01, 0x76,
    0x6e, 0x56, 0xbb, 0xa5, 0xff, 0x9b, 0x11, 0x9d, 0xd6, 0xfa, 0x96, 0x39, 0x2b, 0x7c, 0x1a, 0x0d,
];

#[derive(Debug)]
pub struct Dag {}

impl Dag {
    // TODO: add path to dag
    pub async fn upload(wflow_name: &str) {
        // Generate encryption context to encrypt code and data
        let (pk, msk) = rabe::schemes::bsw::setup();
        let ctx = rabe::ffi::bsw::CpAbeContext {
            _msk: msk.clone(),
            _pk: pk.clone(),
        };

        // Generate a serialized view of the CpAbeContext. This needs to be
        // unsafe as it needs to represent the raw underlying memory, because
        // we will have to re-construct the struct in C++ code
        let serial_ctx = unsafe {
            std::slice::from_raw_parts(
                (&ctx as *const rabe::ffi::bsw::CpAbeContext) as *const u8,
                std::mem::size_of::<rabe::ffi::bsw::CpAbeContext>(),
            )
        };

        // Encrypt it with the shared symmetric key, so that any TEE can use
        // the CP-ABE encryption/decryption context
        let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(&DEMO_SYM_KEY));
        let ctx_nonce = Aes256Gcm::generate_nonce(&mut OsRng);
        let ctx_ct = cipher.encrypt(&ctx_nonce, serial_ctx).unwrap();
        let mut encrypted_ctx = ctx_nonce.to_vec();
        encrypted_ctx.extend_from_slice(&ctx_ct);

        // Serialize and upload context to S3
        S3::upload_bytes(
            "tless",
            &format!("{wflow_name}/crypto/cp-abe-ctx"),
            &encrypted_ctx,
        )
        .await;

        // Use the newly generated context to encrypt the certificate chain
        // FIXME(tless-prod): in a production deployment, these two values
        // should be kept secret
        let plain_text_origin_cert_chain = "G3N0SY5";
        let tee_identity_magic = "G4NU1N3_TL3SS_T33";

        // TODO: read from path,
        let dag_hex_digest = "dag";

        // Encrypt the certificate chain using the adequate policy
        let policy = format!("\"{}\" and \"{}\"", tee_identity_magic, dag_hex_digest);
        let ct = rabe::schemes::bsw::encrypt(
            &pk,
            &policy,
            rabe::utils::policy::pest::PolicyLanguage::HumanPolicy,
            &plain_text_origin_cert_chain.as_bytes(),
        )
        .unwrap();

        let abe_ct_str = match serde_json::to_string(&ct) {
            Ok(ct_str) => ct_str,
            Err(_) => panic!("tlessctl(dag): error encrypting certificate chain"),
        };

        // Encapsulate the cipher-text in a symmetric encryption payload
        let abe_ct_nonce = Aes256Gcm::generate_nonce(&mut OsRng);
        let abe_ct = cipher
            .encrypt(&abe_ct_nonce, abe_ct_str.as_bytes())
            .unwrap();
        let mut encrypted_abe_ct = abe_ct_nonce.to_vec();
        encrypted_abe_ct.extend_from_slice(&abe_ct);

        // Upload it
        S3::upload_bytes(
            "tless",
            &format!("{wflow_name}/inputs/splitter"),
            &encrypted_abe_ct,
        )
        .await;

        // TODO(encrypted-functions): to support encrypted functions, here we
        // would have to keep generating new policies, and encrypting each
        // function body with the new policy

        // DELETE ME just a test
        let tmp_nonce = Aes256Gcm::generate_nonce(&mut OsRng);
        let tmp_ct = cipher
            .encrypt(&tmp_nonce, "Hello world!".as_bytes())
            .unwrap();
        let mut encrypted_tmp = tmp_nonce.to_vec();
        encrypted_tmp.extend_from_slice(&tmp_ct);
        S3::upload_bytes("tless", &format!("{wflow_name}/hello"), &encrypted_tmp).await;
    }
}
