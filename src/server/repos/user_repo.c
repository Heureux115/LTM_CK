#include "server/repos/user_repo.h"
#include <stdio.h>
#include <string.h>

bool user_repo_validate_username(const char *username) {
    if (username == NULL) {
        return false;
    }
    size_t len = strlen(username);
    if (len < CHAT_USERNAME_MIN_LEN || len > CHAT_USERNAME_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = username[i];
        bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool is_digit = (c >= '0' && c <= '9');
        bool is_underscore = (c == '_');
        if (!is_alpha && !is_digit && !is_underscore) {
            return false;
        }
    }
    return true;
}

bool user_repo_canonicalize_username(const char *input, char *out_canonical, size_t out_size) {
    if (input == NULL || out_canonical == NULL) {
        return false;
    }
    if (!user_repo_validate_username(input)) {
        return false;
    }
    size_t len = strlen(input);
    if (out_size < len + 1) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') {
            out_canonical[i] = (char)(c + ('a' - 'A'));
        } else {
            out_canonical[i] = c;
        }
    }
    out_canonical[len] = '\0';
    return true;
}

UserRepoResult user_repo_create(Database *db,
                                const char *raw_username,
                                const char *password_hash,
                                int64_t created_at,
                                int64_t *out_user_id) {
    if (db == NULL || db->handle == NULL || raw_username == NULL || password_hash == NULL || created_at < 0) {
        return USER_REPO_INVALID_ARGUMENT;
    }
    if (strlen(password_hash) == 0) {
        return USER_REPO_INVALID_ARGUMENT;
    }

    char canonical[CHAT_USERNAME_MAX_LEN + 1];
    if (!user_repo_canonicalize_username(raw_username, canonical, sizeof(canonical))) {
        return USER_REPO_INVALID_USERNAME;
    }

    const char *sql = "INSERT INTO users (username, password_hash, created_at, last_seen_at) "
                      "VALUES (?, ?, ?, NULL);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return USER_REPO_DB_ERROR;
    }

    sqlite3_bind_text(stmt, 1, canonical, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, created_at);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        if (out_user_id != NULL) {
            *out_user_id = sqlite3_last_insert_rowid(db->handle);
        }
        sqlite3_finalize(stmt);
        return USER_REPO_OK;
    }

    int extended_rc = sqlite3_extended_errcode(db->handle);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT && (extended_rc == SQLITE_CONSTRAINT_UNIQUE ||
                                    extended_rc == SQLITE_CONSTRAINT_PRIMARYKEY)) {
        return USER_REPO_USERNAME_TAKEN;
    }

    return USER_REPO_DB_ERROR;
}

static void populate_user_record(sqlite3_stmt *stmt, UserRecord *out_user) {
    if (stmt == NULL || out_user == NULL) {
        return;
    }
    out_user->user_id = sqlite3_column_int64(stmt, 0);

    const unsigned char *uname = sqlite3_column_text(stmt, 1);
    if (uname != NULL) {
        snprintf(out_user->username, sizeof(out_user->username), "%s", (const char *)uname);
    } else {
        out_user->username[0] = '\0';
    }

    const unsigned char *phash = sqlite3_column_text(stmt, 2);
    if (phash != NULL) {
        snprintf(out_user->password_hash, sizeof(out_user->password_hash), "%s", (const char *)phash);
    } else {
        out_user->password_hash[0] = '\0';
    }

    out_user->created_at = sqlite3_column_int64(stmt, 3);

    if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
        out_user->has_last_seen = false;
        out_user->last_seen_at = -1;
    } else {
        out_user->has_last_seen = true;
        out_user->last_seen_at = sqlite3_column_int64(stmt, 4);
    }
}

UserRepoResult user_repo_find_by_username(Database *db,
                                          const char *raw_username,
                                          UserRecord *out_user) {
    if (db == NULL || db->handle == NULL || raw_username == NULL || out_user == NULL) {
        return USER_REPO_INVALID_ARGUMENT;
    }

    char canonical[CHAT_USERNAME_MAX_LEN + 1];
    if (!user_repo_canonicalize_username(raw_username, canonical, sizeof(canonical))) {
        return USER_REPO_INVALID_USERNAME;
    }

    const char *sql = "SELECT user_id, username, password_hash, created_at, last_seen_at "
                      "FROM users WHERE username = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return USER_REPO_DB_ERROR;
    }

    sqlite3_bind_text(stmt, 1, canonical, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        populate_user_record(stmt, out_user);
        sqlite3_finalize(stmt);
        return USER_REPO_OK;
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return USER_REPO_NOT_FOUND;
    }

    sqlite3_finalize(stmt);
    return USER_REPO_DB_ERROR;
}

UserRepoResult user_repo_find_by_id(Database *db,
                                    int64_t user_id,
                                    UserRecord *out_user) {
    if (db == NULL || db->handle == NULL || user_id <= 0 || out_user == NULL) {
        return USER_REPO_INVALID_ARGUMENT;
    }

    const char *sql = "SELECT user_id, username, password_hash, created_at, last_seen_at "
                      "FROM users WHERE user_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return USER_REPO_DB_ERROR;
    }

    sqlite3_bind_int64(stmt, 1, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        populate_user_record(stmt, out_user);
        sqlite3_finalize(stmt);
        return USER_REPO_OK;
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return USER_REPO_NOT_FOUND;
    }

    sqlite3_finalize(stmt);
    return USER_REPO_DB_ERROR;
}

UserRepoResult user_repo_update_last_seen(Database *db,
                                          int64_t user_id,
                                          int64_t last_seen_at) {
    if (db == NULL || db->handle == NULL || user_id <= 0 || last_seen_at < 0) {
        return USER_REPO_INVALID_ARGUMENT;
    }

    const char *sql = "UPDATE users SET last_seen_at = ? WHERE user_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return USER_REPO_DB_ERROR;
    }

    sqlite3_bind_int64(stmt, 1, last_seen_at);
    sqlite3_bind_int64(stmt, 2, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        int changes = sqlite3_changes(db->handle);
        sqlite3_finalize(stmt);
        if (changes == 0) {
            return USER_REPO_NOT_FOUND;
        }
        return USER_REPO_OK;
    }

    sqlite3_finalize(stmt);
    return USER_REPO_DB_ERROR;
}

UserRepoResult user_repo_count(Database *db, int64_t *out_count) {
    if (db == NULL || db->handle == NULL || out_count == NULL) {
        return USER_REPO_INVALID_ARGUMENT;
    }

    const char *sql = "SELECT COUNT(*) FROM users;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return USER_REPO_DB_ERROR;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return USER_REPO_OK;
    }

    sqlite3_finalize(stmt);
    return USER_REPO_DB_ERROR;
}

const char *user_repo_result_to_string(UserRepoResult result) {
    switch (result) {
        case USER_REPO_OK:
            return "Thao tác repository người dùng thành công";
        case USER_REPO_INVALID_ARGUMENT:
            return "Đối số repository không hợp lệ";
        case USER_REPO_INVALID_USERNAME:
            return "Tên người dùng không hợp lệ (3–32 ký tự alphanumeric/underscore)";
        case USER_REPO_USERNAME_TAKEN:
            return "Tên người dùng đã được đăng ký";
        case USER_REPO_NOT_FOUND:
            return "Không tìm thấy người dùng";
        case USER_REPO_DB_ERROR:
            return "Lỗi cơ sở dữ liệu khi thao tác repository";
        default:
            return "Lỗi repository người dùng không xác định";
    }
}
