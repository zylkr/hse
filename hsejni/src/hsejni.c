/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2017,2019 Micron Technology, Inc.  All rights reserved.
 */
#include <jni.h>
#include <hse_jni_util.h>
#include <hsejni_internal.h>

#include <hse/hse.h>

#include <hse_util/hse_params_helper.h>

#define MAX_ARGS 32

#include <syslog.h>
#include <pthread.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

struct hse_kvdb *kvdb_h;

size_t        g_val_buf_size = 8192;
pthread_key_t td_getbuf_key;
pthread_key_t td_cursor_key;

_Thread_local struct hse_kvs_cursor *cursor = NULL;

static int
jni_params_string_to_argv(char *p_list, char **argv, uint32_t max_args)
{
    char *cp;
    int   argc;

    if (!p_list || !argv)
        return -EINVAL;

    argc = 0;
    while ((cp = strsep(&p_list, ",")) && argc < max_args)
        if (*cp)
            argv[argc++] = cp;

    if (argc == max_args && cp && *cp)
        syslog(
            LOG_ERR,
            "(HSE JNI) "
            "Extraneous parameters passed (max: %d). Only "
            "the first %d parameters will be processed",
            max_args,
            max_args);

    return argc;
}

int
jni_hse_params_parse(struct hse_params *params, const char *p_list)
{
    int   argc;
    char *argv[MAX_ARGS];
    char *args;
    int   rc;
    int   next_arg = 0;

    args = strdup(p_list);
    if (!args)
        return (-errno);

    argc = jni_params_string_to_argv(args, argv, MAX_ARGS);
    if (argc > 0)
        rc = hse_parse_cli(argc, argv, &next_arg, 0, params);
    else
        rc = argc;

    free(args);

    return rc;
}

int
split_str(char **substr1, char **substr2, char *str)
{
    char *p;

    /* split str at the last occurrence of '/' */
    p = strrchr(str, '/');
    if (p)
        *p++ = 0;
    else
        return -1;

    *substr1 = str;
    *substr2 = p;

    return 0;
}

static void
throw_err(JNIEnv *env, const char *func, uint64_t err)
{
    char err_buf[200];
    char msg[300];

    hse_err_to_string(err, err_buf, sizeof(err_buf), 0);

    snprintf(msg, sizeof(msg), "%s: %s", func, err_buf);

    throw_gen_exception(env, msg);
}

static void
td_exit_getbuf(void *arg)
{
    free(arg);
}

static void
td_exit_cursor(void *arg)
{
    char     err_buf[200];
    uint64_t rc;

    rc = hse_kvs_cursor_destroy((struct hse_kvs_cursor *)arg);
    if (rc) {
        hse_err_to_string(rc, err_buf, sizeof(err_buf), 0);
        syslog(LOG_ERR, "(HSE JNI) %s: hse_kvs_cursor_destroy: %s", __func__, err_buf);
    }
}

JNIEXPORT void JNICALL
Java_org_micron_hse_API_init(JNIEnv *env, jobject jobj, jlong valBufSize)
{
    int      rc;
    uint64_t hse_rc;

    hse_rc = hse_kvdb_init();
    if (hse_rc) {
        char buf[1024];

        hse_err_to_string(hse_rc, buf, 1024, 0);
        syslog(LOG_ERR, "(HSE JNI) %s: hse_kvdb_init: %s", __func__, buf);
    }

    if (valBufSize > g_val_buf_size)
        g_val_buf_size = (valBufSize + 8191) & ~8191;

    rc = pthread_key_create(&td_getbuf_key, td_exit_getbuf);
    if (rc)
        syslog(LOG_ERR, "(HSE JNI) %s: pthread_key_create: %s", __func__, strerror(rc));

    rc = pthread_key_create(&td_cursor_key, td_exit_cursor);
    if (rc)
        syslog(LOG_ERR, "(HSE JNI) %s: pthread_key_create: %s", __func__, strerror(rc));

    syslog(
        LOG_ERR,
        "(HSE JNI) %s: env %p, valBufSize %ld, g_val_buf_size %ld",
        __func__,
        env,
        valBufSize,
        g_val_buf_size);
}

JNIEXPORT void JNICALL
Java_org_micron_hse_API_fini(JNIEnv *env, jobject jobj)
{
    hse_kvdb_fini();
}

JNIEXPORT void JNICALL
               Java_org_micron_hse_API_open(
    JNIEnv *env,
    jobject jobj,
    jshort  dbType,
    jstring mpoolName,
    jstring dsName,
    jstring hseParamsList,
    jstring configPath)
{
    jboolean           isCopy = JNI_FALSE;
    const char *       aMpoolName = NULL;
    const char *       aDsName = NULL;
    const char *       aHseParamsList = NULL;
    const char *       aConfigPath = NULL;
    char *             aKvdbName = NULL;
    char *             aKvsName = NULL;
    char *             dsdup = NULL;
    void *             kvs_h = NULL;
    struct hse_params *params;
    uint64_t           rc;

    jclass   apiCls = 0;
    jfieldID jf = 0;

    apiCls = (*env)->GetObjectClass(env, jobj);
    if (NULL == apiCls) {
        throw_gen_exception(env, "API Class not found");
        return;
    }

    jf = (*env)->GetFieldID(env, apiCls, "nativeHandle", "J");
    if (NULL == jf) {
        throw_gen_exception(env, "JNI: jfieldID not found");
        return;
    }

    hse_params_create(&params);
    if (!params) {
        throw_gen_exception(
            env,
            "HSE parameters "
            "could not be created: "
            "Not enough memory");
    }

    aMpoolName = (*env)->GetStringUTFChars(env, mpoolName, &isCopy);
    aDsName = (*env)->GetStringUTFChars(env, dsName, &isCopy);
    aHseParamsList = (*env)->GetStringUTFChars(env, hseParamsList, &isCopy);
    aConfigPath = (*env)->GetStringUTFChars(env, configPath, &isCopy);

    /*
     * [HSE_REVISIT] Once kvdb name is passed as a parameter, this parsing
     * will no longer be necessary
     */
    dsdup = strdup(aDsName);
    if (!dsdup) {
        throw_gen_exception(env, "Not enough memory");
        return;
    }

    if (split_str(&aKvdbName, &aKvsName, dsdup)) {
        throw_gen_exception(env, "Please use the format: kvdb/kvs");
        goto out;
    }

    if (aConfigPath && aConfigPath[0]) {
        rc = hse_params_from_file(params, aConfigPath);
        if (rc) {
            throw_err(env, "hse_params_from_file", rc);
            goto out;
        }
    }

    if (aHseParamsList && aHseParamsList[0]) {
        if (jni_hse_params_parse(params, aHseParamsList)) {
            throw_gen_exception(
                env,
                "HSE parameters "
                "could not be parsed: "
                "Not enough memory");
            goto out;
        }
    }

    rc = hse_kvdb_open(aMpoolName, params, &kvdb_h);
    if (rc) {
        throw_err(env, "hse_kvdb_open", rc);
        goto out;
    }

    rc = hse_kvdb_kvs_open(kvdb_h, aKvsName, params, (struct hse_kvs **)&kvs_h);

    if (hse_err_to_errno(rc) == ENOENT) {
        rc = hse_kvdb_kvs_make(kvdb_h, aKvsName, params);
        if (rc) {
            hse_kvdb_close(kvdb_h);
            throw_err(env, "hse_kvdb_kvs_make", rc);
            goto out;
        }

        rc = hse_kvdb_kvs_open(kvdb_h, aKvsName, params, (struct hse_kvs **)&kvs_h);
    }

    if (rc) {
        hse_kvdb_close(kvdb_h);
        throw_err(env, "hse_kvdb_kvs_open", rc);
        goto out;
    }

    (*env)->SetLongField(env, jobj, jf, (jlong)kvs_h);

    (*env)->ReleaseStringUTFChars(env, mpoolName, aMpoolName);
    (*env)->ReleaseStringUTFChars(env, dsName, aDsName);
    (*env)->ReleaseStringUTFChars(env, hseParamsList, aHseParamsList);
    (*env)->ReleaseStringUTFChars(env, configPath, aConfigPath);

out:
    hse_params_destroy(params);
    free(dsdup);
}

JNIEXPORT jint JNICALL
Java_org_micron_hse_API_close(JNIEnv *env, jobject jobj, jlong handle)
{
    uint64_t rc;

    if (cursor) {
        rc = pthread_setspecific(td_cursor_key, NULL);
        if (rc) {
            throw_err(env, "pthread_setspecific", rc);
            goto errout;
        }

        rc = hse_kvs_cursor_destroy(cursor);
        if (rc) {
            throw_err(env, "hse_kvs_cursor_destroy", rc);
            goto errout;
        }

        cursor = NULL;
    }

    rc = hse_kvdb_kvs_close((void *)handle);
    if (rc) {
        throw_err(env, "hse_kvdb_kvs_close", rc);
        goto errout;
    }

    rc = hse_kvdb_close(kvdb_h);
    if (rc) {
        throw_err(env, "hse_kvdb_close", rc);
        goto errout;
    }

errout:
    return rc;
}

JNIEXPORT jint JNICALL
               Java_org_micron_hse_API_put(
    JNIEnv *   env,
    jobject    jobj,
    jlong      handle,
    jbyteArray key,
    jbyteArray value)
{
    jint         rval = 0;
    jbyte *      keyA;
    unsigned int keyL;
    jbyte *      valueA;
    unsigned int valueL;
    jboolean     isCopy;
    uint64_t     rc;

    keyA = (*env)->GetByteArrayElements(env, key, &isCopy);
    if (!keyA) {
        throw_gen_exception(env, "Failed to get key bytes");
        return -1;
    }

    valueA = (*env)->GetByteArrayElements(env, value, &isCopy);
    if (!valueA) {
        (*env)->ReleaseByteArrayElements(env, key, keyA, JNI_ABORT);
        throw_gen_exception(env, "Failed to get value bytes");
        return -1;
    }

    keyL = (*env)->GetArrayLength(env, key);
    valueL = (*env)->GetArrayLength(env, value);

    rc = hse_kvs_put((void *)handle, NULL, keyA, keyL, valueA, valueL);
    if (rc) {
        char msg[128];
        int  n;

        n = snprintf(msg, sizeof(msg), "hse_kvs_put: keyL=%u valueL=%u: ", keyL, valueL);
        strerror_r(hse_err_to_errno(rc), msg + n, sizeof(msg) - n);

        throw_gen_exception(env, msg);
        rval = -1;
    }

    (*env)->ReleaseByteArrayElements(env, key, keyA, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, value, valueA, JNI_ABORT);

    return rval;
}

JNIEXPORT jbyteArray JNICALL
Java_org_micron_hse_API_get(JNIEnv *env, jobject jobj, jlong handle, jbyteArray key)
{
    static _Thread_local jbyte *valBuf;

    jbyte *      keyA;
    unsigned int keyL;
    jboolean     isCopy;
    bool         found;
    size_t       vlen;
    jbyteArray   jRetVal = NULL;
    int          i, n;
    uint64_t     rc;

    /* Each calling thread does a once-only allocation of valBuf
     * which will be freed at thread exit by td_exit().
     */
    if (!valBuf) {
        int irc;

        irc = posix_memalign((void **)&valBuf, 4096, g_val_buf_size);
        if (irc || !valBuf) {
            throw_gen_exception(env, "failed to alloc valBuf");
            return NULL;
        }

        irc = pthread_setspecific(td_getbuf_key, valBuf);
        if (irc)
            syslog(LOG_ERR, "(HSE_JNI) %s: pthread_setspecific: %s", __func__, strerror(irc));
    }

    keyA = (*env)->GetByteArrayElements(env, key, &isCopy);
    if (!keyA) {
        throw_gen_exception(env, "Failed to get key bytes");
        return NULL;
    }

    keyL = (*env)->GetArrayLength(env, key);

    rc = hse_kvs_get((void *)handle, NULL, keyA, keyL, &found, valBuf, g_val_buf_size, &vlen);
    if (rc) {
        char msg[128];

        n = snprintf(msg, sizeof(msg), "hse_kvs_get: keyL=%u vlen=%zu: ", keyL, vlen);
        strerror_r(hse_err_to_errno(rc), msg + n, sizeof(msg) - n);

        throw_gen_exception(env, msg);
        goto errout;
    }

    if (!found) {
        char *kbuf = (char *)keyA;
        char  msg[64 + keyL];

        n = snprintf(msg, sizeof(msg), "hse_kvs_get: key not found: keyL=%u: ", keyL);

        for (i = 0; i < keyL; ++i)
            msg[n + i] = isprint(kbuf[i]) ? kbuf[i] : '.';
        msg[n + i] = '\000';

        throw_gen_exception(env, msg);
        goto errout;
    }

    jRetVal = (*env)->NewByteArray(env, vlen);
    if (!jRetVal) {
        throw_gen_exception(env, "Failed to alloc jRetVal");
        goto errout;
    }

    (*env)->SetByteArrayRegion(env, jRetVal, 0, vlen, valBuf);

errout:
    (*env)->ReleaseByteArrayElements(env, key, keyA, JNI_ABORT);

    return jRetVal;
}

JNIEXPORT jint JNICALL
Java_org_micron_hse_API_del(JNIEnv *env, jobject jobj, jlong handle, jbyteArray key)
{
    jint         rval = 0;
    jbyte *      keyA;
    unsigned int keyL;
    jboolean     isCopy;
    uint64_t     rc;

    keyA = (*env)->GetByteArrayElements(env, key, &isCopy);
    if (!keyA) {
        throw_gen_exception(env, "Failed to read key bytes");
        return -1;
    }

    keyL = (*env)->GetArrayLength(env, key);

    rc = hse_kvs_delete((void *)handle, NULL, keyA, keyL);
    if (rc) {
        char msg[128];
        int  n;

        n = snprintf(msg, sizeof(msg), "hse_kvs_delete: keyL=%u: ", keyL);
        strerror_r(hse_err_to_errno(rc), msg + n, sizeof(msg) - n);

        throw_gen_exception(env, msg);
        rval = -1;
    }

    (*env)->ReleaseByteArrayElements(env, key, keyA, JNI_ABORT);

    return rval;
}

JNIEXPORT void JNICALL
               Java_org_micron_hse_API_createCursor(
    JNIEnv *env,
    jobject jobj,
    jlong   handle,
    jstring pfx,
    jint    pfxlen)
{
    jboolean    isCopy = JNI_FALSE;
    const char *cpfx = NULL;
    uint64_t    rc;

    if (cursor != NULL) {
        throw_gen_exception(env, "Cursor already created");
        return;
    }

    if (!pfx && pfxlen != 0) {
        throw_gen_exception(env, "No prefix specified");
        return;
    }

    if (pfx && pfxlen != 0)
        cpfx = (*env)->GetStringUTFChars(env, pfx, &isCopy);

    rc = hse_kvs_cursor_create((void *)handle, NULL, cpfx, pfxlen, &cursor);
    if (rc) {
        cursor = NULL;
        throw_err(env, "hse_kvs_cursor_create", rc);
        return;
    }

    rc = pthread_setspecific(td_cursor_key, cursor);
    if (rc) {
        throw_err(env, "pthread_setspecific", rc);
        return;
    }
}

JNIEXPORT void JNICALL
Java_org_micron_hse_API_destroyCursor(JNIEnv *env, jobject jobj, jlong handle)
{
    uint64_t rc;

    if (cursor == NULL) {
        throw_gen_exception(env, "No active cursor; not created?");
        return;
    }

    rc = pthread_setspecific(td_cursor_key, NULL);
    if (rc) {
        throw_err(env, "pthread_setspecific", rc);
        return;
    }

    rc = hse_kvs_cursor_destroy(cursor);
    if (rc) {
        throw_err(env, "hse_kvs_cursor_destroy", rc);
        return;
    }

    cursor = NULL;
}

JNIEXPORT jbyteArray JNICALL
Java_org_micron_hse_API_seek(JNIEnv *env, jobject jobj, jlong handle, jbyteArray key)
{
    jbyte *      keyA;
    unsigned int keyL;
    jboolean     isCopy;
    const void * foundKey;
    size_t       foundKeyL;
    uint64_t     rc;
    jbyteArray   jRetVal = NULL;

    if (cursor == NULL) {
        throw_gen_exception(env, "No active cursor; not created?");
        goto errout;
    }

    keyA = (*env)->GetByteArrayElements(env, key, &isCopy);
    if (!keyA) {
        throw_gen_exception(env, "Failed to read key bytes");
        goto errout;
    }

    keyL = (*env)->GetArrayLength(env, key);

    rc = hse_kvs_cursor_seek(cursor, NULL, keyA, keyL, &foundKey, &foundKeyL);

    (*env)->ReleaseByteArrayElements(env, key, keyA, JNI_ABORT);

    if (rc) {
        throw_err(env, "hse_kvs_cursor_seek", rc);
        goto errout;
    }

    if (foundKey != NULL) {
        jRetVal = (*env)->NewByteArray(env, foundKeyL);
        if (jRetVal == NULL) {
            throw_gen_exception(env, "Failed to alloc jRetVal");
            goto errout;
        }

        (*env)->SetByteArrayRegion(env, jRetVal, 0, foundKeyL, foundKey);
    }

errout:
    return jRetVal;
}

JNIEXPORT jbyteArray JNICALL
Java_org_micron_hse_API_read(JNIEnv *env, jobject jobj, jlong handle)
{
    const void *keyBuf, *valBuf;
    size_t      klen, vlen;
    bool        eof;
    uint64_t    rc;
    jbyteArray  jRetVal = NULL;

    if (cursor == NULL) {
        throw_gen_exception(env, "No active cursor; not created?");
        goto errout;
    }

    rc = hse_kvs_cursor_read(cursor, NULL, &keyBuf, &klen, &valBuf, &vlen, &eof);

    if (rc) {
        throw_err(env, "hse_kvs_cursor_read", rc);
        goto errout;
    }

    if (eof) {
        throw_eof_exception(env);
        goto errout;
    }

    jRetVal = (*env)->NewByteArray(env, vlen);
    if (!jRetVal) {
        throw_gen_exception(env, "Failed to alloc jRetVal");
        goto errout;
    }

    (*env)->SetByteArrayRegion(env, jRetVal, 0, vlen, valBuf);

errout:
    return jRetVal;
}
