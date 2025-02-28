/*
 * rtp_decoder.c
 *
 * decoder structures and functions for SRTP pcap decoder
 *
 * Example:
 * $ wget --no-check-certificate \
 *     https://raw.githubusercontent.com/gteissier/srtp-decrypt/master/marseillaise-srtp.pcap
 * $ ./test/rtp_decoder -a -t 10 -e 128 -b \
 *     aSBrbm93IGFsbCB5b3VyIGxpdHRsZSBzZWNyZXRz \
 *         < ~/marseillaise-srtp.pcap \
 *         | text2pcap -t "%M:%S." -u 10000,10000 - - \
 *         > ./marseillaise-rtp.pcap
 *
 * There is also a different way of setting up key size and tag size
 * based upon RFC 4568 crypto suite specification, i.e.:
 *
 * $ ./test/rtp_decoder -s AES_CM_128_HMAC_SHA1_80 -b \
 *     aSBrbm93IGFsbCB5b3VyIGxpdHRsZSBzZWNyZXRz ...
 *
 * Audio can be extracted using extractaudio utility from the RTPproxy
 * package:
 *
 * $ extractaudio -A ./marseillaise-rtp.pcap ./marseillaise-out.wav
 *
 * Bernardo Torres <bernardo@torresautomacao.com.br>
 *
 * Some structure and code from https://github.com/gteissier/srtp-decrypt
 */
/*
 *
 * Copyright (c) 2001-2017 Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include "getopt_s.h" /* for local getopt()  */

#include <pcap.h>
#include "rtp_decoder.h"
#include "util.h"

#include <assert.h> /* for assert()  */
#include <stdlib.h>

#ifndef timersub
#define timersub(a, b, result)                                                 \
    do {                                                                       \
        (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                          \
        (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                       \
        if ((result)->tv_usec < 0) {                                           \
            --(result)->tv_sec;                                                \
            (result)->tv_usec += 1000000;                                      \
        }                                                                      \
    } while (0)
#endif

#define MAX_KEY_LEN 96
#define MAX_FILTER 256
#define MAX_FILE 255

struct srtp_crypto_suite {
    const char *can_name;
    bool gcm_on;
    size_t key_size;
    size_t tag_size;
};

static struct srtp_crypto_suite srtp_crypto_suites[] = {
#if 0
  {.can_name = "F8_128_HMAC_SHA1_32", .gcm_on = false, .key_size = 128, .tag_size = 4},
#endif
    { .can_name = "AES_CM_128_HMAC_SHA1_32",
      .gcm_on = false,
      .key_size = 128,
      .tag_size = 4 },
    { .can_name = "AES_CM_128_HMAC_SHA1_80",
      .gcm_on = false,
      .key_size = 128,
      .tag_size = 10 },
    { .can_name = "AES_192_CM_HMAC_SHA1_32",
      .gcm_on = false,
      .key_size = 192,
      .tag_size = 4 },
    { .can_name = "AES_192_CM_HMAC_SHA1_80",
      .gcm_on = false,
      .key_size = 192,
      .tag_size = 10 },
    { .can_name = "AES_256_CM_HMAC_SHA1_32",
      .gcm_on = false,
      .key_size = 256,
      .tag_size = 4 },
    { .can_name = "AES_256_CM_HMAC_SHA1_80",
      .gcm_on = false,
      .key_size = 256,
      .tag_size = 10 },
    { .can_name = "AEAD_AES_128_GCM",
      .gcm_on = true,
      .key_size = 128,
      .tag_size = 16 },
    { .can_name = "AEAD_AES_256_GCM",
      .gcm_on = true,
      .key_size = 256,
      .tag_size = 16 },
    { .can_name = NULL }
};

void rtp_decoder_srtp_log_handler(srtp_log_level_t level,
                                  const char *msg,
                                  void *data)
{
    (void)data;
    char level_char = '?';
    switch (level) {
    case srtp_log_level_error:
        level_char = 'e';
        break;
    case srtp_log_level_warning:
        level_char = 'w';
        break;
    case srtp_log_level_info:
        level_char = 'i';
        break;
    case srtp_log_level_debug:
        level_char = 'd';
        break;
    }
    fprintf(stderr, "SRTP-LOG [%c]: %s\n", level_char, msg);
}

int main(int argc, char *argv[])
{
    char errbuf[PCAP_ERRBUF_SIZE];
    bpf_u_int32 pcap_net = 0;
    pcap_t *pcap_handle;
#if BEW
    struct sockaddr_in local;
#endif
    srtp_sec_serv_t sec_servs = sec_serv_none;
    int c;
    struct srtp_crypto_suite scs, *i_scsp;
    scs.key_size = 128;
    scs.tag_size = 0;
    bool gcm_on = false;
    char *input_key = NULL;
    int b64_input = 0;
    uint8_t key[MAX_KEY_LEN];
    size_t mki_size = 0;
    uint8_t mki[SRTP_MAX_MKI_LEN];
    srtp_master_key_t masterKey;
    srtp_master_key_t *masterKeys[1];
    struct bpf_program fp;
    char filter_exp[MAX_FILTER] = "";
    char pcap_file[MAX_FILE] = "-";
    size_t rtp_packet_offset = DEFAULT_RTP_OFFSET;
    rtp_decoder_t dec;
    srtp_policy_t policy = { 0 };
    rtp_decoder_mode_t mode = mode_rtp;
    srtp_ssrc_t ssrc = { ssrc_any_inbound, 0 };
    uint32_t roc = 0;
    srtp_err_status_t status;
    int len;
    int expected_len;
    int do_list_mods = 0;

    fprintf(stderr, "Using %s [0x%x]\n", srtp_get_version_string(),
            srtp_get_version());

    /* initialize srtp library */
    status = srtp_init();
    if (status) {
        fprintf(stderr,
                "error: srtp initialization failed with error code %d\n",
                status);
        exit(1);
    }

    status = srtp_install_log_handler(rtp_decoder_srtp_log_handler, NULL);
    if (status) {
        fprintf(stderr, "error: install log handler failed\n");
        exit(1);
    }

    /* check args */
    while (1) {
        c = getopt_s(argc, argv, "b:k:i:gt:ae:ld:f:c:m:p:o:s:r:");
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'b':
            b64_input = 1;
        /* fall thru */
        case 'k':
            input_key = optarg_s;
            break;
        case 'i':
            mki_size =
                hex_string_to_octet_string(mki, optarg_s, strlen(optarg_s)) / 2;
            break;
        case 'e':
            scs.key_size = atoi(optarg_s);
            if (scs.key_size != 128 && scs.key_size != 192 &&
                scs.key_size != 256) {
                fprintf(stderr,
                        "error: encryption key size must be 128, 192 or 256 "
                        "(%zu)\n",
                        scs.key_size);
                exit(1);
            }
            input_key = malloc(scs.key_size);
            sec_servs |= sec_serv_conf;
            break;
        case 't':
            scs.tag_size = atoi(optarg_s);
            break;
        case 'a':
            sec_servs |= sec_serv_auth;
            break;
        case 'g':
            gcm_on = true;
            sec_servs |= sec_serv_auth;
            break;
        case 'd':
            status = srtp_set_debug_module(optarg_s, true);
            if (status) {
                fprintf(stderr, "error: set debug module (%s) failed\n",
                        optarg_s);
                exit(1);
            }
            break;
        case 'f':
            if (strlen(optarg_s) > MAX_FILTER) {
                fprintf(stderr, "error: filter bigger than %d characters\n",
                        MAX_FILTER);
                exit(1);
            }
            fprintf(stderr, "Setting filter as %s\n", optarg_s);
            strcpy(filter_exp, optarg_s);
            break;
        case 'l':
            do_list_mods = 1;
            break;
        case 'c':
            for (i_scsp = &srtp_crypto_suites[0]; i_scsp->can_name != NULL;
                 i_scsp++) {
                if (strcasecmp(i_scsp->can_name, optarg_s) == 0) {
                    break;
                }
            }
            if (i_scsp->can_name == NULL) {
                fprintf(stderr, "Unknown/unsupported crypto suite name %s\n",
                        optarg_s);
                exit(1);
            }
            scs = *i_scsp;
            input_key = malloc(scs.key_size);
            sec_servs |= sec_serv_conf | sec_serv_auth;
            gcm_on = scs.gcm_on;
            break;
        case 'm':
            if (strcasecmp("rtp", optarg_s) == 0) {
                mode = mode_rtp;
            } else if (strcasecmp("rtcp", optarg_s) == 0) {
                mode = mode_rtcp;
            } else if (strcasecmp("rtcp-mux", optarg_s) == 0) {
                mode = mode_rtcp_mux;
            } else {
                fprintf(stderr, "Unknown/unsupported mode %s\n", optarg_s);
                exit(1);
            }
            break;
        case 'p':
            if (strlen(optarg_s) > MAX_FILE) {
                fprintf(stderr,
                        "error: pcap file path bigger than %d characters\n",
                        MAX_FILE);
                exit(1);
            }
            strcpy(pcap_file, optarg_s);
            break;
        case 'o':
            rtp_packet_offset = atoi(optarg_s);
            break;
        case 's':
            ssrc.type = ssrc_specific;
            ssrc.value = strtol(optarg_s, NULL, 0);
            break;
        case 'r':
            roc = atoi(optarg_s);
            break;
        default:
            usage(argv[0]);
        }
    }

    if (scs.tag_size == 0) {
        if (gcm_on) {
            scs.tag_size = 16;
        } else {
            scs.tag_size = 10;
        }
    }

    if (gcm_on && scs.tag_size != 16) {
        fprintf(stderr, "error: GCM tag size must be 16 (%zu)\n", scs.tag_size);
        exit(1);
    }

    if (!gcm_on && scs.tag_size != 4 && scs.tag_size != 10) {
        fprintf(stderr, "error: non GCM tag size must be 4 or 10 (%zu)\n",
                scs.tag_size);
        exit(1);
    }

    if (do_list_mods) {
        status = srtp_list_debug_modules();
        if (status) {
            fprintf(stderr, "error: list of debug modules failed\n");
            exit(1);
        }
        return 0;
    }

    if ((sec_servs && !input_key) || (!sec_servs && input_key)) {
        /*
         * a key must be provided if and only if security services have
         * been requested
         */
        if (input_key == NULL) {
            fprintf(stderr, "key not provided\n");
        }
        if (!sec_servs) {
            fprintf(stderr, "no secservs\n");
        }
        fprintf(stderr, "provided\n");
        usage(argv[0]);
    }

    /* report security services selected on the command line */
    fprintf(stderr, "security services: ");
    if (sec_servs & sec_serv_conf) {
        fprintf(stderr, "confidentiality ");
    }
    if (sec_servs & sec_serv_auth) {
        fprintf(stderr, "message authentication");
    }
    if (sec_servs == sec_serv_none) {
        fprintf(stderr, "none");
    }
    fprintf(stderr, "\n");

    /* set up the srtp policy and master key */
    if (sec_servs) {
        /*
         * create policy structure, using the default mechanisms but
         * with only the security services requested on the command line,
         * using the right SSRC value
         */
        switch (sec_servs) {
        case sec_serv_conf_and_auth:
            if (gcm_on) {
#ifdef OPENSSL
                switch (scs.key_size) {
                case 128:
                    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
                    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
                    break;
                case 256:
                    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtp);
                    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtcp);
                    break;
                }
#else
                fprintf(stderr, "error: GCM mode only supported when using the "
                                "OpenSSL crypto engine.\n");
                return 0;
#endif
            } else {
                switch (scs.key_size) {
                case 128:
                    if (scs.tag_size == 4) {
                        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(
                            &policy.rtp);
                        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(
                            &policy.rtcp);
                    } else {
                        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(
                            &policy.rtp);
                        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(
                            &policy.rtcp);
                    }
                    break;
                case 192:
#ifdef OPENSSL
                    if (scs.tag_size == 4) {
                        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_32(
                            &policy.rtp);
                        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(
                            &policy.rtcp);
                    } else {
                        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(
                            &policy.rtp);
                        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(
                            &policy.rtcp);
                    }
#else
                    fprintf(stderr,
                            "error: AES 192 mode only supported when using the "
                            "OpenSSL crypto engine.\n");
                    return 0;

#endif
                    break;
                case 256:
                    if (scs.tag_size == 4) {
                        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_32(
                            &policy.rtp);
                        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(
                            &policy.rtcp);
                    } else {
                        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(
                            &policy.rtp);
                        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(
                            &policy.rtcp);
                    }
                    break;
                }
            }
            break;
        case sec_serv_conf:
            if (gcm_on) {
                fprintf(
                    stderr,
                    "error: GCM mode must always be used with auth enabled\n");
                return -1;
            } else {
                switch (scs.key_size) {
                case 128:
                    srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtp);
                    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(
                        &policy.rtcp);
                    break;
                case 192:
#ifdef OPENSSL
                    srtp_crypto_policy_set_aes_cm_192_null_auth(&policy.rtp);
                    srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(
                        &policy.rtcp);
#else
                    fprintf(stderr,
                            "error: AES 192 mode only supported when using the "
                            "OpenSSL crypto engine.\n");
                    return 0;

#endif
                    break;
                case 256:
                    srtp_crypto_policy_set_aes_cm_256_null_auth(&policy.rtp);
                    srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(
                        &policy.rtcp);
                    break;
                }
            }
            break;
        case sec_serv_auth:
            if (gcm_on) {
#ifdef OPENSSL
                switch (scs.key_size) {
                case 128:
                    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
                    policy.rtp.sec_serv = sec_serv_auth;
                    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
                    policy.rtcp.sec_serv = sec_serv_auth;
                    break;
                case 256:
                    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtp);
                    policy.rtp.sec_serv = sec_serv_auth;
                    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtcp);
                    policy.rtcp.sec_serv = sec_serv_auth;
                    break;
                }
#else
                printf("error: GCM mode only supported when using the OpenSSL "
                       "crypto engine.\n");
                return 0;
#endif
            } else {
                srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtp);
                srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
            }
            break;
        default:
            fprintf(stderr, "error: unknown security service requested\n");
            return -1;
        }

        masterKey.key = key;

        if (mki_size) {
            policy.use_mki = true;
            policy.mki_size = mki_size;
            masterKey.mki_id = mki;
        }

        masterKeys[0] = &masterKey;
        policy.keys = masterKeys;
        policy.num_master_keys = 1;

        policy.next = NULL;
        policy.window_size = 128;
        policy.allow_repeat_tx = false;
        policy.rtp.sec_serv = sec_servs;
        policy.rtcp.sec_serv =
            sec_servs; // sec_serv_none;  /* we don't do RTCP anyway */
        fprintf(stderr, "setting tag len %zu\n", scs.tag_size);
        policy.rtp.auth_tag_len = scs.tag_size;

        if (gcm_on && scs.tag_size != 8) {
            fprintf(stderr, "set tag len %zu\n", scs.tag_size);
            policy.rtp.auth_tag_len = scs.tag_size;
        }

        /*
         * read key from hexadecimal or base64 on command line into an octet
         * string
         */
        if (b64_input) {
            int pad;
            expected_len = policy.rtp.cipher_key_len * 4 / 3;
            len = base64_string_to_octet_string(key, &pad, input_key,
                                                strlen(input_key));
        } else {
            expected_len = policy.rtp.cipher_key_len * 2;
            len = hex_string_to_octet_string(key, input_key, expected_len);
        }
        /* check that hex string is the right length */
        if (len < expected_len) {
            fprintf(stderr,
                    "error: too few digits in key/salt "
                    "(should be %d digits, found %d)\n",
                    expected_len, len);
            exit(1);
        }
        if (strlen(input_key) > policy.rtp.cipher_key_len * 2) {
            fprintf(stderr,
                    "error: too many digits in key/salt "
                    "(should be %zu hexadecimal digits, found %zu)\n",
                    policy.rtp.cipher_key_len * 2, strlen(input_key));
            exit(1);
        }

        size_t key_octets = (scs.key_size / 8);
        size_t salt_octets = policy.rtp.cipher_key_len - key_octets;
        fprintf(stderr, "set master key/salt to %s/",
                octet_string_hex_string(key, key_octets));
        fprintf(stderr, "%s\n",
                octet_string_hex_string(key + key_octets, salt_octets));

        if (mki_size) {
            fprintf(stderr, "set mki to %s\n",
                    octet_string_hex_string(mki, mki_size));
        }

    } else {
        fprintf(stderr,
                "error: neither encryption or authentication were selected\n");
        exit(1);
    }

    policy.ssrc = ssrc;

    if (roc != 0 && policy.ssrc.type != ssrc_specific) {
        fprintf(stderr, "error: setting ROC (-r) requires -s <ssrc>\n");
        exit(1);
    }

    pcap_handle = pcap_open_offline(pcap_file, errbuf);

    if (!pcap_handle) {
        fprintf(stderr, "libpcap failed to open file '%s'\n", errbuf);
        exit(1);
    }
    assert(pcap_handle != NULL);
    if ((pcap_compile(pcap_handle, &fp, filter_exp, 1, pcap_net)) == -1) {
        fprintf(stderr, "Couldn't parse filter %s: %s\n", filter_exp,
                pcap_geterr(pcap_handle));
        return (2);
    }
    if (pcap_setfilter(pcap_handle, &fp) == -1) {
        fprintf(stderr, "couldn't install filter %s: %s\n", filter_exp,
                pcap_geterr(pcap_handle));
        return (2);
    }
    dec = rtp_decoder_alloc();
    if (dec == NULL) {
        fprintf(stderr, "error: malloc() failed\n");
        exit(1);
    }
    fprintf(stderr, "Starting decoder\n");
    if (rtp_decoder_init(dec, policy, mode, rtp_packet_offset, roc)) {
        fprintf(stderr, "error: init failed\n");
        exit(1);
    }

    pcap_loop(pcap_handle, 0, rtp_decoder_handle_pkt, (u_char *)dec);

    if (dec->mode == mode_rtp || dec->mode == mode_rtcp_mux) {
        fprintf(stderr, "RTP packets decoded: %zu\n", dec->rtp_cnt);
    }
    if (dec->mode == mode_rtcp || dec->mode == mode_rtcp_mux) {
        fprintf(stderr, "RTCP packets decoded: %zu\n", dec->rtcp_cnt);
    }
    fprintf(stderr, "Packet decode errors: %zu\n", dec->error_cnt);

    rtp_decoder_deinit(dec);
    rtp_decoder_dealloc(dec);

    status = srtp_shutdown();
    if (status) {
        fprintf(stderr, "error: srtp shutdown failed with error code %d\n",
                status);
        exit(1);
    }

    return 0;
}

void usage(char *string)
{
    fprintf(
        stderr,
        "usage: %s [-d <debug>]* [[-k][-b] <key>] [-a][-t][-e] [-c "
        "<srtp-crypto-suite>] [-m <mode>] [-s <ssrc> [-r <roc>]]\n"
        "or     %s -l\n"
        "where  -a use message authentication\n"
        "       -e <key size> use encryption (use 128 or 256 for key size)\n"
        "       -g Use AES-GCM mode (must be used with -e)\n"
        "       -t <tag size> Tag size to use (in GCM mode use 8 or 16)\n"
        "       -k <key>  sets the srtp master key given in hexadecimal\n"
        "       -b <key>  sets the srtp master key given in base64\n"
        "       -i <mki>  sets master key index in hexadecimal\n"
        "       -l list debug modules\n"
        "       -f \"<pcap filter>\" to filter only the desired SRTP packets\n"
        "       -d <debug> turn on debugging for module <debug>\n"
        "       -c \"<srtp-crypto-suite>\" to set both key and tag size based\n"
        "          on RFC4568-style crypto suite specification\n"
        "       -m <mode> set the mode to be one of [rtp]|rtcp|rtcp-mux\n"
        "       -p <pcap file> path to pcap file (defaults to stdin)\n"
        "       -o byte offset of RTP packet in capture (defaults to 42)\n"
        "       -s <ssrc> restrict decrypting to the given SSRC (in host byte "
        "order)\n"
        "       -r <roc> initial rollover counter, requires -s <ssrc> "
        "(defaults to 0)\n",
        string, string);
    exit(1);
}

rtp_decoder_t rtp_decoder_alloc(void)
{
    return (rtp_decoder_t)malloc(sizeof(rtp_decoder_ctx_t));
}

void rtp_decoder_dealloc(rtp_decoder_t rtp_ctx)
{
    free(rtp_ctx);
}

srtp_err_status_t rtp_decoder_deinit(rtp_decoder_t decoder)
{
    if (decoder->srtp_ctx) {
        return srtp_dealloc(decoder->srtp_ctx);
    }
    return srtp_err_status_ok;
}

srtp_err_status_t rtp_decoder_init(rtp_decoder_t dcdr,
                                   srtp_policy_t policy,
                                   rtp_decoder_mode_t mode,
                                   size_t rtp_packet_offset,
                                   uint32_t roc)
{
    dcdr->rtp_offset = rtp_packet_offset;
    dcdr->srtp_ctx = NULL;
    dcdr->start_tv.tv_usec = 0;
    dcdr->start_tv.tv_sec = 0;
    dcdr->frame_nr = -1;
    dcdr->error_cnt = 0;
    dcdr->rtp_cnt = 0;
    dcdr->rtcp_cnt = 0;
    dcdr->mode = mode;
    dcdr->policy = policy;

    srtp_err_status_t result = srtp_create(&dcdr->srtp_ctx, &dcdr->policy);
    if (result != srtp_err_status_ok) {
        return result;
    }

    if (policy.ssrc.type == ssrc_specific && roc != 0) {
        result = srtp_stream_set_roc(dcdr->srtp_ctx, policy.ssrc.value, roc);
        if (result != srtp_err_status_ok) {
            return result;
        }
    }
    return srtp_err_status_ok;
}

/*
 * decodes key as base64
 */

void hexdump(const void *ptr, size_t size)
{
    size_t i, j;
    const unsigned char *cptr = ptr;

    for (i = 0; i < size; i += 16) {
        fprintf(stdout, "%04zx ", i);
        for (j = 0; j < 16 && i + j < size; j++) {
            fprintf(stdout, "%02x ", cptr[i + j]);
        }
        fprintf(stdout, "\n");
    }
}

void rtp_decoder_handle_pkt(u_char *arg,
                            const struct pcap_pkthdr *hdr,
                            const u_char *bytes)
{
    rtp_decoder_t dcdr = (rtp_decoder_t)arg;
    rtp_msg_t message;
    bool rtp;
    ssize_t pktsize;
    struct timeval delta;
    size_t octets_recvd;
    srtp_err_status_t status;
    dcdr->frame_nr++;

    if ((dcdr->start_tv.tv_sec == 0) && (dcdr->start_tv.tv_usec == 0)) {
        dcdr->start_tv = hdr->ts;
    }

    if (hdr->caplen < dcdr->rtp_offset) {
        return;
    }
    const void *rtp_packet = bytes + dcdr->rtp_offset;

    memcpy((void *)&message, rtp_packet, hdr->caplen - dcdr->rtp_offset);
    pktsize = hdr->caplen - dcdr->rtp_offset;

    if (pktsize < 0) {
        return;
    }

    octets_recvd = pktsize;

    if (dcdr->mode == mode_rtp) {
        rtp = true;
    } else if (dcdr->mode == mode_rtcp) {
        rtp = false;
    } else {
        rtp = true;
        if (octets_recvd >= 2) {
            /* rfc5761 */
            u_char payload_type = *(bytes + dcdr->rtp_offset + 1) & 0x7f;
            rtp = payload_type < 64 || payload_type > 95;
        }
    }

    if (rtp) {
        /* verify rtp header */
        if (message.header.version != 2) {
            return;
        }

        status =
            srtp_unprotect(dcdr->srtp_ctx, (uint8_t *)&message, octets_recvd,
                           (uint8_t *)&message, &octets_recvd);
        if (status) {
            dcdr->error_cnt++;
            return;
        }
        dcdr->rtp_cnt++;
    } else {
        status = srtp_unprotect_rtcp(dcdr->srtp_ctx, (uint8_t *)&message,
                                     octets_recvd, (uint8_t *)&message,
                                     &octets_recvd);
        if (status) {
            dcdr->error_cnt++;
            return;
        }
        dcdr->rtcp_cnt++;
    }
    timersub(&hdr->ts, &dcdr->start_tv, &delta);
    fprintf(stdout, "%02ld:%02d.%06ld\n", (long)(delta.tv_sec / 60),
            (int)(delta.tv_sec % 60), (long)delta.tv_usec);
    hexdump(&message, octets_recvd);
}
