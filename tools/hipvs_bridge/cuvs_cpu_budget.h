/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Invalid, unlimited or unavailable quotas leave the caller's limit intact. */
static inline int cuvs_cpu_quota_limit(const char* quota, const char* period, int limit)
{
  char *end = NULL;
  if (!quota || !period || limit <= 0) return limit;
  errno = 0;
  const long long q = strtoll(quota, &end, 10);
  if (errno || end == quota || *end || q <= 0) return limit;
  errno = 0;
  const long long p = strtoll(period, &end, 10);
  if (errno || end == period || *end || p <= 0) return limit;
  const long long threads = q / p + (q % p != 0);
  return threads < limit ? (int)threads : limit;
}

static inline int cuvs_cpu_read_quota(const char* directory, int unified, int limit)
{
  char path[4096], quota[64], period[64], extra[2];
  int length = snprintf(path, sizeof(path), "%s/%s", directory,
                        unified ? "cpu.max" : "cpu.cfs_quota_us");
  if (length < 0 || (size_t)length >= sizeof(path)) return limit;
  FILE* file = fopen(path, "r");
  if (!file) return limit;
  int fields = unified ? fscanf(file, "%63s %63s %1s", quota, period, extra)
                       : fscanf(file, "%63s %1s", quota, extra);
  fclose(file);
  if (fields != (unified ? 2 : 1)) return limit;
  if (!unified) {
    length = snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return limit;
    file = fopen(path, "r");
    if (!file) return limit;
    fields = fscanf(file, "%63s %1s", period, extra);
    fclose(file);
    if (fields != 1) return limit;
  }
  return cuvs_cpu_quota_limit(quota, period, limit);
}

/* Check the process's cgroup and its visible parents, including a container's
 * namespaced mount root. Unsupported mount layouts simply retain the default. */
static inline int cuvs_cpu_walk_quotas(const char* root, const char* group, int unified, int limit)
{
  char path[4096];
  if (!root || !group || group[0] != '/' || strstr(group, "/../") ||
      strcmp(group, "/..") == 0) return limit;
  const size_t group_len = strlen(group);
  if (group_len >= 3 && strcmp(group + group_len - 3, "/..") == 0) return limit;
  const size_t root_len = strlen(root);
  int length = snprintf(path, sizeof(path), "%s%s", root, group);
  if (length < 0 || (size_t)length >= sizeof(path)) return cuvs_cpu_read_quota(root, unified, limit);
  while ((size_t)length > root_len && path[length - 1] == '/') path[--length] = 0;
  for (;;) {
    limit = cuvs_cpu_read_quota(path, unified, limit);
    if ((size_t)length <= root_len) break;
    char* slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) < root_len) break;
    *slash = 0;
    length = (int)(slash - path);
  }
  return limit;
}

static inline int cuvs_cpu_container_limit(int limit)
{
#ifdef __linux__
  const char* v1_roots[] = {"/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct",
                          "/sys/fs/cgroup/cpuacct,cpu"};
  limit = cuvs_cpu_read_quota("/sys/fs/cgroup", 1, limit);
  for (size_t i = 0; i < sizeof(v1_roots) / sizeof(v1_roots[0]); ++i)
    limit = cuvs_cpu_read_quota(v1_roots[i], 0, limit);
  FILE* file = fopen("/proc/self/cgroup", "r");
  if (file) {
    char line[8192];
    while (fgets(line, sizeof(line), file)) {
      char* first = strchr(line, ':');
      char* second = first ? strchr(first + 1, ':') : NULL;
      if (!second) continue;
      *second = 0;
      char* group = second + 1;
      group[strcspn(group, "\r\n")] = 0;
      if (first[1] == 0) {
        limit = cuvs_cpu_walk_quotas("/sys/fs/cgroup", group, 1, limit);
      } else {
        char controllers[512];
        const int n = snprintf(controllers, sizeof(controllers), ",%s,", first + 1);
        if (n > 0 && (size_t)n < sizeof(controllers) && strstr(controllers, ",cpu,")) {
          for (size_t i = 0; i < sizeof(v1_roots) / sizeof(v1_roots[0]); ++i)
            limit = cuvs_cpu_walk_quotas(v1_roots[i], group, 0, limit);
        }
      }
    }
    fclose(file);
  }
#endif
  return limit;
}
