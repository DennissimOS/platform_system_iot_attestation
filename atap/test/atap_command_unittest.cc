/*
 * Copyright 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include <fstream>

#include <gtest/gtest.h>

#include <base/files/file_util.h>

#include <libatap/libatap.h>

#include "atap_unittest_util.h"
#include "fake_atap_ops.h"

namespace atap {

uint8_t session_key[ATAP_ECDH_KEY_LEN];

// Subclass BaseAtapToolTest to check for memory leaks.
class CommandTest : public BaseAtapTest {
 public:
  CommandTest() {}

  FakeAtapOps ops_;

  void validate_ca_request(const uint8_t* buf,
                           uint32_t buf_size,
                           AtapOperation operation);
  void compute_session_key(const uint8_t device_public_key[ATAP_ECDH_KEY_LEN]);
};

void CommandTest::compute_session_key(
    const uint8_t device_pubkey[ATAP_ECDH_KEY_LEN]) {
  uint8_t shared_secret[ATAP_ECDH_KEY_LEN];
  uint8_t ca_pubkey[ATAP_ECDH_KEY_LEN];
  AtapResult ret = ops_.ecdh_shared_secret_compute(ops_.atap_ops(),
                                                   ATAP_CURVE_TYPE_X25519,
                                                   device_pubkey,
                                                   ca_pubkey,
                                                   shared_secret);
  ASSERT_EQ(ret, ATAP_RESULT_OK);
  ret = derive_session_key(ops_.atap_ops(),
                           device_pubkey,
                           ca_pubkey,
                           shared_secret,
                           "KEY",
                           session_key,
                           ATAP_AES_128_KEY_LEN);
  ASSERT_EQ(ret, ATAP_RESULT_OK);
}

void CommandTest::validate_ca_request(const uint8_t* buf,
                                      uint32_t buf_size,
                                      AtapOperation operation) {
  EXPECT_GT(buf_size, (uint32_t)ATAP_HEADER_LEN);
  uint32_t i = 4;
  uint32_t ca_request_size = *(uint32_t*)next(buf, &i, sizeof(uint32_t));
  EXPECT_EQ(buf_size - ATAP_HEADER_LEN, ca_request_size);
  uint8_t* device_pubkey = next(buf, &i, ATAP_ECDH_KEY_LEN);
  compute_session_key(device_pubkey);
  const uint8_t* iv = next(buf, &i, ATAP_GCM_IV_LEN);
  uint32_t ciphertext_len = *(uint32_t*)next(buf, &i, sizeof(uint32_t));
  EXPECT_EQ(ca_request_size - ATAP_ECDH_KEY_LEN - ATAP_GCM_IV_LEN -
                ATAP_GCM_TAG_LEN - sizeof(uint32_t),
            ciphertext_len);
  const uint8_t* ciphertext = next(buf, &i, ciphertext_len);
  uint8_t* inner = (uint8_t*)atap_malloc(ciphertext_len);
  const uint8_t* tag = next(buf, &i, ATAP_GCM_TAG_LEN);
  AtapResult ret = ops_.aes_gcm_128_decrypt(
      ops_.atap_ops(), ciphertext, ciphertext_len, iv, session_key, tag, inner);
  EXPECT_EQ(ATAP_RESULT_OK, ret);
  i = 4;
  uint32_t inner_ca_request_size =
      *(uint32_t*)next(inner, &i, sizeof(uint32_t));
  EXPECT_EQ(ciphertext_len - ATAP_HEADER_LEN, inner_ca_request_size);
  // Test operation is Issue, no authentication
  int32_t auth_cert_chain_size = *(int32_t*)next(inner, &i, sizeof(int32_t));
  EXPECT_EQ(0, auth_cert_chain_size);
  int32_t auth_signature_size = *(int32_t*)next(inner, &i, sizeof(int32_t));
  EXPECT_EQ(0, auth_signature_size);
  std::string product_id_hash_str;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(kProductIdHash),
                                     &product_id_hash_str));
  const uint8_t* product_id_hash = next(inner, &i, ATAP_SHA256_DIGEST_LEN);
  EXPECT_EQ(0,
            memcmp(product_id_hash,
                   (uint8_t*)&product_id_hash_str[0],
                   ATAP_SHA256_DIGEST_LEN));
  int32_t RSA_pubkey_len = *(int32_t*)next(inner, &i, sizeof(int32_t));
  EXPECT_EQ(0, RSA_pubkey_len);
  int32_t ECDSA_pubkey_len = *(int32_t*)next(inner, &i, sizeof(int32_t));
  EXPECT_EQ(0, ECDSA_pubkey_len);
  int32_t edDSA_pubkey_len = *(int32_t*)next(inner, &i, sizeof(int32_t));
  EXPECT_EQ(0, edDSA_pubkey_len);

  atap_free(inner);
}

TEST_F(CommandTest, GetCaRequestIssue) {
  std::string operation_start;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(kIssueOperationStartPath),
                                     &operation_start));
  uint32_t ca_request_size;
  uint8_t* ca_request;
  AtapResult res = atap_get_ca_request(ops_.atap_ops(),
                                       (uint8_t*)&operation_start[0],
                                       operation_start.size(),
                                       &ca_request,
                                       &ca_request_size);
  EXPECT_EQ(ATAP_RESULT_OK, res);

  std::string ca_ecdh_private_key;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(kCaEcdhPrivateKey),
                                     &ca_ecdh_private_key));

  validate_ca_request(ca_request, ca_request_size, ATAP_OPERATION_ISSUE);

  atap_free(ca_request);
}

TEST_F(CommandTest, SetCaResponseIssue) {
  std::string inner;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(kIssueInnerCaResponsePath),
                                     &inner));
  uint32_t ca_response_size = ATAP_HEADER_LEN + ATAP_GCM_IV_LEN +
                              sizeof(uint32_t) + inner.size() +
                              ATAP_GCM_TAG_LEN;
  uint8_t* ca_response = (uint8_t*)atap_malloc(ca_response_size);
  append_header_to_buf(ca_response, ca_response_size - ATAP_HEADER_LEN);
  uint32_t i = ATAP_HEADER_LEN;
  uint8_t* iv = next(ca_response, &i, ATAP_GCM_IV_LEN);
  ops_.get_random_bytes(ops_.atap_ops(), iv, ATAP_GCM_IV_LEN);
  uint32_t* ciphertext_len = (uint32_t*)next(ca_response, &i, sizeof(uint32_t));
  *ciphertext_len = inner.size();
  uint8_t* ciphertext = next(ca_response, &i, *ciphertext_len);
  uint8_t* tag = next(ca_response, &i, ATAP_GCM_TAG_LEN);
  AtapResult res = ops_.aes_gcm_128_encrypt(ops_.atap_ops(),
                                            (uint8_t*)&inner[0],
                                            inner.size(),
                                            iv,
                                            session_key,
                                            ciphertext,
                                            tag);
  ASSERT_EQ(ATAP_RESULT_OK, res);
  res = atap_set_ca_response(ops_.atap_ops(), ca_response, ca_response_size);
  EXPECT_EQ(ATAP_RESULT_OK, res);
  atap_free(ca_response);
}

}  // namespace atap