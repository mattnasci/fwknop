/*
 * This code is designed to repeatedly call libfko functions multiple times
 * with and without calling fko_destroy().  This allows valgrind to verify
 * whether memory is properly handled between calls.  In addition, libfko
 * functions are called with bogus inputs in order to validate how well libfko
 * validates input arguments.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "fko.h"

#define ENABLE_GPG_TESTS 0
#define FCN_CALLS        5
#define F_INT            100
#define CTX_DESTROY      0
#define NO_CTX_DESTROY   1
#define NEW_CTX          0
#define NO_NEW_CTX       1
#define DO_PRINT         1
#define NO_PRINT         2
#define NO_DIGEST        0
#define DO_DIGEST        1
#define RAW_DIGEST       2
#define MAX_LINE_LEN     3000 /* really long for fuzzing tests */

/* We use HMAC and encryption keys that are as long as the max
 * sizes since we pass in bogus key lengths. That is, libfko
 * functions cannot be expected to handle key lengths that are
 * longer than the key buffers themselves.
*/
#define ENC_KEY         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* 32 bytes (RIJNDAEL_MAX_KEYSIZE) */
#define HMAC_KEY        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" /* 128 bytes (SHA512_BLOCK_LEN) */
#define STR_8BYTES      "12345678"
#define STR_16BYTES     "1234567890123456"
#define STR_64BYTES     "1234567890123456789012345678901234567890123456789012345678901234"
#define SPA_MSG1        "1.1.1.1,tcp/22"
#define SPA_MSG2        "123.123.123.123,tcp/22"
#define SPA_NAT_MSG     "1.2.3.4,1234"
#define SERVER_AUTH_MSG "passwd"
#define SDP_ID    99999

#define IS_EMPTY_LINE(x) ( \
    x == '#' || x == '\n' || x == '\r' || x == ';' || x == '\0' \
)

static void display_ctx(fko_ctx_t ctx);
static void test_loop(int new_ctx_flag, int destroy_ctx_flag, int disable_sdp);
static void test_loop_compounded(int disable_sdp);
#if FUZZING_INTERFACES
static void spa_encoded_msg_fuzzing(void);
#endif
static void ctx_update(fko_ctx_t *ctx, int new_ctx_flag,
        int destroy_ctx_flag, int print_flag);
static void spa_default_ctx(fko_ctx_t *ctx);

static void spa_func_int(fko_ctx_t *ctx, char *name,
        int (*spa_func)(fko_ctx_t ctx, const int modifier), int min, int max,
        int final_val, int new_ctx_flag, int destroy_ctx_flag);
static void spa_func_getset_int(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const int modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, int *val),
        int min, int max, int final_val, int new_ctx_flag, int destroy_ctx_flag);

static void spa_func_getset_uint32(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const uint32_t modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, uint32_t *val),
		uint32_t min, uint32_t max, uint32_t final_val, int new_ctx_flag, int destroy_ctx_flag);

static void spa_func_getset_uint16(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const uint16_t modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, uint16_t *val),
		uint16_t min, uint16_t max, uint16_t final_val, int new_ctx_flag, int destroy_ctx_flag);

static void spa_func_getset_short(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const short modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, short *val),
        int min, int max, int final_val, int digest_flag,
        int new_ctx_flag, int destroy_ctx_flag);

int spa_calls = 0;
int spa_compounded_calls = 0;

int main(int argc, char **argv) {

	int disable_sdp = 0;
	char *disabled = "1";

	if(argc > 1)
	{
		if(strncmp(argv[1], disabled, 1))
			disable_sdp = 1;

		printf("\n[.] Disable SDP setting: %s \n\n", argv[1]);
	}
	else
		printf("\n[.] FKO Wrapper main() did not receive required arg");

    test_loop(NO_NEW_CTX, NO_CTX_DESTROY, disable_sdp);
    test_loop(NEW_CTX, CTX_DESTROY, disable_sdp);
    test_loop(NEW_CTX, NO_CTX_DESTROY, disable_sdp);
    test_loop(NO_NEW_CTX, CTX_DESTROY, disable_sdp);

    printf("\n[+] Total libfko function calls (before compounded tests): %d\n\n",
            spa_calls);

    printf("[+] Running compounded tests via: test_loop_compounded()...\n");
    test_loop_compounded(disable_sdp);

    printf("\n[+] Total compounded function calls: %d\n", spa_compounded_calls);
    printf("[+] Total libfko function calls (after compounded tests): %d\n\n",
            spa_calls);

#if FUZZING_INTERFACES
    printf("[+] libfko fuzzing by setting SPA buffer manually...\n");
    spa_encoded_msg_fuzzing();
#endif

    return 0;
}

#if FUZZING_INTERFACES
static void
spa_encoded_msg_fuzzing(void)
{
    fko_ctx_t      decode_ctx = NULL, decrypt_ctx = NULL;
    int            res = 0, pkt_id, require_success, require_digest, digest_type, msg_len;
    int            line_ctr = 0, spa_payload_ctr = 0;
    FILE          *fz  = NULL;
    char           line[MAX_LINE_LEN] = {0};
    char          *spa_data_final = NULL, *spa_data_copy = NULL;
    char           b64_encoded_msg[MAX_LINE_LEN] = {0};
    unsigned char  b64_decoded_msg[MAX_LINE_LEN] = {0};

    /* fuzzing file contents are formatted like this (and are generated by the spa_fuzzing.py
     * python fuzzer):
     *
     * <pkt_ID> <status: success|fail> <digest: yes|no> <digest type> <base64_SPA_payload>
    */

    if ((fz = fopen("fuzz_spa_payloads", "r")) == NULL)
        return;

    while ((fgets(line, MAX_LINE_LEN, fz)) != NULL)
    {
        line_ctr++;
        line[MAX_LINE_LEN-1] = '\0';

        if (line[strlen(line)-1] == '\n')
            line[strlen(line)-1] = '\0';

        if(IS_EMPTY_LINE(line[0]))
            continue;

        if(sscanf(line, "%d %d %d %d %s", &pkt_id, &require_success,
                    &require_digest, &digest_type, b64_encoded_msg) != 5)
        {
            printf("[+] fuzzing parsing error at line: %d\n", line_ctr);
            continue;
        }

        msg_len = fko_base64_decode(b64_encoded_msg, b64_decoded_msg);

        spa_payload_ctr++;

        fko_new(&decode_ctx);

        if ((res = fko_set_encoded_data(decode_ctx, (char *) b64_decoded_msg,
                        msg_len, require_digest, digest_type)) != FKO_SUCCESS) {
            printf("[-] pkt_id: %d, fko_set_encoded_data(): %s\n", pkt_id, fko_errstr(res));
        }

        res = fko_decode_spa_data(decode_ctx);
        if (require_success) {
            if (res != FKO_SUCCESS) {
                printf("[-] pkt_id: %d, expected decode success but: fko_decode_spa_data(): %s\n",
                        pkt_id, fko_errstr(res));
            }
        } else {
            if (res == FKO_SUCCESS) {
                printf("[-] pkt_id: %d, expected decode failure but: fko_decode_spa_data(): %s\n",
                        pkt_id, fko_errstr(res));
            }
        }

        if (0) {
                fko_destroy(decode_ctx);
                decode_ctx = NULL;
        }

        fko_set_spa_hmac_type(decode_ctx, FKO_HMAC_SHA256);

        res = fko_spa_data_final(decode_ctx, ENC_KEY, strlen(ENC_KEY),
                    HMAC_KEY, strlen(HMAC_KEY));

        if (res == FKO_SUCCESS) {
            if ((res = fko_get_spa_data(decode_ctx, &spa_data_final)) != FKO_SUCCESS) {
                printf("fko_get_spa_data(): %s\n", fko_errstr(res));
            } else {
                printf("PKT_ID: %d, PKT: %s\n", pkt_id, spa_data_final);

                spa_data_copy = strdup(spa_data_final);

                if (spa_data_final != NULL)
                    memset(spa_data_final, 0x0, strlen(spa_data_final));
                fko_destroy(decode_ctx);
                decode_ctx = NULL;

                res = fko_new_with_data(&decrypt_ctx, spa_data_copy, ENC_KEY,
                    strlen(ENC_KEY), FKO_ENC_MODE_CBC, HMAC_KEY, strlen(HMAC_KEY),
                    FKO_HMAC_SHA256);

                if (spa_data_copy != NULL)
                {
                    memset(spa_data_copy, 0x0, strlen(spa_data_copy));
                    free(spa_data_copy);
                }

                if (res == FKO_SUCCESS) {
                    printf("pkt_id: %d Success\n", pkt_id);
                } else {
                    printf("fko_new_with_data(): %s\n", fko_errstr(res));
                }

                fko_destroy(decrypt_ctx);
                decrypt_ctx = NULL;
            }
        } else {
            printf("fko_spa_data_final(): %s\n", fko_errstr(res));
            fko_destroy(decode_ctx);
            decode_ctx = NULL;
        }

        memset(line, 0x0, MAX_LINE_LEN);
        memset(b64_encoded_msg, 0x0, MAX_LINE_LEN);
        memset(b64_decoded_msg, 0x0, MAX_LINE_LEN);
    }

    fclose(fz);

    printf("[+] Sent %d SPA payloads through libfko encode/decode cycle...\n",
            spa_payload_ctr);
    return;
}
#endif

static void ctx_set_null_vals(fko_ctx_t *ctx)
{
    fko_set_rand_value(*ctx, NULL);
    fko_set_username(*ctx, NULL);
    fko_set_spa_message(*ctx, NULL);
    fko_set_spa_nat_access(*ctx, NULL);
    fko_set_spa_server_auth(*ctx, NULL);
    fko_set_spa_data(*ctx, NULL);
    fko_set_timestamp(*ctx, 0);
    fko_set_spa_message_type(*ctx, 0);
    fko_set_spa_message(*ctx, NULL);
    fko_set_spa_nat_access(*ctx, NULL);
    fko_set_spa_server_auth(*ctx, NULL);
    fko_set_spa_client_timeout(*ctx, 0);
    fko_set_spa_digest_type(*ctx, 0);
    fko_set_spa_digest(*ctx);
    fko_set_spa_hmac_type(*ctx, 0);
    fko_set_spa_encryption_mode(*ctx, 0);
    fko_set_spa_data(*ctx, NULL);
    fko_set_spa_hmac(*ctx, NULL, 0);
    fko_set_raw_spa_digest_type(*ctx, 0);
    fko_set_raw_spa_digest(*ctx);

    spa_calls += 20;

    return;
}

static void ctx_add_dupe_data(fko_ctx_t *ctx)
{
    fko_set_rand_value(*ctx, STR_8BYTES);
    fko_set_rand_value(*ctx, STR_16BYTES);
    fko_set_rand_value(*ctx, STR_16BYTES);
    fko_set_username(*ctx, STR_64BYTES);
    fko_set_username(*ctx, STR_64BYTES);
    fko_set_timestamp(*ctx, 12345);
    fko_set_timestamp(*ctx, 12345);
    fko_set_spa_message_type(*ctx, FKO_ACCESS_MSG);
    fko_set_spa_message_type(*ctx, FKO_ACCESS_MSG);
    fko_set_spa_message(*ctx, SPA_MSG1);
    fko_set_spa_message(*ctx, SPA_MSG1);
    fko_set_spa_nat_access(*ctx, SPA_NAT_MSG);
    fko_set_spa_nat_access(*ctx, SPA_NAT_MSG);
    fko_set_spa_server_auth(*ctx, SERVER_AUTH_MSG);
    fko_set_spa_server_auth(*ctx, SERVER_AUTH_MSG);
    fko_set_spa_client_timeout(*ctx, 30);
    fko_set_spa_client_timeout(*ctx, 30);
    fko_set_spa_digest_type(*ctx, FKO_DEFAULT_DIGEST);
    fko_set_spa_digest_type(*ctx, FKO_DEFAULT_DIGEST);
    fko_set_spa_hmac_type(*ctx, FKO_DEFAULT_HMAC_MODE);
    fko_set_spa_hmac_type(*ctx, FKO_DEFAULT_HMAC_MODE);
    fko_set_spa_encryption_mode(*ctx, FKO_ENC_MODE_CBC);
    fko_set_spa_encryption_mode(*ctx, FKO_ENC_MODE_CBC);
    fko_set_spa_data(*ctx, STR_8BYTES);
    fko_set_spa_data(*ctx, STR_64BYTES);

    spa_calls += 24;

    return;
}


static void
test_loop_compounded(int disable_sdp)
{
    fko_ctx_t  ctx = NULL, decrypt_ctx = NULL;
    char *spa_data = NULL;
    int i, j, k, l, res;
    uint32_t sdp_id = SDP_ID;

    if(disable_sdp)
    	sdp_id = 0;

    for (i=0; i<FCN_CALLS; i++) {

        fko_new(&ctx);

        for (j=-1; j<FKO_LAST_MSG_TYPE+1; j++) {

        	res = fko_set_disable_sdp_mode(ctx, disable_sdp);
            if (res != FKO_SUCCESS)
                printf("fko_set_disable_sdp_mode(): %s\n", fko_errstr(res));

        	res = fko_set_sdp_id(ctx, sdp_id);
            if (res != FKO_SUCCESS)
                printf("fko_set_sdp_id(): %s\n", fko_errstr(res));

            res = fko_set_spa_message_type(ctx, j);
            if (res != FKO_SUCCESS)
                printf("fko_set_spa_message_type(): %s\n", fko_errstr(res));

            res = fko_set_spa_client_timeout(ctx, i);
            if (res != FKO_SUCCESS)
                printf("fko_set_spa_client_timeout(): %s\n", fko_errstr(res));

            res = fko_set_timestamp(ctx, 100);
            if (res != FKO_SUCCESS)
                printf("fko_set_timestamp(): %s\n", fko_errstr(res));

            fko_set_spa_message(ctx, SPA_MSG1);
            res = fko_set_spa_message(ctx, SPA_MSG2);
            if (res != FKO_SUCCESS)
                printf("fko_set_spa_message(): %s\n", fko_errstr(res));

            res = fko_set_spa_nat_access(ctx, SPA_NAT_MSG);
            if (res != FKO_SUCCESS)
                printf("fko_set_spa_nat_access(): %s\n", fko_errstr(res));

            res = fko_set_username(ctx, "someuser");
            if (res != FKO_SUCCESS)
                printf("fko_set_username(): %s\n", fko_errstr(res));

            res = fko_set_spa_server_auth(ctx, SERVER_AUTH_MSG);
            if (res != FKO_SUCCESS)
                printf("fko_set_spa_server_auth(): %s\n", fko_errstr(res));

            res = fko_set_spa_hmac_type(ctx, FKO_HMAC_SHA256);
            if (res != FKO_SUCCESS)
                printf("fko_set_spa_hmac_type(): %s\n", fko_errstr(res));

            for (k=-4; k<=16; k+=4) {
                for (l=-4; l<=16; l+=4) {

                    res = fko_spa_data_final(ctx, ENC_KEY, k, HMAC_KEY, l);
                    if (res == FKO_SUCCESS) {
                        res = fko_get_spa_data(ctx, &spa_data);
                        if (res == FKO_SUCCESS) {

                            res = fko_new_with_data(&decrypt_ctx, spa_data, NULL,
                                0, FKO_ENC_MODE_CBC, HMAC_KEY, l, FKO_HMAC_SHA256, sdp_id);

                            if (res == FKO_SUCCESS) {
                                res = fko_decrypt_spa_data(decrypt_ctx, ENC_KEY, k);
                                if (res != FKO_SUCCESS)
                                    printf("fko_decrypt_spa_data(): %s\n", fko_errstr(res));

                                fko_destroy(decrypt_ctx);
                                decrypt_ctx = NULL;
                                spa_calls += 13;
                                spa_compounded_calls += 13;

                            } else {
                                printf("fko_new_with_data(): %s\n", fko_errstr(res));
                            }
                        } else {
                            printf("fko_get_spa_data(): %s\n", fko_errstr(res));
                        }
                    } else {
                        printf("fko_spa_data_final(): %s\n", fko_errstr(res));
                    }
                }
            }
        }
        fko_destroy(ctx);
        ctx = NULL;

        spa_calls += 3;
        spa_compounded_calls += 3;
    }
}

static void
test_loop(int new_ctx_flag, int destroy_ctx_flag, int disable_sdp)
{
    fko_ctx_t  ctx = NULL, decrypt_ctx = NULL;
    int        i, j;
    char       *spa_data = NULL, encode_buf[100], decode_buf[100];
    uint32_t   sdp_id = SDP_ID;

    printf("[+] test_loop(): %s, %s\n",
            new_ctx_flag == NEW_CTX ? "NEW_CTX" : "NO_NEW_CTX",
            destroy_ctx_flag == CTX_DESTROY ? "DESTROY_CTX" : "NO_DESTROY_CTX");
    printf("fko_new(): %s\n", fko_errstr(fko_new(&ctx)));
    fko_destroy(ctx);
    ctx = NULL;
    printf("fko_new(): %s\n", fko_errstr(fko_new(&ctx)));

    spa_func_getset_int(&ctx, "fko_set_spa_client_timeout",
            &fko_set_spa_client_timeout, "fko_get_spa_client_timeout",
            &fko_get_spa_client_timeout, -F_INT, F_INT, 10,
            new_ctx_flag, destroy_ctx_flag);

    spa_func_getset_short(&ctx, "fko_set_spa_message_type",
            &fko_set_spa_message_type, "fko_get_spa_message_type",
            &fko_get_spa_message_type, FKO_COMMAND_MSG-F_INT,
            FKO_LAST_MSG_TYPE+F_INT, FKO_ACCESS_MSG,
            NO_DIGEST, new_ctx_flag, destroy_ctx_flag);

    spa_func_int(&ctx, "fko_set_timestamp",
            &fko_set_timestamp, -F_INT, F_INT, 10,
            new_ctx_flag, destroy_ctx_flag);

    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_set_spa_message(1.1.1.1,tcp/22): %s\n",
                fko_errstr(fko_set_spa_message(ctx, "1.1.1.1,tcp/22")));
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_set_spa_nat_access(1.2.3.4,1234): %s\n",
                fko_errstr(fko_set_spa_nat_access(ctx, "1.2.3.4,1234")));
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_set_username(someuser): %s\n",
                fko_errstr(fko_set_username(ctx, "someuser")));
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    spa_func_getset_short(&ctx, "fko_set_spa_encryption_type",
            &fko_set_spa_encryption_type, "fko_get_spa_encryption_type",
            &fko_get_spa_encryption_type, FKO_ENCRYPTION_INVALID_DATA-F_INT,
            FKO_LAST_ENCRYPTION_TYPE+F_INT, FKO_ENCRYPTION_RIJNDAEL,
            NO_DIGEST, new_ctx_flag, destroy_ctx_flag);

    spa_func_getset_int(&ctx, "fko_set_spa_encryption_mode",
            &fko_set_spa_encryption_mode, "fko_get_spa_encryption_mode",
            &fko_get_spa_encryption_mode, FKO_ENC_MODE_UNKNOWN-F_INT,
            FKO_LAST_ENC_MODE+F_INT, FKO_ENC_MODE_CBC,
            new_ctx_flag, destroy_ctx_flag);

    if (ENABLE_GPG_TESTS) {
        for (i=0; i<FCN_CALLS; i++) {
            printf("fko_set_spa_encryption_type(FKO_ENCRYPTION_GPG): %s\n",
                    fko_errstr(fko_set_spa_encryption_type(ctx, FKO_ENCRYPTION_GPG)));
            ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        }

        for (i=0; i<FCN_CALLS; i++) {
            printf("fko_set_gpg_home_dir(/home/mbr/.gnupg): %s\n",
                    fko_errstr(fko_set_gpg_home_dir(ctx, "/home/mbr/.gnupg")));
            ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        }

        for (i=0; i<FCN_CALLS; i++) {
            printf("fko_set_gpg_recipient(1234asdf): %s\n",
                fko_errstr(fko_set_gpg_recipient(ctx, "1234asdf")));
            ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        }
    }

    spa_func_getset_short(&ctx, "fko_set_spa_digest_type",
            &fko_set_spa_digest_type, "fko_get_spa_digest_type",
            &fko_get_spa_digest_type, FKO_DIGEST_INVALID_DATA-F_INT,
            FKO_LAST_DIGEST_TYPE+F_INT, FKO_DEFAULT_DIGEST,
            DO_DIGEST, new_ctx_flag, destroy_ctx_flag);

    spa_func_getset_short(&ctx, "fko_set_raw_spa_digest_type",
            &fko_set_spa_digest_type, "fko_get_raw_spa_digest_type",
            &fko_get_spa_digest_type, FKO_DIGEST_INVALID_DATA-F_INT,
            FKO_LAST_DIGEST_TYPE+F_INT, FKO_DEFAULT_DIGEST,
            RAW_DIGEST, new_ctx_flag, destroy_ctx_flag);

    spa_func_getset_short(&ctx, "fko_set_spa_hmac_type",
            &fko_set_spa_hmac_type, "fko_get_spa_hmac_type",
            &fko_get_spa_hmac_type, FKO_HMAC_INVALID_DATA-F_INT,
            FKO_LAST_HMAC_MODE+F_INT, FKO_HMAC_SHA256,
            NO_DIGEST, new_ctx_flag, destroy_ctx_flag);

    if(disable_sdp)
    {
    	sdp_id = 0;

        spa_func_getset_uint16(&ctx, "fko_set_disable_sdp_mode",
                &fko_set_disable_sdp_mode, "fko_get_spa_hmac_type",
                &fko_get_disable_sdp_mode, 0, 1, (uint16_t)disable_sdp,
				new_ctx_flag, destroy_ctx_flag);
    }
    else
    {
        spa_func_getset_uint32(&ctx, "fko_set_sdp_id",
                &fko_set_sdp_id, "fko_get_sdp_id",
                &fko_get_sdp_id, 0, UINT32_MAX, sdp_id,
				new_ctx_flag, destroy_ctx_flag);
    }

    printf("Trying encrypt / authenticate step with bogus key lengths...\n");
    for (i=-100; i < 200; i += 10) {
        for (j=-100; j < 200; j += 10) {
            fko_spa_data_final(ctx, ENC_KEY, i, HMAC_KEY, j);
            fko_spa_data_final(ctx, NULL, i, HMAC_KEY, j);
            fko_spa_data_final(ctx, ENC_KEY, i, NULL, j);
            fko_spa_data_final(ctx, NULL, i, NULL, j);
            ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, NO_PRINT);
            spa_calls += 4;
        }
    }

    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_spa_data_final(ENC_KEY, 16, HMAC_KEY, 16): %s\n",
                fko_errstr(fko_spa_data_final(ctx, ENC_KEY, 16, HMAC_KEY, 16)));
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_get_spa_data(): %s\n",
                fko_errstr(fko_get_spa_data(ctx, &spa_data)));
        printf("    SPA DATA: %s\n", spa_data == NULL ? "<NULL>" : spa_data);
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    printf("fko_new_with_data(): %s (data: %s)\n",
        fko_errstr(fko_new_with_data(&decrypt_ctx, spa_data, NULL,
        0, FKO_ENC_MODE_CBC, NULL, 0, FKO_HMAC_SHA256, sdp_id)), spa_data);

    /* verify hmac, decrypt, and display ctx all together*/
    /* this piece cannot be done in SDP mode
     * once the SDP Client ID is removed to do decryption, a second run
     * of the HMAC verification would fail
     */
    if(disable_sdp)
    {
		for (i=0; i<FCN_CALLS; i++) {
			display_ctx(decrypt_ctx);
			printf("fko_verify_hmac() (1): %s\n",
				fko_errstr(fko_verify_hmac(decrypt_ctx, HMAC_KEY, 16)));

			printf("fko_decrypt_spa_data() (1): %s\n",
				fko_errstr(fko_decrypt_spa_data(decrypt_ctx, ENC_KEY, 16)));

			ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
		}
    }

    /* now, separately verify hmac, decrypt, and display ctx */
    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_verify_hmac() (2): %s\n",
            fko_errstr(fko_verify_hmac(decrypt_ctx, HMAC_KEY, 16)));
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    if(!disable_sdp)
    {
		/* now remove the sdp client id in order to decrypt */
		printf("fko_strip_sdp_id() (1): %s\n",
			fko_errstr(fko_strip_sdp_id(decrypt_ctx)));
    }

    /* now decrypt */
    for (i=0; i<FCN_CALLS; i++) {
        printf("fko_decrypt_spa_data() (2): %s\n",
            fko_errstr(fko_decrypt_spa_data(decrypt_ctx, ENC_KEY, 16)));
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    for (i=0; i<FCN_CALLS; i++) {
        display_ctx(decrypt_ctx);
        ctx_update(&ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
    }

    /* NULL tests
    */
    ctx_set_null_vals(&ctx);
    ctx_set_null_vals(&ctx);

    /* Now add data for code coverage - note data values
     * are added twice in this function to exercise ctx
     * member free() code.
    */
    ctx_add_dupe_data(&ctx);

    /* set data values back to NULL
    */
    ctx_set_null_vals(&ctx);
    ctx_set_null_vals(&ctx);

    for (i=0; i<FCN_CALLS; i++) {
        fko_destroy(ctx);
        ctx = NULL;
    }

    for (i=0; i<FCN_CALLS; i++) {
        fko_destroy(decrypt_ctx);
        decrypt_ctx = NULL;
    }

    /* exercise the base64 encode/decode wrapper
    */
    fko_base64_encode((unsigned char *)ENC_KEY, encode_buf, 16);
    fko_base64_decode(encode_buf, (unsigned char *)decode_buf);

    /* call fko_errstr() across valid and invalid values
    */
    for (i=-5; i < FKO_LAST_ERROR+5; i++) {
        printf("libfko error (%d): %s\n", i, fko_errstr(i));
        spa_calls++;
    }

    printf("fko_new_with_data(): %s (data: %s)\n",
        fko_errstr(fko_new_with_data(&decrypt_ctx, "tooshort", ENC_KEY,
        strlen(ENC_KEY), FKO_ENC_MODE_CBC, HMAC_KEY, strlen(HMAC_KEY),
        FKO_HMAC_SHA256, sdp_id)), "tooshort");

    return;
}

static void ctx_update(fko_ctx_t *ctx, int new_ctx_flag,
        int destroy_ctx_flag, int print_flag)
{
    if (destroy_ctx_flag == CTX_DESTROY) {
        if (print_flag == DO_PRINT)
            printf("fko_destroy(): %s\n", fko_errstr(fko_destroy(*ctx)));
        else
            fko_destroy(*ctx);
        spa_calls++;
        *ctx = NULL;
    }
    if (new_ctx_flag == NEW_CTX) {
        /* always destroy before re-creating */
        if (print_flag == DO_PRINT)
            printf("fko_destroy(): %s\n", fko_errstr(fko_destroy(*ctx)));
        else
            fko_destroy(*ctx);
        *ctx = NULL;

        if (print_flag == DO_PRINT)
            printf("fko_new(): %s\n", fko_errstr(fko_new(ctx)));
        else
            fko_new(ctx);
        spa_calls += 2;
    }
    return;
}

static void spa_default_ctx(fko_ctx_t *ctx)
{
    fko_new(ctx);
    fko_set_rand_value(*ctx, NULL);
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_spa_message(*ctx, "123.123.123.123,tcp/22");
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_spa_message_type(*ctx, FKO_ACCESS_MSG);
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_username(*ctx, "someuser");
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_spa_encryption_type(*ctx, FKO_ENCRYPTION_RIJNDAEL);
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_spa_encryption_mode(*ctx, FKO_ENC_MODE_CBC);
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_spa_digest_type(*ctx, FKO_DEFAULT_DIGEST);
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_set_spa_hmac_type(*ctx, FKO_HMAC_SHA256);
    fko_spa_data_final(*ctx, ENC_KEY, 16, HMAC_KEY, 16);
    // display_ctx(*ctx);

    spa_calls += 16;
    return;
}

static void spa_func_getset_int(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const int modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, int *val),
        int min, int max, int final_val, int new_ctx_flag, int destroy_ctx_flag)
{
    fko_ctx_t default_ctx = NULL;
    int get_val;
    int i, res;

    spa_default_ctx(&default_ctx);

    printf("[+] calling libfko get/set: %s/%s\n", get_name, set_name);
    for (i=min; i <= max; i++) {
        get_val = 1234;  /* meaningless default */
        printf("%s(%d): %s\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        printf("%s(%d): %s (DUPE)\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        res = (spa_get)(*ctx, &get_val);
        printf("%s(%d): %s\n", get_name, get_val, fko_errstr(res));

        ctx_update(ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        spa_calls += 3;

        /* also set on a fully populated context */
        (spa_set)(default_ctx, i);
    }
    printf("%s(%d): %s (FINAL)\n", set_name, final_val,
            fko_errstr((spa_set)(*ctx, final_val)));
    display_ctx(*ctx);

    fko_spa_data_final(default_ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_destroy(default_ctx);
    default_ctx = NULL;

    return;
}

static void spa_func_getset_uint32(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const uint32_t modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, uint32_t *val),
		uint32_t min, uint32_t max, uint32_t final_val, int new_ctx_flag, int destroy_ctx_flag)
{
    fko_ctx_t default_ctx = NULL;
    uint32_t get_val;
    uint32_t i;
    int res;

    spa_default_ctx(&default_ctx);

    printf("[+] calling libfko get/set: %s/%s\n", get_name, set_name);
    for (i=min; i <= max; i++) {
        get_val = 1234;  /* meaningless default */
        printf("%s(%"PRIu32"): %s\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        printf("%s(%"PRIu32"): %s (DUPE)\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        res = (spa_get)(*ctx, &get_val);
        printf("%s(%"PRIu32"): %s\n", get_name, get_val, fko_errstr(res));

        ctx_update(ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        spa_calls += 3;

        /* also set on a fully populated context */
        (spa_set)(default_ctx, i);
    }
    printf("%s(%"PRIu32"): %s (FINAL)\n", set_name, final_val,
            fko_errstr((spa_set)(*ctx, final_val)));
    display_ctx(*ctx);

    fko_spa_data_final(default_ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_destroy(default_ctx);
    default_ctx = NULL;

    return;
}

static void spa_func_getset_uint16(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const uint16_t modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, uint16_t *val),
		uint16_t min, uint16_t max, uint16_t final_val, int new_ctx_flag, int destroy_ctx_flag)
{
    fko_ctx_t default_ctx = NULL;
    uint16_t get_val;
    uint16_t i;
    int res;

    spa_default_ctx(&default_ctx);

    printf("[+] calling libfko get/set: %s/%s\n", get_name, set_name);
    for (i=min; i <= max; i++) {
        get_val = 1234;  /* meaningless default */
        printf("%s(%"PRIu16"): %s\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        printf("%s(%"PRIu16"): %s (DUPE)\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        res = (spa_get)(*ctx, &get_val);
        printf("%s(%"PRIu16"): %s\n", get_name, get_val, fko_errstr(res));

        ctx_update(ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        spa_calls += 3;

        /* also set on a fully populated context */
        (spa_set)(default_ctx, i);
    }
    printf("%s(%"PRIu16"): %s (FINAL)\n", set_name, final_val,
            fko_errstr((spa_set)(*ctx, final_val)));
    display_ctx(*ctx);

    fko_spa_data_final(default_ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_destroy(default_ctx);
    default_ctx = NULL;

    return;
}

static void spa_func_int(fko_ctx_t *ctx, char *name,
        int (*spa_set)(fko_ctx_t ctx, const int modifier), int min, int max,
        int final_val, int new_ctx_flag, int destroy_ctx_flag)
{
    fko_ctx_t default_ctx = NULL;
    int i;

    spa_default_ctx(&default_ctx);

    printf("[+] calling libfko function: %s\n", name);
    for (i=min; i <= max; i++) {
        printf("%s(%d): %s\n", name, i, fko_errstr((spa_set)(*ctx, i)));
        printf("%s(%d): %s (DUPE)\n", name, i, fko_errstr((spa_set)(*ctx, i)));

        ctx_update(ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        spa_calls += 2;

        /* also set on a fully populated context */
        (spa_set)(default_ctx, i);
    }
    printf("%s(%d): %s (FINAL)\n", name, final_val,
            fko_errstr((spa_set)(*ctx, final_val)));
    display_ctx(*ctx);

    fko_spa_data_final(default_ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_destroy(default_ctx);
    default_ctx = NULL;

    return;
}

static void spa_func_getset_short(fko_ctx_t *ctx, char *set_name,
        int (*spa_set)(fko_ctx_t ctx, const short modifier),
        char *get_name, int (*spa_get)(fko_ctx_t ctx, short *val),
        int min, int max, int final_val, int digest_flag,
        int new_ctx_flag, int destroy_ctx_flag)
{
    fko_ctx_t default_ctx = NULL;
    short get_val;
    int i, res;

    spa_default_ctx(&default_ctx);

    printf("[+] calling libfko get/set: %s/%s\n", get_name, set_name);
    for (i=min; i <= max; i++) {
        get_val = 1234;  /* meaningless default */
        printf("%s(%d): %s\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));
        printf("%s(%d): %s (DUPE)\n", set_name, i, fko_errstr((spa_set)(*ctx, i)));

        if (digest_flag == DO_DIGEST)
            fko_set_spa_digest(*ctx);
        else if (digest_flag == RAW_DIGEST)
            fko_set_raw_spa_digest(*ctx);

        res = (spa_get)(*ctx, &get_val);
        printf("%s(%d): %s\n", get_name, get_val, fko_errstr(res));

        ctx_update(ctx, new_ctx_flag, destroy_ctx_flag, DO_PRINT);
        if (digest_flag == NO_DIGEST)
            spa_calls += 3;
        else
            spa_calls += 4;

        /* also set on a fully populated context */
        (spa_set)(default_ctx, i);
    }
    printf("%s(%d): %s (FINAL)\n", set_name, final_val,
            fko_errstr((spa_set)(*ctx, final_val)));

    display_ctx(*ctx);

    fko_spa_data_final(default_ctx, ENC_KEY, 16, HMAC_KEY, 16);
    fko_destroy(default_ctx);
    default_ctx = NULL;

    return;
}

/* Show the fields of the FKO context.
*/
static void
display_ctx(fko_ctx_t ctx)
{
    char       *rand_val        = NULL;
    char       *username        = NULL;
    char       *version         = NULL;
    char       *spa_message     = NULL;
    char       *nat_access      = NULL;
    char       *server_auth     = NULL;
    char       *enc_data        = NULL;
    char       *hmac_data       = NULL;
    char       *spa_digest      = NULL;
    char       *spa_data        = NULL;

    time_t      timestamp       = 0;
    short       msg_type        = -1;
    short       digest_type     = -1;
    short       hmac_type       = -1;
    int         encryption_mode = -1;
    int         client_timeout  = -1;

    /* pass in NULL to each fko_get_* function first to ensure
     * that NULL is handled properly
    */
    fko_get_rand_value(ctx, NULL);
    fko_get_rand_value(ctx, &rand_val);
    fko_get_username(ctx, NULL);
    fko_get_username(ctx, &username);
    fko_get_timestamp(ctx, NULL);
    fko_get_timestamp(ctx, &timestamp);
    fko_get_version(ctx, NULL);
    fko_get_version(ctx, &version);
    fko_get_spa_message_type(ctx, NULL);
    fko_get_spa_message_type(ctx, &msg_type);
    fko_get_spa_message(ctx, NULL);
    fko_get_spa_message(ctx, &spa_message);
    fko_get_spa_nat_access(ctx, NULL);
    fko_get_spa_nat_access(ctx, &nat_access);
    fko_get_spa_server_auth(ctx, NULL);
    fko_get_spa_server_auth(ctx, &server_auth);
    fko_get_spa_client_timeout(ctx, NULL);
    fko_get_spa_client_timeout(ctx, &client_timeout);
    fko_get_spa_digest_type(ctx, NULL);
    fko_get_spa_digest_type(ctx, &digest_type);
    fko_get_spa_hmac_type(ctx, NULL);
    fko_get_spa_hmac_type(ctx, &hmac_type);
    fko_get_spa_encryption_mode(ctx, NULL);
    fko_get_spa_encryption_mode(ctx, &encryption_mode);
    fko_get_encoded_data(ctx, NULL);
    fko_get_encoded_data(ctx, &enc_data);
    fko_get_spa_hmac(ctx, NULL);
    fko_get_spa_hmac(ctx, &hmac_data);
    fko_get_spa_digest(ctx, NULL);
    fko_get_spa_digest(ctx, &spa_digest);
    fko_get_spa_data(ctx, NULL);
    fko_get_spa_data(ctx, &spa_data);

    printf("\nFKO Field Values:\n=================\n\n");
    printf("   Random Value: %s\n", rand_val == NULL ? "<NULL>" : rand_val);
    printf("       Username: %s\n", username == NULL ? "<NULL>" : username);
    printf("      Timestamp: %u\n", (unsigned int) timestamp);
    printf("    FKO Version: %s\n", version == NULL ? "<NULL>" : version);
    printf("   Message Type: %i\n", msg_type);
    printf(" Message String: %s\n", spa_message == NULL ? "<NULL>" : spa_message);
    printf("     Nat Access: %s\n", nat_access == NULL ? "<NULL>" : nat_access);
    printf("    Server Auth: %s\n", server_auth == NULL ? "<NULL>" : server_auth);
    printf(" Client Timeout: %d\n", client_timeout);
    printf("    Digest Type: %d\n", digest_type);
    printf("      HMAC Type: %d\n", hmac_type);
    printf("Encryption Mode: %d\n", encryption_mode);
    printf("   Encoded Data: %s\n", enc_data == NULL ? "<NULL>" : enc_data);
    printf("SPA Data Digest: %s\n", spa_digest == NULL ? "<NULL>" : spa_digest);
    printf("           HMAC: %s\n", hmac_data == NULL ? "<NULL>" : hmac_data);
    printf(" Final SPA Data: %s\n", spa_data);

    spa_calls += 31;

    return;
}
