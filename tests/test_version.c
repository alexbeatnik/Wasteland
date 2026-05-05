#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* Copy of functions from main.c so we don't pull in SDL dependencies */
static int version_newer_than(const char *a, const char *b)
{
    int amaj = 0, amin = 0, apatch = 0;
    int bmaj = 0, bmin = 0, bpatch = 0;
    const char *ap = a;
    const char *bp = b;
    while (*ap && (*ap < '0' || *ap > '9')) ap++;
    while (*bp && (*bp < '0' || *bp > '9')) bp++;
    sscanf(ap, "%d.%d.%d", &amaj, &amin, &apatch);
    sscanf(bp, "%d.%d.%d", &bmaj, &bmin, &bpatch);
    if (amaj != bmaj) return amaj > bmaj;
    if (amin != bmin) return amin > bmin;
    return apatch > bpatch;
}

static void build_update_filename(const char *version, char *fname, size_t fsize)
{
#ifdef _WIN32
    snprintf(fname, fsize, "Wasteland-windows.exe");
#elif defined(__APPLE__)
    snprintf(fname, fsize, "Wasteland-macos.dmg");
#elif defined(__linux__)
#   if defined(__x86_64__)
    snprintf(fname, fsize, "wasteland_%s_amd64.deb", version);
#   elif defined(__aarch64__) || defined(__arm64__)
    snprintf(fname, fsize, "wasteland_%s_arm64.deb", version);
#   else
    snprintf(fname, fsize, "wasteland_%s.deb", version);
#   endif
#else
    fname[0] = '\0';
#endif
}

/* ------------------------------------------------------------------------- */
/* version_newer_than tests                                                  */
/* ------------------------------------------------------------------------- */

static void test_same_version(void) {
    ASSERT_FALSE(version_newer_than("0.4", "0.4"));
    ASSERT_FALSE(version_newer_than("1.2.3", "1.2.3"));
}

static void test_older_version(void) {
    ASSERT_FALSE(version_newer_than("0.3", "0.4"));
    ASSERT_FALSE(version_newer_than("0.3.0", "0.4.0"));
    ASSERT_FALSE(version_newer_than("1.0", "2.0"));
}

static void test_newer_major(void) {
    ASSERT_TRUE(version_newer_than("1.0", "0.9"));
    ASSERT_TRUE(version_newer_than("2.0", "1.99"));
}

static void test_newer_minor(void) {
    ASSERT_TRUE(version_newer_than("0.4", "0.3"));
    ASSERT_TRUE(version_newer_than("0.10", "0.9"));
}

static void test_newer_patch(void) {
    ASSERT_TRUE(version_newer_than("0.4.1", "0.4.0"));
    ASSERT_TRUE(version_newer_than("1.2.3", "1.2.2"));
}

static void test_with_v_prefix(void) {
    ASSERT_TRUE(version_newer_than("v0.4", "v0.3"));
    ASSERT_FALSE(version_newer_than("v0.3", "v0.4"));
}

static void test_mixed_prefixes(void) {
    ASSERT_TRUE(version_newer_than("0.4", "v0.3"));
    ASSERT_TRUE(version_newer_than("v0.4", "0.3"));
}

static void test_zero_versions(void) {
    ASSERT_FALSE(version_newer_than("0.0", "0.0"));
    ASSERT_TRUE(version_newer_than("0.1", "0.0"));
}

/* Real GitHub `tag_name` shape (`v0.5`) vs runtime `WASTELAND_VERSION`
 * (`0.5`) — the canonical comparison Wasteland actually performs. */
static void test_release_tag_vs_runtime(void) {
    /* Same release: no banner. */
    ASSERT_FALSE(version_newer_than("v0.5", "0.5"));
    /* Newer tag on GitHub: banner shows. */
    ASSERT_TRUE(version_newer_than("v0.6", "0.5"));
    /* Older tag (rolled back release): no banner. */
    ASSERT_FALSE(version_newer_than("v0.4", "0.5"));
}

/* "X.Y" without patch must be treated as patch=0 — both args. */
static void test_two_component_versions(void) {
    ASSERT_FALSE(version_newer_than("1.0", "1.0.0"));
    ASSERT_FALSE(version_newer_than("1.0.0", "1.0"));
    ASSERT_TRUE(version_newer_than("1.0.1", "1.0"));
}

/* Multi-digit minor versions: lexicographic ordering would say "0.9" > "0.10",
 * but numeric ordering says the opposite. We sscanf as %d, so numeric wins. */
static void test_multi_digit_minor(void) {
    ASSERT_TRUE(version_newer_than("0.10", "0.9"));
    ASSERT_TRUE(version_newer_than("1.20.0", "1.3.0"));
    ASSERT_FALSE(version_newer_than("0.9", "0.10"));
}

/* Empty / garbage input must not crash and must not falsely report "newer". */
static void test_empty_strings(void) {
    /* Both empty → both parse as 0.0.0 → not newer. */
    ASSERT_FALSE(version_newer_than("", ""));
    /* Empty vs real → 0.0.0 vs 0.5.0 → not newer. */
    ASSERT_FALSE(version_newer_than("", "0.5"));
}

static void test_garbage_prefix(void) {
    /* "release-1.2" should still extract 1.2 from the numeric tail. */
    ASSERT_TRUE(version_newer_than("release-1.2", "release-1.1"));
    ASSERT_FALSE(version_newer_than("release-1.0", "release-1.1"));
}

/* ------------------------------------------------------------------------- */
/* build_update_filename tests (platform-specific)                           */
/* ------------------------------------------------------------------------- */

/* Different versions produce different filenames on Linux (version is part
 * of the .deb name). On macOS / Windows the filename is version-agnostic. */
static void test_build_update_filename_versions_differ(void) {
    char a[256], b[256];
    build_update_filename("0.5", a, sizeof(a));
    build_update_filename("0.6", b, sizeof(b));
    ASSERT_TRUE(a[0] != '\0');
    ASSERT_TRUE(b[0] != '\0');
#if defined(__linux__)
    /* Linux .deb name includes the version → strings must differ. */
    ASSERT_FALSE(strcmp(a, b) == 0);
#else
    /* macOS / Windows: filename is the same regardless of version. */
    ASSERT_TRUE(strcmp(a, b) == 0);
#endif
}

static void test_build_update_filename(void) {
    char fname[256];
    build_update_filename("0.5", fname, sizeof(fname));
    ASSERT_TRUE(fname[0] != '\0');
#ifdef _WIN32
    ASSERT_EQ_STR("Wasteland-windows.exe", fname);
#elif defined(__APPLE__)
    ASSERT_EQ_STR("Wasteland-macos.dmg", fname);
#elif defined(__linux__)
#   if defined(__x86_64__)
    ASSERT_EQ_STR("wasteland_0.5_amd64.deb", fname);
#   elif defined(__aarch64__) || defined(__arm64__)
    ASSERT_EQ_STR("wasteland_0.5_arm64.deb", fname);
#   else
    ASSERT_TRUE(strstr(fname, "wasteland_0.5") != NULL);
#   endif
#else
    ASSERT_TRUE(fname[0] == '\0');
#endif
}

/* ------------------------------------------------------------------------- */
/* Suite runner                                                              */
/* ------------------------------------------------------------------------- */

void run_version(void) {
    printf("version_newer_than\n");
    RUN_TEST(test_same_version);
    RUN_TEST(test_older_version);
    RUN_TEST(test_newer_major);
    RUN_TEST(test_newer_minor);
    RUN_TEST(test_newer_patch);
    RUN_TEST(test_with_v_prefix);
    RUN_TEST(test_mixed_prefixes);
    RUN_TEST(test_zero_versions);
    RUN_TEST(test_release_tag_vs_runtime);
    RUN_TEST(test_two_component_versions);
    RUN_TEST(test_multi_digit_minor);
    RUN_TEST(test_empty_strings);
    RUN_TEST(test_garbage_prefix);

    printf("\nbuild_update_filename\n");
    RUN_TEST(test_build_update_filename);
    RUN_TEST(test_build_update_filename_versions_differ);
}

TEST_MAIN(version);
