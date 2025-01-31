// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <ctype.h>
#include <lib/boot-options/boot-options.h>
#include <lib/cmdline.h>
#include <lib/crypto/entropy/collector.h>
#include <lib/crypto/entropy/hw_rng_collector.h>
#include <lib/crypto/entropy/jitterentropy_collector.h>
#include <lib/crypto/entropy/quality_test.h>
#include <lib/crypto/global_prng.h>
#include <lib/crypto/prng.h>
#include <string.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <new>

#include <explicit-memory/bytes.h>
#include <fbl/algorithm.h>
#include <kernel/auto_lock.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <ktl/algorithm.h>
#include <ktl/string_view.h>
#include <lk/init.h>

// See note in //zircon/third_party/ulib/boringssl/BUILD.gn
#define BORINGSSL_NO_CXX
#include <openssl/sha.h>

#define LOCAL_TRACE 0

namespace crypto {

namespace GlobalPRNG {

static PRNG* kGlobalPrng = nullptr;

PRNG* GetInstance() {
  ASSERT(kGlobalPrng);
  return kGlobalPrng;
}

// Returns true if the kernel cmdline provided at least PRNG::kMinEntropy bytes
// of entropy, and false otherwise.
//
// TODO(security): Remove this in favor of virtio-rng once it is available and
// we decide we don't need it for getting entropy from elsewhere.
static bool IntegrateCmdlineEntropy() {
  ktl::string_view entropy{gBootOptions->entropy_mixin};
  if (entropy.empty()) {
    return false;
  }

  constexpr size_t kMaxEntropyArgumentLen = 128;
  const size_t hex_len = ktl::min(entropy.size(), kMaxEntropyArgumentLen);
  entropy = entropy.substr(hex_len);

  for (char c : entropy) {
    if (!isxdigit(c)) {
      panic("Invalid entropy string %.*s: '%c' is not an ASCII hex digit\n",
            static_cast<int>(entropy.size()), entropy.data(), c);
    }
  }

  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(entropy.data()), entropy.size(), digest);
  kGlobalPrng->AddEntropy(digest, sizeof(digest));

  const size_t entropy_added = ktl::max(hex_len / 2, sizeof(digest));
  LTRACEF("Collected %zu bytes of entropy from the kernel cmdline.\n", entropy_added);
  return (entropy_added >= PRNG::kMinEntropy);
}

// Returns true on success, false on failure.
static bool SeedFrom(entropy::Collector* collector) {
  uint8_t buf[PRNG::kMinEntropy] = {0};
  size_t remaining = collector->BytesNeeded(8 * PRNG::kMinEntropy);
#if LOCAL_TRACE
  {
    char name[ZX_MAX_NAME_LEN];
    collector->get_name(name, sizeof(name));
    LTRACEF("About to collect %zu bytes of entropy from '%s'.\n", remaining, name);
  }
#endif
  while (remaining > 0) {
    size_t result = collector->DrawEntropy(buf, ktl::min(sizeof(buf), remaining));
    if (result == 0) {
      LTRACEF(
          "Collected 0 bytes; aborting. "
          "There were %zu bytes remaining to collect.\n",
          remaining);
      return false;
    }

    kGlobalPrng->AddEntropy(buf, result);
    mandatory_memset(buf, 0, sizeof(buf));
    remaining -= result;
  }
  LTRACEF("Successfully collected entropy.\n");
  return true;
}

// Instantiates the global PRNG (in non-thread-safe mode) and seeds it.
static void EarlyBootSeed(uint level) {
  ASSERT(kGlobalPrng == nullptr);

  // Before doing anything else, test our entropy collector. This is
  // explicitly called here rather than in another init hook to ensure
  // ordering (at level LK_INIT_LEVEL_PLATFORM_EARLY + 1, but before the rest
  // of EarlyBootSeed).
  entropy::EarlyBootTest();

  // Statically allocate an array of bytes to put the PRNG into.  We do this
  // to control when the PRNG constructor is called.
  // TODO(security): This causes the PRNG state to be in a fairly predictable
  // place.  Some aspects of KASLR will help with this, but we may
  // additionally want to remap where this is later.
  alignas(alignof(PRNG)) static uint8_t prng_space[sizeof(PRNG)];
  kGlobalPrng = new (&prng_space) PRNG(nullptr, 0, PRNG::NonThreadSafeTag());

  unsigned int successful = 0;  // number of successful entropy sources
  entropy::Collector* collector = nullptr;
  if (entropy::HwRngCollector::GetInstance(&collector) == ZX_OK && SeedFrom(collector)) {
    successful++;
  } else if (gCmdline.GetBool(kernel_option::kCprngSeedRequireHwRng, false)) {
    panic("Failed to seed PRNG from required entropy source: hw-rng\n");
  }
  if (entropy::JitterentropyCollector::GetInstance(&collector) == ZX_OK && SeedFrom(collector)) {
    successful++;
  } else if (gCmdline.GetBool(kernel_option::kCprngSeedRequireJitterEntropy, false)) {
    panic("Failed to seed PRNG from required entropy source: jitterentropy\n");
  }

  if (IntegrateCmdlineEntropy()) {
    successful++;
  } else if (gCmdline.GetBool(kernel_option::kCprngSeedRequireCmdline, false)) {
    panic("Failed to seed PRNG from required entropy source: cmdline\n");
  }

  if (successful == 0) {
    printf(
        "WARNING: System has insufficient randomness.  It is completely "
        "unsafe to use this system for any cryptographic applications."
        "\n");
    // TODO(security): *CRITICAL* This is a fallback for systems without RNG
    // hardware that we should remove and attempt to do better.  If this
    // fallback is used, it breaks all cryptography used on the system.
    // *CRITICAL*
    uint8_t buf[PRNG::kMinEntropy] = {0};
    kGlobalPrng->AddEntropy(buf, sizeof(buf));
    return;
  } else {
    LTRACEF("Successfully collected entropy from %u sources.\n", successful);
  }
}

// Migrate the global PRNG to enter thread-safe mode.
static void BecomeThreadSafe(uint level) { GetInstance()->BecomeThreadSafe(); }

// PRNG reseeding loop.
static int ReseedPRNG(void* arg) {
  for (;;) {
    Thread::Current::SleepRelative(ZX_SEC(30));

    unsigned int successful = 0;  // number of successful entropy sources
    entropy::Collector* collector = nullptr;
    // Reseed using HW RNG and jitterentropy;
    if (entropy::HwRngCollector::GetInstance(&collector) == ZX_OK && SeedFrom(collector)) {
      successful++;
    } else if (gCmdline.GetBool(kernel_option::kCprngReseedRequireHwRng, false)) {
      panic("Failed to reseed PRNG from required entropy source: hw-rng\n");
    }
    if (entropy::JitterentropyCollector::GetInstance(&collector) == ZX_OK && SeedFrom(collector)) {
      successful++;
    } else if (gCmdline.GetBool(kernel_option::kCprngReseedRequireJitterEntropy, false)) {
      panic("Failed to reseed PRNG from required entropy source: jitterentropy\n");
    }

    if (successful == 0) {
      kGlobalPrng->SelfReseed();
      LTRACEF("Reseed PRNG with no new entropy source\n");
    } else {
      LTRACEF("Successfully reseed PRNG from %u sources.\n", successful);
    }
  }
  return 0;
}

// Start a thread to reseed PRNG.
static void StartReseedThread(uint level) {
  Thread* t = Thread::Create("prng-reseed", ReseedPRNG, nullptr, HIGHEST_PRIORITY);
  t->DetachAndResume();
}

}  // namespace GlobalPRNG

}  // namespace crypto

LK_INIT_HOOK(global_prng_seed, crypto::GlobalPRNG::EarlyBootSeed, LK_INIT_LEVEL_PLATFORM_EARLY + 1)

LK_INIT_HOOK(global_prng_thread_safe, crypto::GlobalPRNG::BecomeThreadSafe,
             LK_INIT_LEVEL_THREADING - 1)

LK_INIT_HOOK(global_prng_reseed, crypto::GlobalPRNG::StartReseedThread, LK_INIT_LEVEL_THREADING)
