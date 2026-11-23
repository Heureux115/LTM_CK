#include "server/services/password.h"
#include <sodium.h>
#include <string.h>

bool password_init(void) {
    if (sodium_init() < 0) {
        return false;
    }
    return true;
}

PasswordResult password_validate_input(const char *password, size_t length) {
    if (password == NULL) {
        return PASSWORD_INVALID_ARGUMENT;
    }
    if (length < CHAT_PASSWORD_MIN_BYTES) {
        return PASSWORD_TOO_SHORT;
    }
    if (length > CHAT_PASSWORD_MAX_BYTES) {
        return PASSWORD_TOO_LONG;
    }
    for (size_t i = 0; i < length; ++i) {
        if (password[i] == '\0') {
            return PASSWORD_CONTAINS_NUL;
        }
    }
    return PASSWORD_OK;
}

PasswordResult password_hash(const char *password, size_t length, char *hash_out, size_t hash_out_size) {
    if (password == NULL || hash_out == NULL) {
        return PASSWORD_INVALID_ARGUMENT;
    }
    if (hash_out_size < crypto_pwhash_STRBYTES) {
        return PASSWORD_INVALID_ARGUMENT;
    }
    PasswordResult validation = password_validate_input(password, length);
    if (validation != PASSWORD_OK) {
        return validation;
    }
    if (!password_init()) {
        return PASSWORD_INTERNAL_ERROR;
    }

    if (crypto_pwhash_str(
            hash_out,
            password,
            (unsigned long long)length,
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return PASSWORD_INTERNAL_ERROR;
    }

    return PASSWORD_OK;
}

PasswordResult password_verify(const char *hash, const char *password, size_t length) {
    if (hash == NULL || password == NULL) {
        return PASSWORD_INVALID_ARGUMENT;
    }
    if (strlen(hash) == 0) {
        return PASSWORD_VERIFY_FAILED;
    }
    PasswordResult validation = password_validate_input(password, length);
    if (validation != PASSWORD_OK) {
        return PASSWORD_VERIFY_FAILED;
    }
    if (!password_init()) {
        return PASSWORD_INTERNAL_ERROR;
    }

    if (crypto_pwhash_str_verify(hash, password, (unsigned long long)length) != 0) {
        return PASSWORD_VERIFY_FAILED;
    }

    return PASSWORD_OK;
}

void password_wipe(void *buffer, size_t length) {
    if (buffer != NULL && length > 0) {
        sodium_memzero(buffer, length);
    }
}

const char *password_get_dummy_hash(void) {
    static char dummy_hash[CHAT_PASSWORD_HASH_BYTES] = {0};
    static bool dummy_initialized = false;

    if (!dummy_initialized) {
        if (!password_init()) {
            return "";
        }
        if (password_hash("dummy_fallback_password", 23, dummy_hash, sizeof(dummy_hash)) == PASSWORD_OK) {
            dummy_initialized = true;
        }
    }
    return dummy_hash;
}

const char *password_result_to_string(PasswordResult result) {
    switch (result) {
        case PASSWORD_OK:
            return "Thao tác mật khẩu thành công";
        case PASSWORD_INVALID_ARGUMENT:
            return "Đối số mật khẩu không hợp lệ";
        case PASSWORD_TOO_SHORT:
            return "Mật khẩu quá ngắn (tối thiểu 8 ký tự)";
        case PASSWORD_TOO_LONG:
            return "Mật khẩu quá dài (tối đa 128 ký tự)";
        case PASSWORD_CONTAINS_NUL:
            return "Mật khẩu không được chứa ký tự NUL ('\\0')";
        case PASSWORD_VERIFY_FAILED:
            return "Mật khẩu hoặc chuỗi băm không khớp";
        case PASSWORD_INTERNAL_ERROR:
            return "Lỗi nội bộ thư viện mã hóa mật khẩu";
        default:
            return "Lỗi mật khẩu không xác định";
    }
}
