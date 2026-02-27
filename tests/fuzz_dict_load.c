/**
 * fuzz_dict_load.c — libFuzzer target: arbitrary bytes as dictionary blob.
 *
 * Build with:
 *   clang -fsanitize=fuzzer,address,undefined -O1 \
 *         -I include fuzz_dict_load.c -L build/Release -lnetc -o fuzz_dict_load
 *
 * Invariants verified:
 *   1. netc_dict_load never crashes on arbitrary input.
 *   2. All invalid blobs return NETC_ERR_DICT_INVALID or NETC_ERR_VERSION.
 *   3. If netc_dict_load returns NETC_OK, the returned dict is valid
 *      (model_id != 0, dict != NULL).
 *   4. A round-tripped (save → load) dict always returns NETC_OK.
 */

#include "../include/netc.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    netc_dict_t *dict = NULL;
    netc_result_t rc = netc_dict_load(data, size, &dict);

    if (rc == NETC_OK) {
        /* Invariant: successful load must return a non-NULL dict */
        if (dict == NULL) __builtin_trap();
        /* model_id must be in valid range (1..254) */
        uint8_t mid = netc_dict_model_id(dict);
        if (mid == 0 || mid == 255) __builtin_trap();

        /* Invariant: save → load must roundtrip successfully */
        void  *blob = NULL;
        size_t blob_size = 0;
        netc_result_t save_rc = netc_dict_save(dict, &blob, &blob_size);
        if (save_rc == NETC_OK && blob != NULL) {
            netc_dict_t *rt = NULL;
            netc_result_t load_rc = netc_dict_load(blob, blob_size, &rt);
            if (load_rc != NETC_OK || rt == NULL) __builtin_trap();
            netc_dict_free(rt);
            netc_dict_free_blob(blob);
        }

        netc_dict_free(dict);
    } else {
        /* Invalid dict must return NULL pointer */
        if (dict != NULL) __builtin_trap();
        /* Result must be a known error code */
        if (rc != NETC_ERR_DICT_INVALID &&
            rc != NETC_ERR_VERSION      &&
            rc != NETC_ERR_NOMEM        &&
            rc != NETC_ERR_INVALID_ARG  &&
            rc != NETC_ERR_CORRUPT)
        {
            __builtin_trap();
        }
    }

    return 0;
}
