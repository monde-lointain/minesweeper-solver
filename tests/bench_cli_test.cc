/* bench_cli_test.cc — Stream B.4.
 * Arg parsing: defaults, preset mapping, custom clamp/reject, error handling.
 */
#include <gtest/gtest.h>

#include "args.h"

TEST(BenchCli, Defaults) {
  char* av[] = {(char*)"bench"};
  struct BenchConfig c;
  const char* e = NULL;
  ASSERT_EQ(bench_parse_args(1, av, &c, &e), 0);
  EXPECT_EQ(c.width, 30);
  EXPECT_EQ(c.height, 16);
  EXPECT_EQ(c.mines, 99);
  EXPECT_EQ(c.games, 1000000u);
  EXPECT_EQ(c.seed, 1u);
  EXPECT_EQ(c.threads, 0);
  EXPECT_EQ(c.policy_id, POLICY_INFOGAIN);
  EXPECT_STREQ(c.label, "expert");
}

TEST(BenchCli, PresetIntermediateAndScalarFlags) {
  char* av[] = {(char*)"bench",   (char*)"--difficulty", (char*)"intermediate",
                (char*)"--games", (char*)"500",          (char*)"--seed",
                (char*)"7",       (char*)"--threads",    (char*)"3"};
  struct BenchConfig c;
  const char* e = NULL;
  ASSERT_EQ(bench_parse_args(9, av, &c, &e), 0);
  EXPECT_EQ(c.width, 16);
  EXPECT_EQ(c.height, 16);
  EXPECT_EQ(c.mines, 40);
  EXPECT_EQ(c.games, 500u);
  EXPECT_EQ(c.seed, 7u);
  EXPECT_EQ(c.threads, 3);
  EXPECT_STREQ(c.label, "intermediate");
}

TEST(BenchCli, CustomDimsClampToEnvelope) {
  char* av[] = {(char*)"bench",    (char*)"--width", (char*)"999",
                (char*)"--height", (char*)"999",     (char*)"--mines",
                (char*)"100"};
  struct BenchConfig c;
  const char* e = NULL;
  ASSERT_EQ(bench_parse_args(7, av, &c, &e), 0);
  EXPECT_EQ(c.width, 100);  /* clamped 999 -> BOARD_MAX_W */
  EXPECT_EQ(c.height, 100); /* clamped 999 -> BOARD_MAX_H */
  EXPECT_EQ(c.mines, 100);
  EXPECT_STREQ(c.label, "custom");
}

TEST(BenchCli, CustomMinesOutOfRangeRejected) {
  char* av[] = {(char*)"bench", (char*)"--width", (char*)"9", (char*)"--height",
                (char*)"9",     (char*)"--mines", (char*)"81"}; /* 81 >= 9*9 */
  struct BenchConfig c;
  const char* e = NULL;
  EXPECT_EQ(bench_parse_args(7, av, &c, &e), -1);
  EXPECT_NE(e, nullptr);
}

TEST(BenchCli, PartialCustomRejected) {
  char* av[] = {(char*)"bench", (char*)"--width", (char*)"16"};
  struct BenchConfig c;
  const char* e = NULL;
  EXPECT_EQ(bench_parse_args(3, av, &c, &e), -1);
}

TEST(BenchCli, UnknownFlagAndBadNumber) {
  char* av1[] = {(char*)"bench", (char*)"--bogus"};
  struct BenchConfig c;
  const char* e = NULL;
  EXPECT_EQ(bench_parse_args(2, av1, &c, &e), -1);

  char* av2[] = {(char*)"bench", (char*)"--games", (char*)"abc"};
  EXPECT_EQ(bench_parse_args(3, av2, &c, &e), -1);
}

TEST(BenchCli, HelpReturnsOne) {
  char* av[] = {(char*)"bench", (char*)"--help"};
  struct BenchConfig c;
  const char* e = NULL;
  EXPECT_EQ(bench_parse_args(2, av, &c, &e), 1);
}
