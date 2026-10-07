#include <stdio.h>
#include <string.h>

#include "port_updater_parse.h"

static int sFailures;

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); sFailures++; } } while (0)

/* Newest first, like the GitHub API: a beta, a stable, an older stable. */
static const char kJson[] =
    "[{\"tag_name\":\"v0.7.0\",\"prerelease\":true,\"assets\":["
    "{\"name\":\"a.zip\",\"browser_download_url\":\"http://h/a.zip\"},"
    "{\"name\":\"mzm.cia\",\"browser_download_url\":\"http://h/beta.cia\"}]},"
    "{\"tag_name\":\"v0.6.5\",\"prerelease\":false,\"assets\":["
    "{\"browser_download_url\":\"http://h/stable.cia\"}]},"
    "{\"tag_name\":\"v0.6.4\",\"prerelease\": false,\"assets\":["
    "{\"browser_download_url\":\"http://h/old.cia\"}]}]";

/* Bodies as GitHub returns them: markdown JSON-escaped, notes block between
 * the markers, the generated changelog after it. */
static const char kNotesJson[] =
    "[{\"tag_name\":\"v0.7.2\",\"prerelease\":true,\"assets\":[],"
    "\"body\":\"> **Beta build.**\\n\\n<!-- mzm-notes -->\\n## What's new\\n\\n"
    "- **Fixed** the `SETTINGS` button\\r\\n* Caf\\u00e9 \\\"quoted\\\" text\\n"
    "<!-- /mzm-notes -->\\n\\n## Changelog\\n- fix: something (abc)\"},"
    "{\"tag_name\":\"v0.7.1\",\"prerelease\":false,\"assets\":[],"
    "\"body\":\"<!-- mzm-notes -->\\n- Stable change\\n<!-- /mzm-notes -->\"},"
    "{\"tag_name\":\"v0.7.0\",\"prerelease\":false,\"assets\":[],\"body\":\"no block here\"},"
    "{\"tag_name\":\"v0.6.9\",\"prerelease\":false,\"assets\":[],\"body\":null}]";

static void TestNotes(void) {
    char out[1024];
    int n;

    /* From v0.7.0 with betas: the two newer releases, newest first. */
    n = Updater_CollectNotes(kNotesJson, true, "v0.7.0", out, sizeof(out));
    CHECK(n == 2);
    CHECK(strncmp(out, "== v0.7.2 ==\n", 13) == 0);
    CHECK(strstr(out, "- Fixed the SETTINGS button\n") != NULL);
    CHECK(strstr(out, "- Caf\xc3\xa9 \"quoted\" text\n") != NULL);
    CHECK(strstr(out, "What's new") == NULL);        /* heading dropped */
    CHECK(strstr(out, "Changelog") == NULL);         /* outside the block */
    CHECK(strstr(out, "Beta build") == NULL);
    CHECK(strstr(out, "\n== v0.7.1 ==\n- Stable change\n") != NULL);
    CHECK(strstr(out, "v0.7.0") == NULL);            /* not newer than current */

    /* Stable channel skips the beta. */
    n = Updater_CollectNotes(kNotesJson, false, "v0.7.0", out, sizeof(out));
    CHECK(n == 1);
    CHECK(strstr(out, "v0.7.2") == NULL);

    /* A far older install sees everything newer, and the releases without a
     * block say so instead of vanishing. */
    n = Updater_CollectNotes(kNotesJson, true, "v0.6.4", out, sizeof(out));
    CHECK(n == 4);
    CHECK(strstr(out, "== v0.7.0 ==\n- (no notes for this version)\n") != NULL);
    CHECK(strstr(out, "== v0.6.9 ==\n- (no notes for this version)\n") != NULL);

    /* A -dev build sits before its own X.Y.Z. */
    n = Updater_CollectNotes(kNotesJson, true, "v0.7.1-dev.3+abc", out, sizeof(out));
    CHECK(n == 2);

    /* Nothing newer, junk, empty. */
    CHECK(Updater_CollectNotes(kNotesJson, true, "v0.7.2", out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(Updater_CollectNotes("[]", true, "v0.1.0", out, sizeof(out)) == 0);
    CHECK(Updater_CollectNotes("{\"message\":\"rate limited\"}", true, "v0.1.0", out, sizeof(out)) == 0);

    /* Tiny buffer: truncated at a line boundary, still terminated. */
    n = Updater_CollectNotes(kNotesJson, true, "v0.6.4", out, 40);
    CHECK(strlen(out) < 40);
    CHECK(Updater_CollectNotes(kNotesJson, true, "v0.6.4", out, 0) == 0);

    /* Truncated JSON mid-body: the block so far is still used. */
    n = Updater_CollectNotes("[{\"tag_name\":\"v1.0.0\",\"body\":\"<!-- mzm-notes -->\\n- Part", true, "v0.9.0", out, sizeof(out));
    CHECK(n == 1);
    CHECK(strstr(out, "- Part") != NULL);
}

int main(void) {
    UpdaterRelease rel;

    CHECK(Updater_IsNewer("v0.6.4", "v0.6.5"));
    CHECK(!Updater_IsNewer("v0.6.5", "v0.6.5"));
    CHECK(!Updater_IsNewer("v0.6.5", "v0.6.4"));
    CHECK(Updater_IsNewer("v0.6.9", "v0.7.0"));
    CHECK(Updater_IsNewer("v0.6.4-dev.3+abc", "v0.6.4"));
    CHECK(!Updater_IsNewer("v0.6.4", "v0.6.4-dev.3+abc"));
    CHECK(Updater_IsNewer("v0.6.4-dev.3+abc", "v0.6.5"));
    CHECK(!Updater_IsNewer("garbage", "v0.6.5"));
    CHECK(!Updater_IsNewer("v0.6.4", ""));

    CHECK(Updater_PickRelease(kJson, false, &rel));
    CHECK(strcmp(rel.tag, "v0.6.5") == 0);
    CHECK(strcmp(rel.cia_url, "http://h/stable.cia") == 0);
    CHECK(!rel.prerelease);

    CHECK(Updater_PickRelease(kJson, true, &rel));
    CHECK(strcmp(rel.tag, "v0.7.0") == 0);
    CHECK(strcmp(rel.cia_url, "http://h/beta.cia") == 0);
    CHECK(rel.prerelease);

    CHECK(!Updater_PickRelease("[]", true, &rel));
    CHECK(!Updater_PickRelease("{\"message\":\"rate limited\"}", true, &rel));
    /* Truncated mid-URL: the incomplete release must be skipped, not garbled. */
    CHECK(!Updater_PickRelease("[{\"tag_name\":\"v1.0.0\",\"assets\":[{\"browser_download_url\":\"http://h/x.ci", true, &rel));

    TestNotes();

    if (sFailures == 0) printf("updater_parse_test: OK\n");
    return sFailures ? 1 : 0;
}
