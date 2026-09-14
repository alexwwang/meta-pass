// tools/signing/keychain_keygen.c —— 在 macOS Keychain 生成 meta-pass 固件签名密钥对。
//
// 生成 EC P-256 密钥对存入登录 Keychain(标签 com.folotoy.meta-pass.signing),
// 导出公钥 SubjectPublicKeyInfo DER 到 stdout(由调用方转为 PEM 发布)。
// 私钥不出 Keychain;密钥首次被其他进程使用签名时 macOS 会弹一次授权框。
//
// 用法: keychain-keygen [--force] > public.der
//   已有同名密钥时拒绝覆盖;--force 先删旧钥再生成(旧钥签名的固件将全部失效)。
//
// 编译: cc -O2 -framework Security -framework CoreFoundation keychain_keygen.c -o keychain-keygen
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <string.h>

#define KEY_TAG "com.folotoy.meta-pass.signing"

// EC P-256 SubjectPublicKeyInfo 固定前缀(26B),后接 65B ANSI X9.63 未压缩点(0x04||X||Y)。
static const uint8_t SPKI_PREFIX[] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
    0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
    0x42, 0x00,
};

static CFDataRef key_tag_data(void)
{
    return CFDataCreate(NULL, (const UInt8 *)KEY_TAG, (CFIndex)strlen(KEY_TAG));
}

static CFMutableDictionaryRef base_query(CFDataRef tag)
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassKey);
    CFDictionarySetValue(q, kSecAttrApplicationTag, tag);
    CFDictionarySetValue(q, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    return q;
}

int main(int argc, char **argv)
{
    const bool force = (argc == 2 && strcmp(argv[1], "--force") == 0);
    if (argc > 2 || (argc == 2 && !force)) {
        fprintf(stderr, "usage: %s [--force] > public.der\n", argv[0]);
        return 2;
    }

    CFDataRef tag = key_tag_data();
    CFMutableDictionaryRef query = base_query(tag);
    CFDictionarySetValue(query, kSecReturnRef, kCFBooleanTrue);

    CFTypeRef existing = NULL;
    if (SecItemCopyMatching(query, &existing) == errSecSuccess) {
        if (!force) {
            fprintf(stderr, "error: key '%s' already exists (use --force to regenerate)\n",
                    KEY_TAG);
            return 1;
        }
        CFRelease(existing);
        CFMutableDictionaryRef del = base_query(tag);
        SecItemDelete(del);
        CFRelease(del);
        fprintf(stderr, "note: old key deleted\n");
    }
    CFRelease(query);

    // 生成密钥对:永久存入 Keychain。不带 AccessControl → 默认 ACL(创建者免提示)。
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    int bits = 256;
    CFNumberRef size = CFNumberCreate(NULL, kCFNumberIntType, &bits);
    CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, size);
    CFRelease(size);
    CFDictionarySetValue(attrs, kSecAttrIsPermanent, kCFBooleanTrue);
    CFDictionarySetValue(attrs, kSecAttrApplicationTag, tag);
    CFStringRef label = CFSTR("meta-pass firmware signing");
    CFDictionarySetValue(attrs, kSecAttrLabel, label);

    CFErrorRef err = NULL;
    SecKeyRef priv = SecKeyCreateRandomKey(attrs, &err);
    CFRelease(attrs);
    CFRelease(tag);
    if (!priv) {
        CFStringRef desc = CFErrorCopyDescription(err);
        char buf[256];
        CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
        fprintf(stderr, "error: SecKeyCreateRandomKey: %s\n", buf);
        CFRelease(desc);
        CFRelease(err);
        return 1;
    }

    SecKeyRef pub = SecKeyCopyPublicKey(priv);
    CFRelease(priv);
    if (!pub) {
        fprintf(stderr, "error: SecKeyCopyPublicKey failed\n");
        return 1;
    }
    CFDataRef raw = SecKeyCopyExternalRepresentation(pub, &err);
    CFRelease(pub);
    if (!raw || CFDataGetLength(raw) != 65 || CFDataGetBytePtr(raw)[0] != 0x04) {
        fprintf(stderr, "error: unexpected public key encoding\n");
        return 1;
    }

    // 拼 SPKI DER 输出(91 字节,与固件 meta_sign_pubkey.h 格式一致)。
    fwrite(SPKI_PREFIX, 1, sizeof(SPKI_PREFIX), stdout);
    fwrite(CFDataGetBytePtr(raw), 1, (size_t)CFDataGetLength(raw), stdout);
    CFRelease(raw);
    fprintf(stderr, "keypair generated in Keychain as '%s'\n", KEY_TAG);
    return 0;
}
