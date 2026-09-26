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

    if (sFailures == 0) printf("updater_parse_test: OK\n");
    return sFailures ? 1 : 0;
}
