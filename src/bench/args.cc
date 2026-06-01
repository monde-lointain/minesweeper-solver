/* args.cc — CLI parsing for the benchmark harness (Stream B.4).
 * Return codes: 0 run, 1 help, -1 error (*errmsg set). Orthodox C-style.
 */
#include "args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minesweeper/types.h" /* BOARD_MIN_W .. BOARD_MAX_H, enum Difficulty */
#include "solver/difficulty.h"
#include "solver/util.h" /* solver_clampi */

static int parse_u64(const char* s, uint64_t* out) {
  char* end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  if (end == s || *end != '\0' || errno != 0) {
    return -1;
  }
  *out = (uint64_t)v;
  return 0;
}

static int parse_long(const char* s, long* out) {
  char* end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno != 0) {
    return -1;
  }
  *out = v;
  return 0;
}

static int set_preset(struct BenchConfig* cfg, const char* name) {
  int diff;
  if (strcmp(name, "beginner") == 0) {
    diff = DIFF_BEGINNER;
  } else if (strcmp(name, "intermediate") == 0) {
    diff = DIFF_INTERMEDIATE;
  } else if (strcmp(name, "expert") == 0) {
    diff = DIFF_EXPERT;
  } else {
    return -1;
  }
  /* Dimensions + label from the shared source of truth (same as the GUI app).
   */
  difficulty_preset_dims(diff, &cfg->width, &cfg->height, &cfg->mines);
  strcpy(cfg->label, difficulty_preset_name(diff));
  return 0;
}

int bench_parse_args(int argc, char** argv, struct BenchConfig* cfg,
                     const char** errmsg) {
  set_preset(cfg, "expert");
  cfg->games = 1000000;
  cfg->seed = 1;
  cfg->threads = 0;
  cfg->policy_id = POLICY_INFOGAIN;
  cfg->quiet = false;
  *errmsg = NULL;

  int cw = -1;
  int ch = -1;
  int cm = -1;

  for (int i = 1; i < argc; ++i) {
    const char* s = argv[i];
    if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
      return 1;
    }
    if (strcmp(s, "--quiet") == 0) {
      cfg->quiet = true;
      continue;
    }
    /* the rest take a value */
    if (i + 1 >= argc) {
      *errmsg = "missing value for option";
      return -1;
    }
    const char* val = argv[++i];
    if (strcmp(s, "--difficulty") == 0) {
      if (set_preset(cfg, val) != 0) {
        *errmsg = "unknown difficulty (beginner|intermediate|expert)";
        return -1;
      }
    } else if (strcmp(s, "--policy") == 0) {
      if (strcmp(val, "infogain") == 0) {
        cfg->policy_id = POLICY_INFOGAIN;
      } else if (strcmp(val, "baseline") == 0) {
        cfg->policy_id = POLICY_BASELINE;
      } else {
        *errmsg = "unknown policy (infogain|baseline)";
        return -1;
      }
    } else if (strcmp(s, "--games") == 0) {
      if (parse_u64(val, &cfg->games) != 0) {
        *errmsg = "bad --games value";
        return -1;
      }
    } else if (strcmp(s, "--seed") == 0) {
      uint64_t tmp = 0;
      if (parse_u64(val, &tmp) != 0) {
        *errmsg = "bad --seed value";
        return -1;
      }
      cfg->seed = (uint32_t)tmp;
    } else if (strcmp(s, "--threads") == 0) {
      long t = 0;
      if (parse_long(val, &t) != 0) {
        *errmsg = "bad --threads value";
        return -1;
      }
      cfg->threads = (int)t;
    } else if (strcmp(s, "--width") == 0) {
      long t = 0;
      if (parse_long(val, &t) != 0) {
        *errmsg = "bad --width value";
        return -1;
      }
      cw = (int)t;
    } else if (strcmp(s, "--height") == 0) {
      long t = 0;
      if (parse_long(val, &t) != 0) {
        *errmsg = "bad --height value";
        return -1;
      }
      ch = (int)t;
    } else if (strcmp(s, "--mines") == 0) {
      long t = 0;
      if (parse_long(val, &t) != 0) {
        *errmsg = "bad --mines value";
        return -1;
      }
      cm = (int)t;
    } else {
      *errmsg = "unknown argument";
      return -1;
    }
  }

  if (cw >= 0 || ch >= 0 || cm >= 0) {
    if (cw < 0 || ch < 0 || cm < 0) {
      *errmsg = "custom board requires --width --height --mines together";
      return -1;
    }
    cw = solver_clampi(cw, BOARD_MIN_W, BOARD_MAX_W);
    ch = solver_clampi(ch, BOARD_MIN_H, BOARD_MAX_H);
    if (cm < 1 || cm >= cw * ch) {
      *errmsg = "--mines out of range (1 .. width*height-1)";
      return -1;
    }
    cfg->width = cw;
    cfg->height = ch;
    cfg->mines = cm;
    strcpy(cfg->label, "custom");
  }

  return 0;
}

void bench_usage(const char* prog) {
  printf(
      "usage: %s [options]\n"
      "  --difficulty beginner|intermediate|expert   (default: expert)\n"
      "  --width W --height H --mines M               custom board (<=30x24)\n"
      "  --games N        number of games            (default: 1000000)\n"
      "  --seed S         base RNG seed; game i uses seed+i   (default: 1)\n"
      "  --threads T      worker threads, 0=auto      (default: 0)\n"
      "  --policy infogain|baseline                   (default: infogain)\n"
      "  --quiet          suppress the pre-run line\n"
      "  -h, --help       this message\n",
      prog);
}
