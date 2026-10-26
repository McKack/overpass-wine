/* SPDX-License-Identifier: LGPL-2.1-or-later
 * License verification and public metadata stay inside the runtime. The only
 * credential operation is the existing StoreLicenseTokenRequest bridge. */
#include <wincrypt.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <math.h>

static HRESULT receipt_json( const BYTE *bytes, SIZE_T size, IJsonObject **out )
{
    IJsonValueStatics *statics = NULL;
    IJsonValue *value = NULL;
    HSTRING name = NULL, text = NULL;
    WCHAR *wide = NULL;
    int length;
    HRESULT hr = E_UNEXPECTED;
    *out = NULL;
    if (!bytes || !size || size > 4 * 1024 * 1024 || memchr(bytes, 0, size)) return hr;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)bytes, size, NULL, 0);
    if (!length) return hr;
    if (!(wide = calloc(length, sizeof(WCHAR)))) return E_OUTOFMEMORY;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)bytes, size, wide, length);
    if (FAILED(hr = WindowsCreateString(RuntimeClass_Windows_Data_Json_JsonValue,
        wcslen(RuntimeClass_Windows_Data_Json_JsonValue), &name)) ||
        FAILED(hr = RoGetActivationFactory(name, &IID_IJsonValueStatics, (void **)&statics)) ||
        FAILED(hr = WindowsCreateString(wide, length, &text)) ||
        FAILED(hr = IJsonValueStatics_Parse(statics, text, &value))) goto done;
    hr = IJsonValue_GetObject(value, out);
done:
    if (value) IJsonValue_Release(value);
    if (statics) IJsonValueStatics_Release(statics);
    WindowsDeleteString(name); WindowsDeleteString(text); free(wide); return hr;
}

#define RECEIPT_GETTER(name, sdk, type) \
static HRESULT receipt_get_##name( IJsonObject *obj, const WCHAR *key, type *out ) \
{ HSTRING text; HRESULT hr = WindowsCreateString(key, wcslen(key), &text); \
  if (SUCCEEDED(hr)) { hr = IJsonObject_GetNamed##sdk(obj, text, out); WindowsDeleteString(text); } return hr; }
RECEIPT_GETTER(string, String, HSTRING)
RECEIPT_GETTER(number, Number, double)
RECEIPT_GETTER(array, Array, IJsonArray *)
RECEIPT_GETTER(object, Object, IJsonObject *)
RECEIPT_GETTER(boolean, Boolean, boolean)
#undef RECEIPT_GETTER

static HRESULT receipt_array_size( IJsonArray *array, UINT32 *size )
{
    IVector_IJsonValue *vector;
    HRESULT hr = IJsonArray_QueryInterface(array, &IID_IVector_IJsonValue, (void **)&vector);
    if (SUCCEEDED(hr)) { hr = IVector_IJsonValue_get_Size(vector, size); IVector_IJsonValue_Release(vector); }
    return hr;
}

static char *receipt_string( IJsonObject *obj, const WCHAR *name, UINT32 limit )
{
    HSTRING text;
    UINT32 length;
    const WCHAR *wide;
    char *result = NULL;
    int size;
    if (FAILED(receipt_get_string(obj, name, &text))) return NULL;
    wide = WindowsGetStringRawBuffer(text, &length);
    if (length > limit) goto done;
    for (UINT32 i = 0; i < length; i++) if (!wide[i]) goto done;
    size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, NULL, 0, NULL, NULL);
    if (length && !size) goto done;
    if (!(result = calloc(size + 1, 1))) goto done;
    if (size) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, result, size, NULL, NULL);
done:
    WindowsDeleteString(text); return result;
}

static HRESULT receipt_decode( const char *text, SIZE_T length, BOOL url, BYTE **out, DWORD *size )
{
    SIZE_T i, padded, padding = 0;
    char *copy;
    HRESULT hr = E_UNEXPECTED;
    *out = NULL; *size = 0;
    if (!length || length > 64000 || length % 4 == 1) return hr;
    padded = (length + 3) & ~3;
    if (!(copy = calloc(padded + 1, 1))) return E_OUTOFMEMORY;
    for (i = 0; i < length; i++)
    {
        char c = text[i];
        if (padding && c != '=') goto done;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) copy[i] = c;
        else if (url && (c == '-' || c == '_')) copy[i] = c == '-' ? '+' : '/';
        else if (!url && (c == '+' || c == '/')) copy[i] = c;
        else if (!url && c == '=' && i >= length - 2 && length % 4 == 0) { copy[i] = c; padding++; }
        else goto done;
    }
    for (i = length; i < padded; i++) copy[i] = '=';
    /* This Wine base rejects CRYPT_STRING_STRICT. Validate alphabet, padding
     * and decoded length here instead of relaxing the input contract. */
    if (!CryptStringToBinaryA(copy, padded, CRYPT_STRING_BASE64, NULL, size, NULL, NULL)) goto done;
    if (*size != padded / 4 * 3 - (padding ? padding : padded - length)) goto done;
    if (!(*out = calloc(*size + 1, 1))) { hr = E_OUTOFMEMORY; goto done; }
    if (!CryptStringToBinaryA(copy, padded, CRYPT_STRING_BASE64, *out, size, NULL, NULL))
    { free(*out); *out = NULL; goto done; }
    hr = S_OK;
done:
    free(copy); return hr;
}

/* Fixed official hosts only, normal certificate/hostname verification, no
 * redirects or arbitrary token URLs. No Authorization or receipt is sent here. */
static HRESULT receipt_public_get( const WCHAR *host, const WCHAR *path, SIZE_T limit, BYTE **out, SIZE_T *length )
{
    HINTERNET session = NULL, connection = NULL, request = NULL;
    DWORD status, size = sizeof(status), available, read, redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    BYTE *buffer = NULL, *grown;
    ULONGLONG start = GetTickCount64();
    HRESULT hr = E_FAIL;
    *out = NULL; *length = 0;
    if (wcscmp(host, L"licensing.mp.microsoft.com") && wcscmp(host, L"displaycatalog.mp.microsoft.com")) return E_INVALIDARG;
    if (!(session = WinHttpOpen(L"Xodus-Nexus-runtime/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0))) goto done;
    if (!WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000)) goto done;
    if (!(connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) ||
        !(request = WinHttpOpenRequest(connection, L"GET", path, NULL, NULL, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))) goto done;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL) || status != 200) goto done;
    do
    {
        if (GetTickCount64() - start > 20000 || !WinHttpQueryDataAvailable(request, &available)) goto done;
        if (!available) break;
        if (available > limit || *length > limit - available) { hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); goto done; }
        if (!(grown = realloc(buffer, *length + available + 1))) { hr = E_OUTOFMEMORY; goto done; }
        buffer = grown;
        if (!WinHttpReadData(request, buffer + *length, available, &read) || !read) goto done;
        *length += read; buffer[*length] = 0;
    } while (TRUE);
    if (!*length) goto done;
    *out = buffer; buffer = NULL; hr = S_OK;
done:
    free(buffer);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (FAILED(hr)) *length = 0;
    return hr;
}

static BOOL receipt_cert_id( const char *id )
{
    UINT32 i;
    if (!id || strlen(id) != 40) return FALSE;
    for (i = 0; i < 40; i++) if (!((id[i] >= 'A' && id[i] <= 'F') || (id[i] >= '0' && id[i] <= '9'))) return FALSE;
    return TRUE;
}

/* cert bytes must come from the fixed Microsoft endpoint in production. Tests
 * call this pure validator with their own synthetic key, never change trust. */
static HRESULT receipt_verify( const char *jwt, const char *nonce, const char *cert_id,
    const BYTE *certificate, DWORD certificate_size, IJsonObject **claim, UINT64 *expiry )
{
    const char *dot = strchr(jwt, '.'), *second;
    BYTE *header = NULL, *payload = NULL, *signature = NULL, *claim_bytes = NULL;
    DWORD header_size, payload_size, signature_size, claim_size;
    IJsonObject *head = NULL, *body = NULL, *data = NULL;
    char *alg = NULL, *issuer = NULL, *audience = NULL, *encoded = NULL, *challenge = NULL, *id = NULL;
    PCCERT_CONTEXT cert = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    BCRYPT_PKCS1_PADDING_INFO padding = { BCRYPT_SHA256_ALGORITHM };
    BYTE digest[32], thumbprint[20];
    DWORD thumbprint_size = sizeof(thumbprint);
    char hex[41];
    UINT32 i, stage = 0;
    double exp, nbf;
    UINT64 now = time(NULL);
    HRESULT hr = E_ACCESSDENIED;
    *claim = NULL;
    if (!dot || !(second = strchr(dot + 1, '.')) || strchr(second + 1, '.') || strlen(jwt) > 48000 || !receipt_cert_id(cert_id)) return hr;
    if (FAILED(receipt_decode(jwt, dot - jwt, TRUE, &header, &header_size)) ||
        FAILED(receipt_decode(dot + 1, second - dot - 1, TRUE, &payload, &payload_size)) ||
        FAILED(receipt_decode(second + 1, strlen(second + 1), TRUE, &signature, &signature_size)) ||
        FAILED(receipt_json(header, header_size, &head)) || FAILED(receipt_json(payload, payload_size, &body))) goto done;
    stage = 1;
    if (!(alg = receipt_string(head, L"alg", 16)) || strcmp(alg, "RS256")) goto done;
    stage = 2;
    if (!(cert = CertCreateCertificateContext(X509_ASN_ENCODING, certificate, certificate_size)) ||
        CertVerifyTimeValidity(NULL, cert->pCertInfo) ||
        !CryptHashCertificate(0, CALG_SHA1, 0, certificate, certificate_size, thumbprint, &thumbprint_size) || thumbprint_size != 20) goto done;
    stage = 3;
    for (i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02X", thumbprint[i]);
    if (strcmp(hex, cert_id) || !CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, &cert->pCertInfo->SubjectPublicKeyInfo, 0, NULL, &key) ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (BYTE *)jwt, second - jwt, digest, sizeof(digest)) ||
        BCryptVerifySignature(key, &padding, digest, sizeof(digest), signature, signature_size, BCRYPT_PAD_PKCS1)) goto done;
    stage = 4;
    issuer = receipt_string(body, L"iss", 128); audience = receipt_string(body, L"aud", 128);
    if (!issuer || strcmp(issuer, "LicensingFD") || !audience || strcmp(audience, "SDK/ISV") ||
        FAILED(receipt_get_number(body, L"exp", &exp)) || FAILED(receipt_get_number(body, L"nbf", &nbf)) ||
        !isfinite(exp) || !isfinite(nbf) || exp != floor(exp) || nbf != floor(nbf) ||
        nbf < 0 || nbf > now + 60 || exp <= now || exp > 9007199254740991.0 || exp <= nbf) goto done;
    stage = 5;
    encoded = receipt_string(body, L"LicenseTokenClaim", 64000);
    if (!encoded || FAILED(receipt_decode(encoded, strlen(encoded), FALSE, &claim_bytes, &claim_size))) goto done;
    /* Microsoft permits a short non-JSON prefix before the first '{'. */
    for (i = 0; i < claim_size && claim_bytes[i] != '{'; i++);
    if (i > 64 || i == claim_size || FAILED(receipt_json(claim_bytes + i, claim_size - i, &data))) goto done;
    challenge = receipt_string(data, L"customDeveloperString", 128); id = receipt_string(data, L"certificateId", 40);
    stage = 6;
    if (!challenge || strcmp(challenge, nonce) || !id || strcmp(id, cert_id)) goto done;
    *expiry = exp; *claim = data; data = NULL; hr = S_OK;
done:
    if (FAILED(hr)) TRACE("Store receipt verification failed at stage %u, hr=%08lx.\n", stage, hr);
    if (key) BCryptDestroyKey(key);
    if (cert) CertFreeCertificateContext(cert);
    if (head) IJsonObject_Release(head);
    if (body) IJsonObject_Release(body);
    if (data) IJsonObject_Release(data);
    free(header); free(payload); free(signature); free(claim_bytes);
    free(alg); free(issuer); free(audience); free(encoded); free(challenge); free(id);
    return hr;
}

static HRESULT receipt_load( const char *jwt, const char *nonce, IJsonObject **claim, UINT64 *expiry )
{
    const char *dot = strchr(jwt, '.'), *second;
    BYTE *payload = NULL, *decoded = NULL, *document = NULL, *der = NULL;
    DWORD payload_size, decoded_size, der_size;
    SIZE_T size;
    IJsonObject *body = NULL, *data = NULL, *cert_json = NULL;
    char *encoded = NULL, *id = NULL, *certificate = NULL;
    WCHAR path[160];
    UINT32 i;
    HRESULT hr = E_ACCESSDENIED;
    if (!dot || !(second = strchr(dot + 1, '.')) || strlen(jwt) > 48000) return hr;
    if (FAILED(receipt_decode(dot + 1, second - dot - 1, TRUE, &payload, &payload_size)) ||
        FAILED(receipt_json(payload, payload_size, &body))) goto done;
    encoded = receipt_string(body, L"LicenseTokenClaim", 64000);
    if (!encoded || FAILED(receipt_decode(encoded, strlen(encoded), FALSE, &decoded, &decoded_size))) goto done;
    for (i = 0; i < decoded_size && decoded[i] != '{'; i++);
    if (i > 64 || i == decoded_size || FAILED(receipt_json(decoded + i, decoded_size - i, &data))) goto done;
    id = receipt_string(data, L"certificateId", 40);
    if (!receipt_cert_id(id)) goto done;
    swprintf(path, ARRAY_SIZE(path), L"/v8.0/licenseToken/fullCertificate/%S", id);
    if (FAILED(hr = receipt_public_get(L"licensing.mp.microsoft.com", path, 64000, &document, &size))) goto done;
    /* The current official endpoint returns {certificate:base64-DER}. */
    hr = E_ACCESSDENIED;
    if (FAILED(receipt_json(document, size, &cert_json)) || !(certificate = receipt_string(cert_json, L"certificate", 32000)) ||
        FAILED(receipt_decode(certificate, strlen(certificate), FALSE, &der, &der_size))) goto done;
    hr = receipt_verify(jwt, nonce, id, der, der_size, claim, expiry);
done:
    if (body) IJsonObject_Release(body);
    if (data) IJsonObject_Release(data);
    if (cert_json) IJsonObject_Release(cert_json);
    free(payload); free(decoded); free(document); free(der); free(encoded); free(id); free(certificate);
    return hr;
}

static HRESULT receipt_date( const char *text, UINT64 *out )
{
    SYSTEMTIME st = {0};
    FILETIME ft;
    ULARGE_INTEGER ticks;
    unsigned year, month, day, hour, minute, second;
    int length = 0;
    const char *suffix;
    if (!text || sscanf(text, "%4u-%2u-%2uT%2u:%2u:%2u%n", &year, &month, &day, &hour, &minute, &second, &length) != 6) return E_UNEXPECTED;
    suffix = text + length;
    if (*suffix == '.') { suffix++; if (*suffix < '0' || *suffix > '9') return E_UNEXPECTED; while (*suffix >= '0' && *suffix <= '9') suffix++; }
    if (strcmp(suffix, "Z") && strcmp(suffix, "+00:00")) return E_UNEXPECTED;
    st.wYear = year; st.wMonth = month; st.wDay = day; st.wHour = hour; st.wMinute = minute; st.wSecond = second;
    if (!SystemTimeToFileTime(&st, &ft)) return E_UNEXPECTED;
    ticks.LowPart = ft.dwLowDateTime; ticks.HighPart = ft.dwHighDateTime;
    if (ticks.QuadPart < 116444736000000000ULL) return E_UNEXPECTED;
    *out = (ticks.QuadPart - 116444736000000000ULL) / 10000000;
    return S_OK;
}

struct receipt_license { char id[13], sku[5]; UINT64 expiry; };

static HRESULT receipt_inventory( IJsonObject *claim, const char **ids, UINT32 count,
    struct receipt_license *licenses, UINT32 *license_count, UINT64 *expiry )
{
    IJsonArray *array = NULL;
    UINT32 length, i, j;
    double version;
    HRESULT hr = E_UNEXPECTED;
    if (FAILED(receipt_get_number(claim, L"tokenVersion", &version)) || version != 1 ||
        FAILED(receipt_get_array(claim, L"licensableProducts", &array)) ||
        FAILED(receipt_array_size(array, &length)) || length > 512) goto done;
    for (i = 0; i < length; i++)
    {
        IJsonObject *row = NULL;
        char *product = NULL, *sku = NULL, *date = NULL;
        UINT64 until;
        hr = E_UNEXPECTED;
        if (FAILED(IJsonArray_GetObjectAt(array, i, &row))) goto done;
        product = receipt_string(row, L"productId", 12); sku = receipt_string(row, L"skuId", 4);
        date = receipt_string(row, L"endDate", 64);
        hr = E_UNEXPECTED;
        if (product && entitled_id(product) && entitled_sku_id(sku) && date && SUCCEEDED(receipt_date(date, &until)))
        {
            for (j = 0; j < count && strcmp(ids[j], product); j++);
            /* Never expose an unrelated product from a broader receipt. */
            if (j == count || until <= (UINT64)time(NULL)) hr = S_OK;
            else
            {
                for (j = 0; j < *license_count; j++)
                    if (!strcmp(licenses[j].id, product) && !strcmp(licenses[j].sku, sku)) break;
                if (j < *license_count)
                { if (until < licenses[j].expiry) licenses[j].expiry = until; hr = S_OK; }
                else if (*license_count < ENTITLED_MAX_PRODUCTS)
                {
                    memcpy(licenses[*license_count].id, product, 13);
                    memcpy(licenses[*license_count].sku, sku, 5);
                    licenses[(*license_count)++].expiry = until; hr = S_OK;
                }
                if (until < *expiry) *expiry = until;
            }
        }
        free(product); free(sku); free(date); IJsonObject_Release(row);
        if (FAILED(hr)) goto done;
    }
    hr = S_OK;
done:
    if (array) IJsonArray_Release(array);
    return hr;
}

static HRESULT receipt_metadata_parse( struct receipt_license *license, UINT32 kinds,
    const BYTE *bytes, SIZE_T size, xmlNodePtr root )
{
    IJsonObject *document = NULL, *product = NULL, *localized = NULL, *entry = NULL, *sku = NULL, *properties = NULL, *product_properties = NULL;
    IJsonArray *products = NULL, *locals = NULL, *skus = NULL;
    char *id = NULL, *type = NULL, *title = NULL, *lang = NULL, *sku_id = NULL, *offer = NULL;
    UINT32 kind = 0, count, i;
    boolean trial = FALSE;
    xmlNodePtr node, sku_node;
    char kind_text[8], expiry_text[32];
    HRESULT hr = E_UNEXPECTED;
    if (FAILED(receipt_json(bytes, size, &document)) || FAILED(receipt_get_array(document, L"Products", &products)) ||
        FAILED(receipt_array_size(products, &count)) || count != 1 || FAILED(IJsonArray_GetObjectAt(products, 0, &product))) goto done;
    id = receipt_string(product, L"ProductId", 12); type = receipt_string(product, L"ProductType", 32);
    if (!id || strcmp(id, license->id) || !type) goto done;
    if (!strcmp(type, "Consumable")) kind = 1;
    else if (!strcmp(type, "Durable")) kind = 2;
    else if (!strcmp(type, "Game")) kind = 4;
    else if (!strcmp(type, "Pass")) kind = 8;
    else if (!strcmp(type, "UnmanagedConsumable")) kind = 16;
    if (!kind) { hr = E_NOTIMPL; goto done; }
    if (!(kind & kinds)) { hr = S_OK; goto done; }
    if (SUCCEEDED(receipt_get_object(product, L"Properties", &product_properties)))
        offer = receipt_string(product_properties, L"InAppOfferToken", 256);
    if (FAILED(receipt_get_array(product, L"LocalizedProperties", &locals)) ||
        FAILED(IJsonArray_GetObjectAt(locals, 0, &localized)) ||
        !(title = receipt_string(localized, L"ProductTitle", 1024)) || !(lang = receipt_string(localized, L"Language", 35)) ||
        FAILED(receipt_get_array(product, L"DisplaySkuAvailabilities", &skus)) ||
        FAILED(receipt_array_size(skus, &count)) || count > 100) goto done;
    for (i = 0; i < count; i++)
    {
        hr = E_UNEXPECTED;
        if (FAILED(IJsonArray_GetObjectAt(skus, i, &entry))) goto done;
        if (FAILED(receipt_get_object(entry, L"Sku", &sku))) goto done;
        sku_id = receipt_string(sku, L"SkuId", 4);
        if (sku_id && !strcmp(sku_id, license->sku)) break;
        free(sku_id); sku_id = NULL;
        IJsonObject_Release(sku); sku = NULL; IJsonObject_Release(entry); entry = NULL;
    }
    hr = E_UNEXPECTED;
    if (!sku || !sku_id || strcmp(sku_id, license->sku)) goto done;
    {
        if (FAILED(receipt_get_object(sku, L"Properties", &properties))) goto done;
        /* Unknown trial state must not be advertised as a full SKU. */
        if (FAILED(receipt_get_boolean(properties, L"IsTrial", &trial))) goto done;
    }
    snprintf(kind_text, sizeof(kind_text), "%u", kind);
    for (node = root->children; node; node = node->next)
    {
        char *previous = entitled_text(node, "StoreId");
        BOOL match = previous && !strcmp(previous, id);
        free(previous);
        if (match) break;
    }
    if (!node)
    {
        if (!(node = xmlNewTextChild(root, NULL, BAD_CAST "Product", NULL))) { hr = E_OUTOFMEMORY; goto done; }
        xmlNewTextChild(node, NULL, BAD_CAST "StoreId", BAD_CAST id);
        xmlNewTextChild(node, NULL, BAD_CAST "Title", BAD_CAST title);
        xmlNewTextChild(node, NULL, BAD_CAST "Description", BAD_CAST "");
        xmlNewTextChild(node, NULL, BAD_CAST "Language", BAD_CAST lang);
        xmlNewTextChild(node, NULL, BAD_CAST "InAppOfferToken", BAD_CAST (offer ? offer : ""));
        xmlNewTextChild(node, NULL, BAD_CAST "ProductKind", BAD_CAST kind_text);
        xmlNewTextChild(node, NULL, BAD_CAST "HasDigitalDownload", BAD_CAST "false");
        xmlNewTextChild(node, NULL, BAD_CAST "IsInUserCollection", BAD_CAST "true");
    }
    /* A package exists only when public metadata advertises one. */
    {
        IJsonArray *packages = NULL;
        UINT32 packages_count = 0;
        if (SUCCEEDED(receipt_get_array(properties, L"Packages", &packages)))
        { receipt_array_size(packages, &packages_count); IJsonArray_Release(packages); }
        if (packages_count) xmlNodeSetContent(entitled_child(node, "HasDigitalDownload"), BAD_CAST "true");
    }
    sku_node = xmlNewTextChild(node, NULL, BAD_CAST "Sku", NULL);
    if (!sku_node) { hr = E_OUTOFMEMORY; goto done; }
    xmlNewTextChild(sku_node, NULL, BAD_CAST "SkuId", BAD_CAST license->sku);
    xmlNewTextChild(sku_node, NULL, BAD_CAST "Title", BAD_CAST title);
    xmlNewTextChild(sku_node, NULL, BAD_CAST "Description", BAD_CAST "");
    xmlNewTextChild(sku_node, NULL, BAD_CAST "Language", BAD_CAST lang);
    xmlNewTextChild(sku_node, NULL, BAD_CAST "IsTrial", BAD_CAST (trial ? "true" : "false"));
    xmlNewTextChild(sku_node, NULL, BAD_CAST "IsInUserCollection", BAD_CAST "true");
    snprintf(expiry_text, sizeof(expiry_text), "%llu", (unsigned long long)license->expiry);
    xmlNewTextChild(sku_node, NULL, BAD_CAST "EndDate", BAD_CAST expiry_text);
    hr = S_OK;
done:
    if (document) IJsonObject_Release(document);
    if (product) IJsonObject_Release(product);
    if (localized) IJsonObject_Release(localized);
    if (entry) IJsonObject_Release(entry);
    if (sku) IJsonObject_Release(sku);
    if (properties) IJsonObject_Release(properties);
    if (product_properties) IJsonObject_Release(product_properties);
    if (products) IJsonArray_Release(products);
    if (locals) IJsonArray_Release(locals);
    if (skus) IJsonArray_Release(skus);
    free(id); free(type); free(title); free(lang); free(sku_id); free(offer);
    return hr;
}

static HRESULT receipt_metadata( struct receipt_license *license, UINT32 kinds, const char *market,
    const char *language, xmlNodePtr root )
{
    WCHAR path[256];
    BYTE *bytes = NULL;
    SIZE_T size;
    HRESULT hr;
    swprintf(path, ARRAY_SIZE(path), L"/v7.0/products?bigIds=%S&market=%S&languages=%S", license->id, market, language);
    hr = receipt_public_get(L"displaycatalog.mp.microsoft.com", path, 4 * 1024 * 1024, &bytes, &size);
    if (SUCCEEDED(hr)) hr = receipt_metadata_parse(license, kinds, bytes, size, root);
    free(bytes); return hr;
}

static HRESULT entitled_fetch( const char *request, LONG *canceled, struct entitled_snapshot **out )
{
    xmlDocPtr doc = NULL, result = NULL;
    xmlNodePtr root, node, response_root;
    char *ids[ENTITLED_MAX_PRODUCTS] = {0}, *market = NULL, *language = NULL;
    struct receipt_license licenses[ENTITLED_MAX_PRODUCTS];
    UINT32 count = 0, license_count = 0, i, j, batch;
    UINT64 kinds, expiry = UINT64_MAX;
    ULONGLONG deadline = GetTickCount64() + 120000;
    char number[32];
    HRESULT hr = E_UNEXPECTED, com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    *out = NULL;
    doc = xmlReadMemory(request, strlen(request), NULL, NULL, XML_PARSE_NONET);
    if (!doc) goto done;
    root = xmlDocGetRootElement(doc);
    ids[count++] = entitled_text(root, "ParentProductId");
    for (node = root->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && !strcmp((char *)node->name, "ProductId"))
        { if (count == ENTITLED_MAX_PRODUCTS) goto done; ids[count++] = (char *)xmlNodeGetContent(node); }
    for (i = 0; i < count; i++) if (!entitled_id(ids[i])) goto done;
    market = entitled_text(root, "Market"); language = entitled_text(root, "Language");
    if (!market || strlen(market) != 2 || !language || strlen(language) > 35 ||
        FAILED(entitled_uint(root, "ProductKinds", &kinds))) goto done;
    for (i = 0; market[i]; i++) if (market[i] < 'A' || market[i] > 'Z') goto done;
    for (i = 0; language[i]; i++) if (!((language[i] >= 'A' && language[i] <= 'Z') ||
        (language[i] >= 'a' && language[i] <= 'z') || (language[i] >= '0' && language[i] <= '9') || language[i] == '-')) goto done;
    for (batch = 0; batch < count; batch += 64)
    {
        BYTE random[32];
        char nonce[65], *body = NULL, *jwt = NULL;
        IJsonObject *claim = NULL;
        UINT64 token_expiry = 0;
        if (InterlockedCompareExchange(canceled, 0, 0)) { hr = E_ABORT; goto done; }
        if (GetTickCount64() >= deadline) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); goto done; }
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) { hr = E_FAIL; goto done; }
        for (j = 0; j < 32; j++) sprintf(nonce + j * 2, "%02x", random[j]);
        hr = xodus_store_license_body((const char **)&ids[batch], min(64, count - batch), nonce, &body);
        if (SUCCEEDED(hr)) hr = xodus_store_license_xml(body, strlen(body), &jwt);
        free(body);
        if (SUCCEEDED(hr)) hr = receipt_load(jwt, nonce, &claim, &token_expiry);
        xodus_store_license_note("store-entitled-receipt", hr);
        if (jwt) { SecureZeroMemory(jwt, strlen(jwt)); free(jwt); }
        if (SUCCEEDED(hr))
        {
            if (token_expiry < expiry) expiry = token_expiry;
            hr = receipt_inventory(claim, (const char **)ids, count, licenses, &license_count, &expiry);
            xodus_store_license_note("store-entitled-inventory", hr);
        }
        if (claim) IJsonObject_Release(claim);
        if (FAILED(hr)) goto done;
    }
    if (!(result = xmlNewDoc(BAD_CAST "1.0"))) { hr = E_OUTOFMEMORY; goto done; }
    if (!(response_root = xmlNewNode(NULL, BAD_CAST "Response"))) { hr = E_OUTOFMEMORY; goto done; }
    xmlDocSetRootElement(result, response_root);
    xmlNewTextChild(response_root, NULL, BAD_CAST "HResult", BAD_CAST "0");
    for (i = 0; i < license_count; i++)
    {
        if (InterlockedCompareExchange(canceled, 0, 0)) { hr = E_ABORT; goto done; }
        if (GetTickCount64() >= deadline) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); goto done; }
        hr = receipt_metadata(&licenses[i], kinds, market, language, response_root);
        xodus_store_license_note("store-entitled-metadata", hr);
        if (FAILED(hr)) goto done;
    }
    if (expiry > (UINT64)time(NULL) + 60) expiry = (UINT64)time(NULL) + 60;
    snprintf(number, sizeof(number), "%llu", (unsigned long long)expiry);
    xmlNewTextChild(response_root, NULL, BAD_CAST "ValidUntil", BAD_CAST number);
    {
        xmlChar *bytes; int size;
        xmlDocDumpMemory(result, &bytes, &size);
        if (!bytes) { hr = E_OUTOFMEMORY; goto done; }
        hr = size <= 60000 ? entitled_parse((char *)bytes, size, out) : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        xmlFree(bytes);
    }
done:
    for (i = 0; i < count; i++) free(ids[i]);
    free(market); free(language);
    if (doc) xmlFreeDoc(doc);
    if (result) xmlFreeDoc(result);
    if (SUCCEEDED(com)) CoUninitialize();
    return hr;
}
