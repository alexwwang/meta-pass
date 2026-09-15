// tools/signing/extract-pubkey.c —— 从 Keychain 导出已存在的 EC 签名密钥的公钥 DER。
// 用法: extract-pubkey > public.pem
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <string.h>

#define KEY_TAG "com.folotoy.meta-pass.signing"

int main(void) {
    CFDataRef tag = CFDataCreate(NULL, (const UInt8 *)KEY_TAG, (CFIndex)strlen(KEY_TAG));
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassKey);
    CFDictionarySetValue(query, kSecAttrApplicationTag, tag);
    CFDictionarySetValue(query, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(query, kSecAttrKeyClass, kSecAttrKeyClassPublic);
    CFDictionarySetValue(query, kSecReturnRef, kCFBooleanTrue);

    CFTypeRef item = NULL;
    OSStatus st = SecItemCopyMatching(query, &item);
    CFRelease(query); CFRelease(tag);
    if (st != errSecSuccess) {
        fprintf(stderr, "error: public key not found (status %d)\n", (int)st);
        return 1;
    }
    SecKeyRef pub = (SecKeyRef)item;
    SecKeyRef priv = NULL;

    // 用公钥取标签 → 再找私钥 → 从私钥导出公钥（部分 Keychain 只返回私钥项）
    tag = CFDataCreate(NULL, (const UInt8 *)KEY_TAG, (CFIndex)strlen(KEY_TAG));
    CFMutableDictionaryRef pq = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(pq, kSecClass, kSecClassKey);
    CFDictionarySetValue(pq, kSecAttrApplicationTag, tag);
    CFDictionarySetValue(pq, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(pq, kSecAttrKeyClass, kSecAttrKeyClassPrivate);
    CFDictionarySetValue(pq, kSecReturnRef, kCFBooleanTrue);
    if (SecItemCopyMatching(pq, &item) == errSecSuccess) {
        priv = (SecKeyRef)item;
    }
    CFRelease(pq); CFRelease(tag);

    CFDataRef der = NULL;
    if (pub) {
        der = SecKeyCopyExternalRepresentation(pub, NULL);
        if (der && CFDataGetLength(der) > 0) {
            fwrite(CFDataGetBytePtr(der), 1, CFDataGetLength(der), stdout);
            CFRelease(der);
        }
        CFRelease(pub);
    }
    if (!der && priv) {
        // fallback: 从私钥推导公钥
        CFMutableDictionaryRef pquery = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(pquery, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
        CFDictionarySetValue(pquery, kSecReturnRef, kCFBooleanTrue);
        // 没有直接从 private 导出 public 的 API，输出错误提示
        fprintf(stderr, "error: could not extract public key from private key\n");
    }
    if (!der) {
        fprintf(stderr, "error: SecKeyCopyExternalRepresentation failed\n");
    }
    if (priv) CFRelease(priv);
    return der ? 0 : 1;
}
