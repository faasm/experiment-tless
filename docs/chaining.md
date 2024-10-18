## TLess Chaining Protocol

TLess implements a TEE-agnostic chaining protocol. It has the following steps:

### Notes

#### CP-ABE Set-Up

Argue that we do not need Multi-Authority ABE[1] as our single authority is the
single person encrypting things, which is the user, which we trust.

[1] https://eprint.iacr.org/2010/351.pdf

The trusted set-up of the CP-ABE scheme is as follows:
1. User (trusted) generates Public Key and Master Key -> bundle
2. User encrypts each function according to the DAG
3. User encrypts the bundle from 1 and uploads it to S3.
3. User uploads encrypted funcs

All of this happens in `tlessctl` as part of `tlessctl dag upload`.

> `tlessctl dag upload` is, still, not fully functional.

#### Public Key Distribution

How do ECFs in TLess are bootstrapped with the right keys?

In both cases, secret injection: when we build the WASM payload (or the initrd
image) we embed a well-known public key. This happens, automatically, when a
user is uploading the function to the registry.

For example, when we need to validate a user's signature, we can add the
user's public key to the function code, and measure it.

The same goes for well-known keys/certs like those for the attestation
service.

### 0. Receive Request

As part of an execution request, we receive the following JSON:

```json
{
  "dag": Sign(H(DAG), User)
  "func":  "NameOf(F_N)"
  "cert_chain": Enc(H(F_0) || H(F_1) || H(F_N-1), TEE)
}
```

where `H(DAG)` is the `sha512` digest of the workflow DAG and is signed with
the user's private key. The workflow DAG is a YAML file with the following
format:

```yaml
funcs:
  - name: splitter
    scale: 1
    chains_to: mapper
  - name: mapper
    scale: N
    chains_to: reducer
  - name: reducer
    scale: 1
```

`NameOf(F_N)` corresponds to the name of the function as specified in the DAG.
Lastly, the `cert_chain` is a hash chain of the plain-text version of each
function body (hash of the WASM bytecode for Faasm or binary of the function
code for Knative respectively) signed to a shared TEE identity.

### 1. Acquire TEE Identity

The first step to execute the chaining request is to acquire the shared TEE
identity. To this extent, the TEE performs the remote attestation protocol
(from inside the TEE), establishes a TLS connection with the attestation
authority, and gets a certificate in return.

For SGX (TLess-Faasm), the attestation authority is Microsoft's Azure
Attestation Service. For SEV (Tless-Knative), the attestation authority is
the `trustee`.

This certificate we will call `TEE_cert` and, for all purposes, can be treated
as a string.

### 2. Validate User Signature

The second step is to validate the user signature in the `dag` field.

We store well-known public keys in an Azure Vault, so we fetch them from there.

### 3. Decrypt the Certificate Chain

After step 1, we can now generate the shared TEE identity by combining `TEE_cert`
and `H(DAG)`, using CP-ABE KeyGen:

```cpp
std::string key = tless::cpAbeKeyGen(teeCert + dagHash);
```

then we can use this key to decrypt the certificate chain:

```bash
std::string chain = tless::cpAbeDecrypt(encryptedChain, key);
```

### 4. Acquire ECF Identity

Once we have the certificate chain, we can acquire the ECF identity required
to decrypt our function's plain-text code:

```cpp
std::string ecfKey = tless::cpAbeKeyGen(chain);
```

### 5. Acquire ECF Token

To decrypt the function's plain-text code, we also need a single-use ECF
token for our specific function.

TODO IMPLEMENT ME

### 6. Fetch Function Code

Now we are ready to fetch/decrypt the function code, and decrypt it using CP-ABE:

TODO: fetch encrypted function code

```cpp
std::string funcCode = tless::cpAbeDecrypt(encryptedFuncCode, ecfKey);
```

### 7. Update the Certificate Chain for Downstream Calls

Lastly, we only need to generate the updated certificate chain and sign it
to the TEE identity:

```
std::string newChain = chain + H(funcCode);
std::string newEncryptedChain = tless::cpAbeEncrypt(newChain, key);
```
