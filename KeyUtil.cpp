/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "KeyUtil.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <linux/fscrypt.h>
#include <openssl/sha.h>
#include <sys/ioctl.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <keyutils.h>

#include "KeyStorage.h"
#include "Utils.h"

namespace android {
namespace vold {

using android::fscrypt::EncryptionOptions;
using android::fscrypt::EncryptionPolicy;

// This must be acquired before calling fscrypt ioctls that operate on keys.
// This prevents race conditions between evicting and reinstalling keys.
static std::mutex fscrypt_keyring_mutex;

const KeyGeneration neverGen() {
    return KeyGeneration{0, false, false};
}

static bool randomKey(size_t size, KeyBuffer* key) {
    *key = KeyBuffer(size);
    if (ReadRandomBytes(key->size(), key->data()) != 0) {
        // TODO status_t plays badly with PLOG, fix it.
        LOG(ERROR) << "Random read failed";
        return false;
    }
    return true;
}

bool generateStorageKey(const KeyGeneration& gen, KeyBuffer* key) {
    if (!gen.allow_gen) {
        LOG(ERROR) << "Generating storage key not allowed";
        return false;
    }
    if (gen.use_hw_wrapped_key) {
        if (gen.keysize != FSCRYPT_MAX_KEY_SIZE) {
            LOG(ERROR) << "Cannot generate a wrapped key " << gen.keysize << " bytes long";
            return false;
        }
        LOG(DEBUG) << "Generating wrapped storage key";
        return generateWrappedStorageKey(key);
    } else {
        LOG(DEBUG) << "Generating standard storage key";
        return randomKey(gen.keysize, key);
    }
}

static bool isFsKeyringSupportedImpl() {
    android::base::unique_fd fd(open("/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC));

    // FS_IOC_ADD_ENCRYPTION_KEY with a NULL argument will fail with ENOTTY if
    // the ioctl isn't supported.  Otherwise it will fail with another error
    // code such as EFAULT.
    //
    // Note that there's no need to check for FS_IOC_REMOVE_ENCRYPTION_KEY,
    // since it's guaranteed to be available if FS_IOC_ADD_ENCRYPTION_KEY is.
    // There's also no need to check for support on external volumes separately
    // from /data, since either the kernel supports the ioctls on all
    // fscrypt-capable filesystems or it doesn't.
    errno = 0;
    (void)ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, NULL);
    if (errno == ENOTTY) {
        LOG(INFO) << "Kernel doesn't support FS_IOC_ADD_ENCRYPTION_KEY.  Falling back to "
                     "session keyring";
        return false;
    }
    if (errno != EFAULT) {
        PLOG(WARNING) << "Unexpected error from FS_IOC_ADD_ENCRYPTION_KEY";
    }
    LOG(DEBUG) << "Detected support for FS_IOC_ADD_ENCRYPTION_KEY";
    return true;
}

// Return true if the kernel supports the ioctls to add/remove fscrypt keys
// directly to/from the filesystem.
bool isFsKeyringSupported(void) {
    static bool supported = isFsKeyringSupportedImpl();
    return supported;
}

// Get raw keyref - used to make keyname and to pass to ioctl
static std::string generateKeyRef(const uint8_t* key, int length) {
    SHA512_CTX c;

    SHA512_Init(&c);
    SHA512_Update(&c, key, length);
    unsigned char key_ref1[SHA512_DIGEST_LENGTH];
    SHA512_Final(key_ref1, &c);

    SHA512_Init(&c);
    SHA512_Update(&c, key_ref1, SHA512_DIGEST_LENGTH);
    unsigned char key_ref2[SHA512_DIGEST_LENGTH];
    SHA512_Final(key_ref2, &c);

    static_assert(FSCRYPT_KEY_DESCRIPTOR_SIZE <= SHA512_DIGEST_LENGTH,
                  "Hash too short for descriptor");
    return std::string((char*)key_ref2, FSCRYPT_KEY_DESCRIPTOR_SIZE);
}

static bool fillKey(const KeyBuffer& key, fscrypt_key* fs_key) {
    if (key.size() != FSCRYPT_MAX_KEY_SIZE) {
        LOG(ERROR) << "Wrong size key " << key.size();
        return false;
    }
    static_assert(FSCRYPT_MAX_KEY_SIZE == sizeof(fs_key->raw), "Mismatch of max key sizes");
    fs_key->mode = 0;  // unused by kernel
    memcpy(fs_key->raw, key.data(), key.size());
    fs_key->size = key.size();
    return true;
}

static char const* const NAME_PREFIXES[] = {"ext4", "f2fs", "fscrypt", nullptr};

static std::string keyrefstring(const std::string& raw_ref) {
    std::ostringstream o;
    for (unsigned char i : raw_ref) {
        o << std::hex << std::setw(2) << std::setfill('0') << (int)i;
    }
    return o.str();
}

static std::string buildLegacyKeyName(const std::string& prefix, const std::string& raw_ref) {
    return prefix + ":" + keyrefstring(raw_ref);
}

// Get the ID of the keyring we store all fscrypt keys in when the kernel is too
// old to support FS_IOC_ADD_ENCRYPTION_KEY and FS_IOC_REMOVE_ENCRYPTION_KEY.
static bool fscryptKeyring(key_serial_t* device_keyring) {
    *device_keyring = keyctl_search(KEY_SPEC_SESSION_KEYRING, "keyring", "fscrypt", 0);
    if (*device_keyring == -1) {
        PLOG(ERROR) << "Unable to find device keyring";
        return false;
    }
    return true;
}

// Add an encryption key to the legacy global session keyring.
static bool installKeyLegacy(const KeyBuffer& key, const std::string& raw_ref) {
    // Place fscrypt_key into automatically zeroing buffer.
    KeyBuffer fsKeyBuffer(sizeof(fscrypt_key));
    fscrypt_key& fs_key = *reinterpret_cast<fscrypt_key*>(fsKeyBuffer.data());

    if (!fillKey(key, &fs_key)) return false;
    key_serial_t device_keyring;
    if (!fscryptKeyring(&device_keyring)) return false;
    for (char const* const* name_prefix = NAME_PREFIXES; *name_prefix != nullptr; name_prefix++) {
        auto ref = buildLegacyKeyName(*name_prefix, raw_ref);
        key_serial_t key_id =
            add_key("logon", ref.c_str(), (void*)&fs_key, sizeof(fs_key), device_keyring);
        if (key_id == -1) {
            PLOG(ERROR) << "Failed to insert key into keyring " << device_keyring;
            return false;
        }
        LOG(DEBUG) << "Added key " << key_id << " (" << ref << ") to keyring " << device_keyring
                   << " in process " << getpid();
    }
    return true;
}

// Build a struct fscrypt_key_specifier for use in the key management ioctls.
static bool buildKeySpecifier(fscrypt_key_specifier* spec, const EncryptionPolicy& policy) {
    switch (policy.options.version) {
        case 1:
            if (policy.key_raw_ref.size() != FSCRYPT_KEY_DESCRIPTOR_SIZE) {
                LOG(ERROR) << "Invalid key specifier size for v1 encryption policy: "
                           << policy.key_raw_ref.size();
                return false;
            }
            spec->type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
            memcpy(spec->u.descriptor, policy.key_raw_ref.c_str(), FSCRYPT_KEY_DESCRIPTOR_SIZE);
            return true;
        case 2:
            if (policy.key_raw_ref.size() != FSCRYPT_KEY_IDENTIFIER_SIZE) {
                LOG(ERROR) << "Invalid key specifier size for v2 encryption policy: "
                           << policy.key_raw_ref.size();
                return false;
            }
            spec->type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
            memcpy(spec->u.identifier, policy.key_raw_ref.c_str(), FSCRYPT_KEY_IDENTIFIER_SIZE);
            return true;
        default:
            LOG(ERROR) << "Invalid encryption policy version: " << policy.options.version;
            return false;
    }
}

bool installKey(const std::string& mountpoint, const EncryptionOptions& options,
                const KeyBuffer& key, EncryptionPolicy* policy) {
    const std::lock_guard<std::mutex> lock(fscrypt_keyring_mutex);
    policy->options = options;
    // Put the fscrypt_add_key_arg in an automatically-zeroing buffer, since we
    // have to copy the raw key into it.
    KeyBuffer arg_buf(sizeof(struct fscrypt_add_key_arg) + key.size(), 0);
    struct fscrypt_add_key_arg* arg = (struct fscrypt_add_key_arg*)arg_buf.data();

    // Initialize the "key specifier", which is like a name for the key.
    switch (options.version) {
        case 1:
            // A key for a v1 policy is specified by an arbitrary 8-byte
            // "descriptor", which must be provided by userspace.  We use the
            // first 8 bytes from the double SHA-512 of the key itself.
            if (options.use_hw_wrapped_key) {
                /* When wrapped key is supported, only the first 32 bytes are
                   the same per boot. The second 32 bytes can change as the ephemeral
                   key is different. */
                policy->key_raw_ref = generateKeyRef((const uint8_t*)key.data(), key.size()/2);
            } else {
            policy->key_raw_ref = generateKeyRef((const uint8_t*)key.data(), key.size());
            }
            if (!isFsKeyringSupported()) {
                return installKeyLegacy(key, policy->key_raw_ref);
            }
            if (!buildKeySpecifier(&arg->key_spec, *policy)) {
                return false;
            }
            break;
        case 2:
            // A key for a v2 policy is specified by an 16-byte "identifier",
            // which is a cryptographic hash of the key itself which the kernel
            // computes and returns.  Any user-provided value is ignored; we
            // just need to set the specifier type to indicate that we're adding
            // this type of key.
            arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
            break;
        default:
            LOG(ERROR) << "Invalid encryption policy version: " << options.version;
            return false;
    }

    if (options.use_hw_wrapped_key) arg->__flags |= __FSCRYPT_ADD_KEY_FLAG_HW_WRAPPED;
    // Provide the raw key.
    arg->raw_size = key.size();
    memcpy(arg->raw, key.data(), key.size());

    android::base::unique_fd fd(open(mountpoint.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << mountpoint << " to install key";
        return false;
    }

    if (ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, arg) != 0) {
        PLOG(ERROR) << "Failed to install fscrypt key to " << mountpoint;
        return false;
    }

    if (arg->key_spec.type == FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER) {
        // Retrieve the key identifier that the kernel computed.
        policy->key_raw_ref =
                std::string((char*)arg->key_spec.u.identifier, FSCRYPT_KEY_IDENTIFIER_SIZE);
    }
    LOG(DEBUG) << "Installed fscrypt key with ref " << keyrefstring(policy->key_raw_ref) << " to "
               << mountpoint;
    return true;
}

// Remove an encryption key from the legacy global session keyring.
static bool evictKeyLegacy(const std::string& raw_ref) {
    key_serial_t device_keyring;
    if (!fscryptKeyring(&device_keyring)) return false;
    bool success = true;
    for (char const* const* name_prefix = NAME_PREFIXES; *name_prefix != nullptr; name_prefix++) {
        auto ref = buildLegacyKeyName(*name_prefix, raw_ref);
        auto key_serial = keyctl_search(device_keyring, "logon", ref.c_str(), 0);

        // Unlink the key from the keyring.  Prefer unlinking to revoking or
        // invalidating, since unlinking is actually no less secure currently, and
        // it avoids bugs in certain kernel versions where the keyring key is
        // referenced from places it shouldn't be.
        if (keyctl_unlink(key_serial, device_keyring) != 0) {
            PLOG(ERROR) << "Failed to unlink key with serial " << key_serial << " ref " << ref;
            success = false;
        } else {
            LOG(DEBUG) << "Unlinked key with serial " << key_serial << " ref " << ref;
        }
    }
    return success;
}

static void waitForBusyFiles(const struct fscrypt_key_specifier key_spec, const std::string ref,
                             const std::string mountpoint) {
    android::base::unique_fd fd(open(mountpoint.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << mountpoint << " to evict key";
        return;
    }

    std::chrono::milliseconds wait_time(3200);
    std::chrono::milliseconds total_wait_time(0);
    while (wait_time <= std::chrono::milliseconds(51200)) {
        total_wait_time += wait_time;
        std::this_thread::sleep_for(wait_time);

        const std::lock_guard<std::mutex> lock(fscrypt_keyring_mutex);

        struct fscrypt_get_key_status_arg get_arg;
        memset(&get_arg, 0, sizeof(get_arg));
        get_arg.key_spec = key_spec;

        if (ioctl(fd, FS_IOC_GET_ENCRYPTION_KEY_STATUS, &get_arg) != 0) {
            PLOG(ERROR) << "Failed to get status for fscrypt key with ref " << ref << " from "
                        << mountpoint;
            return;
        }
        if (get_arg.status != FSCRYPT_KEY_STATUS_INCOMPLETELY_REMOVED) {
            LOG(DEBUG) << "Key status changed, cancelling busy file cleanup for key with ref "
                       << ref << ".";
            return;
        }

        struct fscrypt_remove_key_arg remove_arg;
        memset(&remove_arg, 0, sizeof(remove_arg));
        remove_arg.key_spec = key_spec;

        if (ioctl(fd, FS_IOC_REMOVE_ENCRYPTION_KEY, &remove_arg) != 0) {
            PLOG(ERROR) << "Failed to clean up busy files for fscrypt key with ref " << ref
                        << " from " << mountpoint;
            return;
        }
        if (remove_arg.removal_status_flags & FSCRYPT_KEY_REMOVAL_STATUS_FLAG_OTHER_USERS) {
            // Should never happen because keys are only added/removed as root.
            LOG(ERROR) << "Unexpected case: key with ref " << ref
                       << " is still added by other users!";
        } else if (!(remove_arg.removal_status_flags &
                     FSCRYPT_KEY_REMOVAL_STATUS_FLAG_FILES_BUSY)) {
            LOG(INFO) << "Successfully cleaned up busy files for key with ref " << ref
                      << ".  After waiting " << total_wait_time.count() << "ms.";
            return;
        }
        LOG(WARNING) << "Files still open after waiting " << total_wait_time.count()
                     << "ms.  Key with ref " << ref << " still has unlocked files!";
        wait_time *= 2;
    }
    LOG(ERROR) << "Waiting for files to close never completed.  Files using key with ref " << ref
               << " were not locked!";
}

bool evictKey(const std::string& mountpoint, const EncryptionPolicy& policy) {
    const std::lock_guard<std::mutex> lock(fscrypt_keyring_mutex);
    if (policy.options.version == 1 && !isFsKeyringSupported()) {
        return evictKeyLegacy(policy.key_raw_ref);
    }

    android::base::unique_fd fd(open(mountpoint.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << mountpoint << " to evict key";
        return false;
    }

    struct fscrypt_remove_key_arg arg;
    memset(&arg, 0, sizeof(arg));

    if (!buildKeySpecifier(&arg.key_spec, policy)) {
        return false;
    }

    std::string ref = keyrefstring(policy.key_raw_ref);

    if (ioctl(fd, FS_IOC_REMOVE_ENCRYPTION_KEY, &arg) != 0) {
        PLOG(ERROR) << "Failed to evict fscrypt key with ref " << ref << " from " << mountpoint;
        return false;
    }

    LOG(DEBUG) << "Evicted fscrypt key with ref " << ref << " from " << mountpoint;
    if (arg.removal_status_flags & FSCRYPT_KEY_REMOVAL_STATUS_FLAG_OTHER_USERS) {
        // Should never happen because keys are only added/removed as root.
        LOG(ERROR) << "Unexpected case: key with ref " << ref << " is still added by other users!";
    } else if (arg.removal_status_flags & FSCRYPT_KEY_REMOVAL_STATUS_FLAG_FILES_BUSY) {
        LOG(WARNING)
                << "Files still open after removing key with ref " << ref
                << ".  These files were not locked!  Punting busy file clean up to worker thread.";
        // Processes are killed asynchronously in ActivityManagerService due to performance issues
        // with synchronous kills.  If there were busy files they will probably be killed soon. Wait
        // for them asynchronously.
        std::thread busyFilesThread(waitForBusyFiles, arg.key_spec, ref, mountpoint);
        busyFilesThread.detach();
    }
    return true;
}

bool retrieveOrGenerateKey(const std::string& key_path, const std::string& tmp_path,
                           const KeyAuthentication& key_authentication, const KeyGeneration& gen,
                           KeyBuffer* key) {
    if (pathExists(key_path)) {
        LOG(DEBUG) << "Key exists, using: " << key_path;
        if (!retrieveKey(key_path, key_authentication, key)) return false;
    } else {
        if (!gen.allow_gen) {
            LOG(ERROR) << "No key found in " << key_path;
            return false;
        }
        LOG(INFO) << "Creating new key in " << key_path;
        if (!generateStorageKey(gen, key)) return false;
        if (!storeKeyAtomically(key_path, tmp_path, key_authentication, *key)) return false;
    }
    return true;
}

}  // namespace vold
}  // namespace android
