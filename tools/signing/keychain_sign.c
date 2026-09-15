// tools/signing/keychain_sign.c —— 用 Keychain 中的 meta-pass 私钥对 32B 摘要做 ECDSA-P256 签名。
//
// 用法: keychain-sign <digest.bin> > signature.der
//   digest.bin 必须是恰好 32 字节的 SHA-256 摘要;
//   输出为 X9.62/DER 编码签名(64..72 字节),与固件 meta_sign.c 验签格式一致。
//   首次使用会弹 macOS Keychain 授权框(选"始终允许"则后续免提示)。
//
// 编译: cc -O2 -framework Security -framework CoreFoundation keychain_sign.c -o keychain-sign
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <string.h>

#define KEY_TAG "com.folotoy.meta-pass.signing"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <digest.bin> > signature.der\n", argv[0]);
        return 2;
    }

    uint8_t digest[32];
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(digest, 1, sizeof(digest), f) != sizeof(digest)) {
        fprintf(stderr, "error: %s must be a 32-byte SHA-256 digest\n", argv[1]);
        return 2;
    }
    fclose(f);

    CFDataRef tag = CFDataCreate(NULL, (const UInt8 *)KEY_TAG, (CFIndex)strlen(KEY_TAG));
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassKey);
    CFDictionarySetValue(query, kSecAttrApplicationTag, tag);
    CFDictionarySetValue(query, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(query, kSecAttrKeyClass, kSecAttrKeyClassPrivate);
    CFDictionarySetValue(query, kSecReturnRef, kCFBooleanTrue);
    CFTypeRef item = NULL;
    OSStatus status = SecItemCopyMatching(query, &item);
    CFRelease(query);
    CFRelease(tag);
    if (status != errSecSuccess) {
        fprintf(stderr, "error: key '%s' not found in Keychain — run keychain-keygen first\n",
                KEY_TAG);
        return 1;
    }
    SecKeyRef key = (SecKeyRef)item;

    CFDataRef digest_data = CFDataCreate(NULL, digest, sizeof(digest));
    CFErrorRef err = NULL;
    CFDataRef sig = SecKeyCreateSignature(key, kSecKeyAlgorithmECDSASignatureDigestX962SHA256,
                                          digest_data, &err);
    CFRelease(digest_data);
    CFRelease(key);
    if (!sig) {
        CFStringRef desc = err ? CFErrorCopyDescription(err) : NULL;
        char buf[256] = "unknown";
        if (desc) {
            CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(desc);
        }
        fprintf(stderr, "error: SecKeyCreateSignature: %s\n", buf);
        if (err) CFRelease(err);
        return 1;
    }

    fwrite(CFDataGetBytePtr(sig), 1, (size_t)CFDataGetLength(sig), stdout);
    CFRelease(sig);
    return 0;
}
