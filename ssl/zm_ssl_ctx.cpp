/**
 * @file zm_ssl_ctx.cpp
 * @brief SSL/TLS 上下文管理模块实现
 */

#include "zm_ssl_ctx.h"

#include "zm_ssl_fingerprint.h"
#include "../util/zm_util_logger.h"
#include "../util/zm_util_file.h"
#include "../net/zm_net_runloop.h"

#include <../libevent/include/event2/event.h>
#include <../openssl/include/openssl/ssl.h>
#include <../openssl/include/openssl/pkcs12.h>
#include <../openssl/include/openssl/crypto.h>
#include <../openssl/include/openssl/err.h>
#include <../openssl/include/openssl/rand.h>

/**
 * @brief 获取 OpenSSL 错误描述字符串
 * @param errcode  错误码，传入 0 时自动获取最近一次错误
 * @return 错误描述 C 字符串
 */
#define ZM_OPENSSL_ERR_STR(errcode) ERR_error_string((errcode)?(errcode):ERR_get_error(), NULL)

/* ========== Internal helpers ========== */

/**
 * @brief 将 ASN1_TIME 转换为 time_t
 *
 * 支持 UTCTime (两位年份，YYMMDDHHmmSS) 和 GeneralizedTime (四位年份，YYYYMMDDHHmmSS) 两种格式。
 * 两位年份 < 70 视为 2000 年代，>= 70 视为 1900 年代。
 *
 * @param time  ASN1_TIME 指针
 * @return 对应的 time_t 值
 * @note 未做时区转换，基于本地时区解释
 */
static time_t ASN1_GetTimeT(ASN1_TIME* time)
{
    struct tm t;
    const char* str = (const char*)ASN1_STRING_get0_data(time);
    size_t i = 0;

    memset(&t, 0, sizeof(t));
    if (ASN1_STRING_type(time) == V_ASN1_UTCTIME)
    {
        /* two digit year */
        t.tm_year = (str[i++] - '0') * 10;
        t.tm_year += (str[i++] - '0');
        if (t.tm_year < 70)
        {
            t.tm_year += 100;
        }
    }
    else if (ASN1_STRING_type(time) == V_ASN1_GENERALIZEDTIME)
    {
        /* four digit year */
        t.tm_year = (str[i++] - '0') * 1000;
        t.tm_year += (str[i++] - '0') * 100;
        t.tm_year += (str[i++] - '0') * 10;
        t.tm_year += (str[i++] - '0');
        t.tm_year -= 1900;
    }
    t.tm_mon = (str[i++] - '0') * 10;
    t.tm_mon += (str[i++] - '0') - 1; // -1 since January is 0 not 1.
    t.tm_mday = (str[i++] - '0') * 10;
    t.tm_mday += (str[i++] - '0');
    t.tm_hour = (str[i++] - '0') * 10;
    t.tm_hour += (str[i++] - '0');
    t.tm_min = (str[i++] - '0') * 10;
    t.tm_min += (str[i++] - '0');
    t.tm_sec = (str[i++] - '0') * 10;
    t.tm_sec += (str[i++] - '0');

    return mktime(&t);
}


/* ========== ZmSSLContext public ========== */

/* See header for documentation */
int ZmSSLContext::SetPfxCertFilePass(SSL_CTX* sslctx, const char* pfxfile, const char* pass)
{
    //Y_LOGI("[ssl] Set the certificate from file %s", pfxfile);
    ZmByteBuffer heap(2048);
    ZmFile::Read(pfxfile, heap);
    return SetPfxCertBufferPass(sslctx, heap.Head(), heap.Size(), pass);
}

/* See header for documentation */
int ZmSSLContext::SetPfxCertBufferPass(SSL_CTX* sslctx, const unsigned char* pfxbuf, size_t len, const char* pass)
{
    int             ret = 0;
    STACK_OF(X509)* ca = NULL;
    X509* cert = NULL;
    EVP_PKEY* evpkey = NULL;
    PKCS12* pkcs12 = NULL;

    //YLoggerSub logger(//Y_LOG_TAG_ASTRALISER, "[ssl][cert] Set SSL Context certificate,");
    ZmMemoryBIO mbio(pfxbuf, len);
    while (true)
    {
        pkcs12 = d2i_PKCS12_bio(mbio.Bio(), NULL);
        if (NULL == pkcs12)
        {
            //logger.Info("Invoke d2i_PKCS12_bio() failed: %s", Y_OPENSSL_ERR_STR(0));
            ret = 2;
            break;
        }

        if (!PKCS12_verify_mac(pkcs12, pass, pass ? (int)strlen(pass) : 0))
        {
            //logger.Info("Invoke PKCS12_verify_mac() failed: %s", Y_OPENSSL_ERR_STR(0));
            /* 重新设置 MAC 以兼容 MAC 校验失败的非标准 PFX 文件 */
            PKCS12_set_mac(pkcs12, pass, pass ? (int)strlen(pass) : 0, NULL, 0, 0, NULL);
        }

        if (!PKCS12_parse(pkcs12, pass, &evpkey, &cert, &ca))
        {
            //logger.Info("Invoke PKCS12_parse() failed: %s", Y_OPENSSL_ERR_STR(0));
            ret = 3;
            break;
        }

        if (SSL_CTX_use_certificate(sslctx, cert) < 1)
        {
            //logger.Info("Invoke SSL_CTX_use_certificate() failed: %s", Y_OPENSSL_ERR_STR(0));
            ret = 11;
            break;
        }

        if (SSL_CTX_use_PrivateKey(sslctx, evpkey) < 1)
        {
            //logger.Info("Invoke SSL_CTX_use_PrivateKey() failed: %s", Y_OPENSSL_ERR_STR(0));
            ret = 12;
            break;
        }

        if (!SSL_CTX_check_private_key(sslctx))
        {
            //logger.Info("Invoke SSL_CTX_check_private_key() failed: %s", Y_OPENSSL_ERR_STR(0));
            ret = 13;
            break;
        }
        //logger.Trace("succeeded");
        break;
    }

    if (pkcs12)
    {
        PKCS12_free(pkcs12);
    }
    if (evpkey)
    {
        EVP_PKEY_free(evpkey);
    }
    if (cert)
    {
        X509_free(cert);
    }
    if (ca)
    {
        sk_X509_pop_free(ca, X509_free);
    }
    return ret;
}

/* See header for documentation */
int ZmSSLContext::UseCertBuffer(SSL_CTX* sslctx, const unsigned char* buffer, size_t len)
{
    int   ret = 0;
    ZmMemoryBIO mbio(buffer, len);
    X509* cert = PEM_read_bio_X509(mbio.Bio(), NULL, NULL, NULL);

    if (cert && 1 == SSL_CTX_use_certificate(sslctx, cert))
    {
#ifdef SSL_CTX_clear_chain_certs
        SSL_CTX_clear_chain_certs(sslctx);
#else
        if (sslctx->extra_certs != NULL)
        {
            sk_X509_pop_free(sslctx->extra_certs, X509_free);
            sslctx->extra_certs = NULL;
        }
#endif
        ret = 1;
        X509* ca = NULL;
        /* 依次读取后续证书并加入证书链 */
        while ((ca = PEM_read_bio_X509(mbio.Bio(), NULL, 0, NULL)) != NULL)
        {
            if (!SSL_CTX_add_extra_chain_cert(sslctx, ca))
            {
                X509_free(ca);
                ret = 0;
                break;
            }
            /* 成功加入链的证书由 SSL_CTX 接管，不可再 free */
        }
        if (1 == ret)
        {
            /* PEM 结束时正常会报 PEM_R_NO_START_LINE，属于预期行为，清除该错误 */
            unsigned long  err = ERR_peek_last_error();
            if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE)
            {
                ERR_clear_error();
            }
            else
            {
                ret = 0; /* 其他错误 */
            }
        }
    }
    if (cert)
    {
        X509_free(cert);
    }
    return ret;
}

/* See header for documentation */
int ZmSSLContext::UsePrivateKeyFilePass(SSL_CTX* sslctx, const char* filename, const char* pass)
{
    int  ret = -1;
    BIO* bio = BIO_new(BIO_s_file());

    if (bio)
    {
        BIO_read_filename(bio, filename);
        ret = UsePrivateKeyBIOPass(sslctx, bio, pass);
        if (BIO_set_close(bio, BIO_CLOSE)) {}
        BIO_free(bio);
    }
    return ret;
}

/* See header for documentation */
int ZmSSLContext::UsePrivateKeyBufferPass(SSL_CTX* sslctx, const unsigned char* buffer, size_t len, const char* pass)
{
    ZmMemoryBIO mbio(buffer, len);
    return UsePrivateKeyBIOPass(sslctx, mbio.Bio(), pass);
}

/* See header for documentation */
void ZmSSLContext::DumpX509(const X509* x)
{
    if (x)
    {
        ZmMemoryBIO mbio;
        X509_print_ex(mbio.Bio(), const_cast<X509*>(x), XN_FLAG_COMPAT, X509_FLAG_COMPAT);
        if (mbio.Buf())
        {
            //Y_LOGI("[ssl][cert]Dump the X509 Certificate info\n %.*s", (int)mbio.BufLen(), mbio.BufData());
        }
    }
}

/* See header for documentation */
void ZmSSLContext::DER2PEM(ZmByteBuffer& pem_, BYTE* der, size_t dlen)
{
    ZmMemoryBIO in(der, dlen);
    ZmMemoryBIO out;

    X509* cert = d2i_X509_bio(in.Bio(), NULL);
    if (cert)
    {
        PEM_write_bio_X509(out.Bio(), cert);
        if (out.Buf())
        {
            pem_.Reset(out.BufLen(), out.BufData());
        }
        X509_free(cert);
    }
}

/* See header for documentation */
bool ZmSSLContext::ParseX509(ZM_X509_INFO* xinfo, const char* pem, size_t plen)
{
    memset(xinfo, 0, sizeof(ZM_X509_INFO));
    ZmMemoryBIO in((const BYTE*)pem, plen);
    X509* x509 = PEM_read_bio_X509_AUX(in.Bio(), NULL, NULL, NULL);
    if (x509)
    {
        xinfo->version = X509_get_version(x509);
        X509_NAME_oneline(X509_get_subject_name(x509), xinfo->subject, sizeof(xinfo->subject) - 1);
        X509_NAME_oneline(X509_get_issuer_name(x509), xinfo->issuer, sizeof(xinfo->issuer) - 1);
        xinfo->not_before = ASN1_GetTimeT(X509_get_notBefore(x509));
        xinfo->not_after = ASN1_GetTimeT(X509_get_notAfter(x509));

        ASN1_INTEGER* serial = X509_get_serialNumber(x509);
        if (serial)
        {
            int serial_len = ASN1_STRING_length(serial);
            int serial_type = ASN1_STRING_type(serial);
            const unsigned char* serial_data = ASN1_STRING_get0_data(serial);

            if (serial_len <= (int)sizeof(long))
            {
                /* 短序列号: 直接以十进制输出 */
                long l = ASN1_INTEGER_get(serial);
                if (serial_type == V_ASN1_NEG_INTEGER)
                {
                    snprintf(xinfo->serial, sizeof(xinfo->serial), "-%lu", -l);
                }
                else
                {
                    snprintf(xinfo->serial, sizeof(xinfo->serial), "%lu", l);
                }
            }
            else
            {
                /* 长序列号: 以冒号分隔的十六进制输出，如 "01:23:AB:CD" */
                size_t offset = snprintf(xinfo->serial, sizeof(xinfo->serial), "%s",
                    (serial_type == V_ASN1_NEG_INTEGER) ? "-" : "");
                for (int i = 0; i < serial_len; i++)
                {
                    offset += snprintf(xinfo->serial + offset, sizeof(xinfo->serial) - offset,
                        "%s%02X", i > 0 ? ":" : "", serial_data[i]);
                }
            }
        }
        X509_free(x509);
        return true;
    }
    return false;
}

/* See header for documentation */
void ZmSSLContext::ExtractCert(ZmStringList& pems, const BYTE* data, size_t dlen, const char* password)
{
    OPENSSL_init_crypto(OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);

    ZmMemoryBIO in(data, dlen);
    PKCS12* p12 = d2i_PKCS12_bio(in.Bio(), NULL);
    if (p12)
    {
        EVP_PKEY* pkey = NULL;
        X509* cert = NULL;
        STACK_OF(X509)* ca = NULL;
        /* 首先尝试解析所有部分 (私钥+证书+CA链) */
        if (!PKCS12_parse(p12, password, &pkey, &cert, &ca))
        {
            unsigned long errcode = ERR_get_error();
            //Y_LOGI("[ssl][cert] Extract certificate, PKCS12_parse(PKEY) failed: %s", Y_OPENSSL_ERR_STR(errcode));
            /*
             * 标准 OpenSSL 无法解析 SM2 证书私钥 (错误码 0x100D5010)，
             * 此时会放弃私钥，仅解析证书和 CA 链。
             */
            if (0x100D5010UL == errcode && PKCS12_parse(p12, password, NULL, &cert, &ca))
            {
                //Y_LOGI("[ssl][cert] Extract certificate, PKCS12_parse(NONE-PKEY) failed: %s", Y_OPENSSL_ERR_STR(0));
            }
        }
        if (pkey)
        {
            ZmMemoryBIO mbio;
            PEM_write_bio_PrivateKey(mbio.Bio(), pkey, NULL, NULL, 0, NULL, NULL);
            if (mbio.Buf())
            {
                pems.AddEntry(mbio.BufData(), mbio.BufLen());
            }
            EVP_PKEY_free(pkey);
        }
        if (cert)
        {
            ZmMemoryBIO mbio;
            PEM_write_bio_X509(mbio.Bio(), cert);
            if (mbio.Buf())
            {
                pems.AddEntry(mbio.BufData(), mbio.BufLen());
            }
            X509_free(cert);
        }
        if (ca)
        {
            for (int i = 0; i < sk_X509_num(ca); i++)
            {
                ZmMemoryBIO mbio;
                PEM_write_bio_X509(mbio.Bio(), sk_X509_value(ca, i));
                if (mbio.Buf())
                {
                    pems.AddEntry(mbio.BufData(), mbio.BufLen());
                }
            }
            sk_X509_pop_free(ca, X509_free);
        }
        PKCS12_free(p12);
    }
    else
    {
        //Y_LOGI("[ssl][cert] Extract certificate, d2i_PKCS12_bio() failed: %s", Y_OPENSSL_ERR_STR(0));
    }
}


/* ========== ZmSSLContext private ========== */

/* See header for documentation */
SSL_CTX* ZmSSLContext::MakeClientCTX()
{
    OPENSSL_init_ssl(0, NULL);

    SSL_CTX* sslctx = SSL_CTX_new(TLS_client_method());
    if (NULL == sslctx)
    {
        return NULL;
    }

    SSL_CTX_set_options(sslctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    // 现代密码套件：仅 ECDHE 前向安全 + GCM/CHACHA20 AEAD 加密
    SSL_CTX_set_cipher_list(sslctx,
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305");

    SSL_CTX_set_verify(sslctx, SSL_VERIFY_NONE, NULL);
    return sslctx;
}

/* See header for documentation */
SSL_CTX* ZmSSLContext::MakeClientCTX(const char* certFile, const char* keyFile)
{
    SSL_CTX* ctx = MakeClientCTX();
    if (NULL == ctx)
        return NULL;

    // ★ 使用 certificate_chain_file：支持 PEM 文件中包含服务器证书 + 中间 CA 链
    if (SSL_CTX_use_certificate_chain_file(ctx, certFile) != 1)
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (UsePrivateKeyFilePass(ctx, keyFile, "") != 1)
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ctx))
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

/* See header for documentation */
SSL_CTX* ZmSSLContext::MakeServerCTX(uint32_t sessionCacheSize,
                                     const char* sessionContext)
{
    OPENSSL_init_ssl(0, NULL);

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (NULL == ctx)
        return NULL;

    // 禁用不安全的老协议版本，仅允许 TLS 1.2+
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    // 现代密码套件：仅 ECDHE 前向安全 + GCM/CHACHA20 AEAD 加密
    SSL_CTX_set_cipher_list(ctx,
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305");

    // 启用自动 DH/ECDH 参数协商
    SSL_CTX_set_dh_auto(ctx, 1);
    SSL_CTX_set_ecdh_auto(ctx, 1);

    // Session Cache：仅在显式传入容量时启用（默认不启用）
    if (sessionCacheSize > 0 && sessionContext)
    {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx, sessionCacheSize);
        SSL_CTX_set_session_id_context(ctx,
            (const unsigned char*)sessionContext,
            (unsigned int)strlen(sessionContext));
    }

    return ctx;
}

/* See header for documentation */
SSL_CTX* ZmSSLContext::MakeServerCTX(const char* certFile, const char* keyFile,
                                     uint32_t sessionCacheSize,
                                     const char* sessionContext)
{
    SSL_CTX* ctx = MakeServerCTX(sessionCacheSize, sessionContext);
    if (NULL == ctx)
        return NULL;

    // ★ 使用 certificate_chain_file：支持 PEM 文件中包含服务器证书 + 中间 CA 链
    if (SSL_CTX_use_certificate_chain_file(ctx, certFile) != 1)
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (UsePrivateKeyFilePass(ctx, keyFile, "") != 1)
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ctx))
    {
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

void ZmSSLContext::Release()
{
    CONF_modules_unload(1);
}

bool ZmSSLContext::ValidateSSLFingerprint(evutil_socket_t fd, SSL* ssl, const char* host, uint16_t port)
{
    return ZmSSLFingerprint::instance()->Validate(fd, ssl, host, port);
}

bool ZmSSLContext::ValidateSSLFingerprint(SSL* ssl, const char* fingerprint)
{
    return ZmSSLFingerprint::instance()->Validate(ssl, fingerprint);
}

/* See header for documentation */
int ZmSSLContext::UsePrivateKeyBIOPass(SSL_CTX* sslctx, void* bio, const char* pass)
{
    int       ret = -1;
    BIO* key = (BIO*)bio;
    /* pass 通过 OpenSSL 的 pem_password_cb 回调参数传入，而非标准回调函数 */
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key, NULL, NULL, (void*)pass);

    if (NULL != pkey)
    {
        if (SSL_CTX_use_PrivateKey(sslctx, pkey) > 0)
        {
            ret = 1;
        }
        else
        {
            //Y_LOGI("[ssl][cert] Use Private key by BIO-Pass, SSL_CTX_use_PrivateKey() failed: %s", Y_OPENSSL_ERR_STR(0));
        }
        EVP_PKEY_free(pkey);
    }
    else
    {
        //Y_LOGI("[ssl][cert] Use Private key by BIO-Pass,  PEM_read_bio_PrivateKey() failed: %s", Y_OPENSSL_ERR_STR(0));
    }
    return ret;
}


ZmSSLContext::~ZmSSLContext()
{
    Release();
}

/* ========== ZmMemoryBIO ========== */

/* See header for documentation */
ZmMemoryBIO::ZmMemoryBIO(const BYTE* data /*= NULL*/, size_t len /*= 0*/) : m_bio(NULL), m_buf(NULL)
{
    m_bio = (NULL != data) ? BIO_new_mem_buf((void*)data, (int)len) : BIO_new(BIO_s_mem());
}

/* See header for documentation */
ZmMemoryBIO::~ZmMemoryBIO()
{
    if (m_bio)
    {
        if (BIO_set_close(m_bio, BIO_CLOSE)) {}
        BIO_free_all(m_bio);
    }
}

/* See header for documentation */
BIO* ZmMemoryBIO::Bio()
{
    return m_bio;
}

/* See header for documentation */
BUF_MEM* ZmMemoryBIO::Buf()
{
    BIO_get_mem_ptr(m_bio, &m_buf);
    return m_buf;
}

/* See header for documentation */
const char* ZmMemoryBIO::BufData()
{
    return Buf() ? m_buf->data : NULL;
}

/* See header for documentation */
size_t ZmMemoryBIO::BufLen()
{
    return Buf() ? m_buf->length : 0;
}

// ============================================================================
// ZmSessionTicketManager
// ============================================================================

ZmSessionTicketManager::ZmSessionTicketManager()
{
    memset(&m_key, 0, sizeof(m_key));
}

ZmSessionTicketManager::~ZmSessionTicketManager()
{
    // 密钥用完后清零,防止内存泄漏后泄露密钥
    OPENSSL_cleanse(&m_key, sizeof(m_key));
}

bool ZmSessionTicketManager::Init(const char* ticketFile)
{
    if (ticketFile && ticketFile[0])
        m_ticketFile = ticketFile;

    bool loaded = false;

    // ① 尝试从已有文件加载(兼容旧 48B 格式,自动重生成覆盖)
    if (!m_ticketFile.empty())
    {
        ZmByteBuffer heap(ZM_TICKET_KEYS_LEN + 16);
        if (ZmFile::Read(m_ticketFile.c_str(), heap))
        {
            if (heap.Size() == ZM_TICKET_KEYS_LEN)
            {
                memcpy(&m_key, heap.Head(), ZM_TICKET_KEYS_LEN);
                loaded = true;
            }
            else if (heap.Size() == 48)
            {
                // 旧 48B 格式(name16|hmac16|aes16)从未启用过,直接重生成
                PUBLIC_LOG_WARN("ticket key 文件 {} 为旧 48B 格式,重新生成 80B 密钥", m_ticketFile);
            }
        }
    }

    // ② 不存在或无效 → 生成随机密钥
    if (!loaded)
    {
        if (RAND_bytes(m_key.data, ZM_TICKET_KEYS_LEN) != 1)
            return false;

        // 持久化到文件
        if (!m_ticketFile.empty())
            ZmFile::Write(m_ticketFile.c_str(), m_key.data, ZM_TICKET_KEYS_LEN);
    }

    return true;
}

void ZmSessionTicketManager::RotateKeys()
{
    if (RAND_bytes(m_key.data, ZM_TICKET_KEYS_LEN) != 1)
        return;  // 本轮轮换失败,保留旧 key 继续使用

    if (!m_ticketFile.empty())
        ZmFile::Write(m_ticketFile.c_str(), m_key.data, ZM_TICKET_KEYS_LEN);
}

void ZmSessionTicketManager::SetKeys(struct ssl_ctx_st* ctx)
{
    if (!ctx) return;

    if (SSL_CTX_set_tlsext_ticket_keys((SSL_CTX*)ctx, m_key.data,
                                       ZM_TICKET_KEYS_LEN) != 1)
    {
        // 失败不影响 TLS:回退 OpenSSL 内部随机密钥(仅恢复不跨重启)
        PUBLIC_LOG_WARN("SetKeys: SSL_CTX_set_tlsext_ticket_keys 失败,回退内部随机密钥");
    }
}

void ZmSessionTicketManager::SetTicketDataCB(struct ssl_ctx_st* ctx,
                                             int (*gen_cb)(SSL*, void*),
                                             int (*dec_cb)(SSL*, SSL_SESSION*,
                                                           const unsigned char*,
                                                           size_t, int, void*),
                                             void* arg)
{
    if (!ctx) return;
    SSL_CTX_set_session_ticket_cb((SSL_CTX*)ctx, gen_cb, dec_cb, arg);
}

// ============================================================================
// ZmTicketKeyRotator
// ============================================================================

ZmTicketKeyRotator::ZmTicketKeyRotator()
    : m_evLoop(nullptr), m_timer(nullptr), m_ctrlEvent(nullptr)
{}

ZmTicketKeyRotator::~ZmTicketKeyRotator()
{
    Stop();
}

bool ZmTicketKeyRotator::Init(const char* ticketFile)
{
    return m_ticketMgr.Init(ticketFile);
}

bool ZmTicketKeyRotator::Start(time_t intervalSeconds, OnRotateCB onRotate)
{
    if (m_evLoop)
        return false;  // 已经启动

    m_onRotate = onRotate;

    // ① 创建独立事件循环线程
    m_evLoop = new ZmEvBaseRunLoop("TicketKeyRotator");
    if (!m_evLoop->Loop())
    {
        delete m_evLoop;
        m_evLoop = nullptr;
        return false;
    }

    struct event_base* evbase = m_evLoop->GetEventBase();

    // ② 持久定时器：到期自动轮换
    m_timer = event_new(evbase, -1, EV_PERSIST, OnTimer, this);
    if (m_timer)
    {
        struct timeval tv = { (long)intervalSeconds, 0 };
        event_add(m_timer, &tv);
    }

    // ③ 控制事件：支持跨线程 RotateNow() 调用
    m_ctrlEvent = event_new(evbase, -1, EV_READ | EV_PERSIST, OnCtrl, this);
    if (m_ctrlEvent)
        event_add(m_ctrlEvent, nullptr);

    return true;
}

void ZmTicketKeyRotator::RotateNow()
{
    if (m_ctrlEvent)
        event_active(m_ctrlEvent, CTRL_ROTATE, 0);
}

bool ZmTicketKeyRotator::IsRunning() const
{
    return m_evLoop != nullptr && m_evLoop->IsLooped();
}

void ZmTicketKeyRotator::Stop()
{
    if (m_timer)
    {
        event_free(m_timer);
        m_timer = nullptr;
    }
    if (m_ctrlEvent)
    {
        event_free(m_ctrlEvent);
        m_ctrlEvent = nullptr;
    }
    if (m_evLoop)
    {
        m_evLoop->Stop();
        delete m_evLoop;
        m_evLoop = nullptr;
    }
}

void ZmTicketKeyRotator::OnTimer(evutil_socket_t, short, void* arg)
{
    ((ZmTicketKeyRotator*)arg)->doRotate();
}

void ZmTicketKeyRotator::OnCtrl(evutil_socket_t, short what, void* arg)
{
    if (what & CTRL_ROTATE)
        ((ZmTicketKeyRotator*)arg)->doRotate();
}

void ZmTicketKeyRotator::doRotate()
{
    m_ticketMgr.RotateKeys();
    if (m_onRotate)
        m_onRotate();
}
