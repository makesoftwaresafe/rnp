/*
 * Copyright (c) 2018-2022, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include "crypto/signatures.h"
#include "librepgp/stream-packet.h"
#include "librepgp/stream-sig.h"
#include "utils.h"
#include "sec_profile.hpp"

/**
 * @brief Add signature fields to the hash context and finish it.
 * @param hash initialized hash context fed with signed data (document, key, etc).
 *             It is finalized in this function.
 * @param sig populated or loaded signature
 * @param hbuf buffer to store the resulting hash. Must be large enough for hash output.
 * @param hlen on success will be filled with the hash size, otherwise zeroed
 * @return RNP_SUCCESS on success or some error otherwise
 */
static void
signature_hash_finish(const pgp_signature_t &sig, rnp::Hash &hash, uint8_t *hbuf, size_t &hlen)
{
    hash.add(sig.hashed_data, sig.hashed_len);
    if (sig.version > PGP_V3) {
        uint8_t trailer[6] = {0x04, 0xff, 0x00, 0x00, 0x00, 0x00};
        STORE32BE(&trailer[2], sig.hashed_len);
        hash.add(trailer, 6);
    }
    hlen = hash.finish(hbuf);
}

std::unique_ptr<rnp::Hash>
signature_init(const pgp_key_material_t &key, pgp_hash_alg_t hash_alg)
{
    auto hash = rnp::Hash::create(hash_alg);
    if (key.alg == PGP_PKA_SM2) {
#if defined(ENABLE_SM2)
        rnp_result_t r = sm2_compute_za(key.ec, *hash);
        if (r != RNP_SUCCESS) {
            RNP_LOG("failed to compute SM2 ZA field");
            throw rnp::rnp_exception(r);
        }
#else
        RNP_LOG("SM2 ZA computation not available");
        throw rnp::rnp_exception(RNP_ERROR_NOT_IMPLEMENTED);
#endif
    }
    return hash;
}

void
signature_calculate(pgp_signature_t &     sig,
                    pgp_key_material_t &  seckey,
                    rnp::Hash &           hash,
                    rnp::SecurityContext &ctx)
{
    uint8_t              hval[PGP_MAX_HASH_SIZE];
    size_t               hlen = 0;
    rnp_result_t         ret = RNP_ERROR_GENERIC;
    const pgp_hash_alg_t hash_alg = hash.alg();

    /* Finalize hash first, since function is required to do this */
    try {
        signature_hash_finish(sig, hash, hval, hlen);
    } catch (const std::exception &e) {
        RNP_LOG("Failed to finalize hash: %s", e.what());
        throw;
    }

    if (!seckey.secret) {
        RNP_LOG("Secret key is required.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    if (sig.palg != seckey.alg) {
        RNP_LOG("Signature and secret key do not agree on algorithm type.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    /* Validate key material if didn't before */
    seckey.validate(ctx, false);
    if (!seckey.valid()) {
        RNP_LOG("Attempt to sign with invalid key material.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    /* copy left 16 bits to signature */
    memcpy(sig.lbits, hval, 2);

    /* sign */
    pgp_signature_material_t material = {};
    switch (sig.palg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY:
        ret = rsa_sign_pkcs1(&ctx.rng, &material.rsa, sig.halg, hval, hlen, &seckey.rsa);
        if (ret) {
            RNP_LOG("rsa signing failed");
        }
        break;
    case PGP_PKA_EDDSA:
        ret = eddsa_sign(&ctx.rng, &material.ecc, hval, hlen, &seckey.ec);
        if (ret) {
            RNP_LOG("eddsa signing failed");
        }
        break;
    case PGP_PKA_DSA:
        ret = dsa_sign(&ctx.rng, &material.dsa, hval, hlen, &seckey.dsa);
        if (ret != RNP_SUCCESS) {
            RNP_LOG("DSA signing failed");
        }
        break;
    /*
     * ECDH is signed with ECDSA. This must be changed when ECDH will support
     * X25519, but I need to check how it should be done exactly.
     */
    case PGP_PKA_ECDH:
    case PGP_PKA_ECDSA:
    case PGP_PKA_SM2: {
        const ec_curve_desc_t *curve = get_curve_desc(seckey.ec.curve);
        if (!curve) {
            RNP_LOG("Unknown curve");
            ret = RNP_ERROR_BAD_PARAMETERS;
            break;
        }
        if (!curve_supported(seckey.ec.curve)) {
            RNP_LOG("EC sign: curve %s is not supported.", curve->pgp_name);
            ret = RNP_ERROR_NOT_SUPPORTED;
            break;
        }
        /* "-2" because ECDSA on P-521 must work with SHA-512 digest */
        if (BITS_TO_BYTES(curve->bitlen) - 2 > hlen) {
            RNP_LOG("Message hash too small");
            ret = RNP_ERROR_BAD_PARAMETERS;
            break;
        }

        if (sig.palg == PGP_PKA_SM2) {
#if defined(ENABLE_SM2)
            ret = sm2_sign(&ctx.rng, &material.ecc, hash_alg, hval, hlen, &seckey.ec);
            if (ret) {
                RNP_LOG("SM2 signing failed");
            }
#else
            RNP_LOG("SM2 signing is not available.");
            ret = RNP_ERROR_NOT_IMPLEMENTED;
#endif
            break;
        }

        ret = ecdsa_sign(&ctx.rng, &material.ecc, hash_alg, hval, hlen, &seckey.ec);
        if (ret) {
            RNP_LOG("ECDSA signing failed");
        }
        break;
    }
    default:
        RNP_LOG("Unsupported algorithm %d", sig.palg);
        break;
    }
    if (ret) {
        throw rnp::rnp_exception(ret);
    }
    try {
        sig.write_material(material);
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        throw;
    }
}

rnp_result_t
signature_validate(const pgp_signature_t &     sig,
                   const pgp_key_material_t &  key,
                   rnp::Hash &                 hash,
                   const rnp::SecurityContext &ctx)
{
    if (sig.palg != key.alg) {
        RNP_LOG("Signature and key do not agree on algorithm type: %d vs %d",
                (int) sig.palg,
                (int) key.alg);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* Check signature security */
    if (ctx.profile.hash_level(sig.halg, sig.creation()) < rnp::SecurityLevel::Default) {
        RNP_LOG("Insecure hash algorithm %d, marking signature as invalid.", sig.halg);
        return RNP_ERROR_SIGNATURE_INVALID;
    }

    /* Finalize hash */
    uint8_t hval[PGP_MAX_HASH_SIZE];
    size_t  hlen = 0;
    try {
        signature_hash_finish(sig, hash, hval, hlen);
    } catch (const std::exception &e) {
        RNP_LOG("Failed to finalize signature hash.");
        return RNP_ERROR_GENERIC;
    }

    /* compare lbits */
    if (memcmp(hval, sig.lbits, 2)) {
        RNP_LOG("wrong lbits");
        return RNP_ERROR_SIGNATURE_INVALID;
    }

    /* validate signature */
    pgp_signature_material_t material = {};
    try {
        sig.parse_material(material);
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    rnp_result_t ret = RNP_ERROR_GENERIC;
    switch (sig.palg) {
    case PGP_PKA_DSA:
        ret = dsa_verify(&material.dsa, hval, hlen, &key.dsa);
        break;
    case PGP_PKA_EDDSA:
        ret = eddsa_verify(&material.ecc, hval, hlen, &key.ec);
        break;
    case PGP_PKA_SM2:
#if defined(ENABLE_SM2)
        ret = sm2_verify(&material.ecc, hash.alg(), hval, hlen, &key.ec);
#else
        RNP_LOG("SM2 verification is not available.");
        ret = RNP_ERROR_NOT_IMPLEMENTED;
#endif
        break;
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_SIGN_ONLY:
        ret = rsa_verify_pkcs1(&material.rsa, sig.halg, hval, hlen, &key.rsa);
        break;
    case PGP_PKA_RSA_ENCRYPT_ONLY:
        RNP_LOG("RSA encrypt-only signature considered as invalid.");
        ret = RNP_ERROR_SIGNATURE_INVALID;
        break;
    case PGP_PKA_ECDSA:
        if (!curve_supported(key.ec.curve)) {
            RNP_LOG("ECDSA verify: curve %d is not supported.", (int) key.ec.curve);
            ret = RNP_ERROR_NOT_SUPPORTED;
            break;
        }
        ret = ecdsa_verify(&material.ecc, hash.alg(), hval, hlen, &key.ec);
        break;
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN:
        RNP_LOG("ElGamal are considered as invalid.");
        ret = RNP_ERROR_SIGNATURE_INVALID;
        break;
    default:
        RNP_LOG("Unknown algorithm");
        ret = RNP_ERROR_BAD_PARAMETERS;
    }
    return ret;
}
