from __future__ import annotations

import logging
import unittest
from pathlib import Path

from scripts.dev_build import GeneratedCode, WorkspaceState, configure_arguments, plan_build
from scripts.sccache_launcher import (
    CompileStats, choose_launcher, describe_build_stats, sccache_environment, server_settings_problem, stats_delta,
)

SCCACHE = Path("C:/tools/sccache/sccache.exe")
BUILD = Path("/cache/dev/n")
DEPENDENCIES = Path("/cache/d")
NINJA = Path("/vs/ninja.exe")
GENERATED = GeneratedCode(normalized=Path("/g/k/normalized"), libultra=Path("/g/k/libultra.json"),
                          audio=Path("/g/k/audio"))
KEY = "71b7842b36c8cf5a"


def arguments_with(launcher: Path | None) -> list[str]:
    return configure_arguments(BUILD, GENERATED, DEPENDENCIES, NINJA, launcher)


def workspace_configured_with(arguments: list[str]) -> WorkspaceState:
    return WorkspaceState(store_key=KEY, configure_arguments=arguments, configure_succeeded=True,
                          has_cmake_cache=True, cmake_generator="Ninja")


class LauncherDecisionTests(unittest.TestCase):
    def test_found_sccache_becomes_the_launcher(self) -> None:
        decision = choose_launcher(no_sccache=False, found=str(SCCACHE))
        self.assertEqual(decision.launcher, SCCACHE)
        self.assertEqual(decision.level, logging.INFO)
        self.assertIn(str(SCCACHE), decision.message)

    def test_missing_sccache_warns_with_the_install_command(self) -> None:
        decision = choose_launcher(no_sccache=False, found=None)
        self.assertIsNone(decision.launcher)
        self.assertEqual(decision.level, logging.WARNING)
        self.assertIn("winget install Mozilla.sccache", decision.message)

    def test_no_sccache_flag_builds_without_it_and_only_informs(self) -> None:
        decision = choose_launcher(no_sccache=True, found=str(SCCACHE))
        self.assertIsNone(decision.launcher)
        self.assertEqual(decision.level, logging.INFO)
        self.assertIn("--no-sccache", decision.message)


class LauncherConfigureArgumentTests(unittest.TestCase):
    def test_launcher_is_set_for_c_and_cxx(self) -> None:
        arguments = arguments_with(SCCACHE)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=" + str(SCCACHE), arguments)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=" + str(SCCACHE), arguments)

    def test_without_launcher_both_are_cleared(self) -> None:
        # Empty, not left out: the CMake cache would otherwise keep an earlier sccache launcher.
        arguments = arguments_with(None)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=", arguments)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=", arguments)


class LauncherReconfigureTests(unittest.TestCase):
    def test_same_launcher_does_not_reconfigure(self) -> None:
        plan = plan_build(workspace_configured_with(arguments_with(SCCACHE)), KEY, arguments_with(SCCACHE))
        self.assertFalse(plan.configure)

    def test_starting_to_use_sccache_reconfigures_naming_the_launcher(self) -> None:
        plan = plan_build(workspace_configured_with(arguments_with(None)), KEY, arguments_with(SCCACHE))
        self.assertTrue(plan.configure)
        self.assertEqual(plan.reasons, (f"compiler launcher changed: none -> {SCCACHE}",))

    def test_stopping_to_use_sccache_reconfigures_naming_the_launcher(self) -> None:
        plan = plan_build(workspace_configured_with(arguments_with(SCCACHE)), KEY, arguments_with(None))
        self.assertEqual(plan.reasons, (f"compiler launcher changed: {SCCACHE} -> none",))

    def test_tree_configured_before_launchers_existed_counts_as_none(self) -> None:
        before = [argument for argument in arguments_with(None) if "COMPILER_LAUNCHER" not in argument]
        plan = plan_build(workspace_configured_with(before), KEY, arguments_with(SCCACHE))
        self.assertEqual(plan.reasons, (f"compiler launcher changed: none -> {SCCACHE}",))


class EnvironmentTests(unittest.TestCase):
    def test_cache_lives_under_jfg_with_a_20_gib_cap(self) -> None:
        environment = sccache_environment(Path("C:/Users/dev/AppData/Local"))
        self.assertEqual(environment, {
            "SCCACHE_DIR": str(Path("C:/Users/dev/AppData/Local/JFG/cache/sccache")),
            "SCCACHE_CACHE_SIZE": "20G",
        })


def stats_document(requests: int, hits: int, misses: int, not_cacheable: int, non_cacheable: int) -> dict:
    """An `sccache --show-stats --stats-format=json` document, as sccache 0.8+ writes it."""
    return {
        "stats": {
            "compile_requests": requests,
            "requests_not_cacheable": not_cacheable,
            "non_cacheable_compilations": non_cacheable,
            "cache_hits": {"counts": {"C/C++": hits}, "adv_counts": {"msvc": hits}},
            "cache_misses": {"counts": {"C/C++": misses}, "adv_counts": {"msvc": misses}},
        },
        "cache_location": "Local disk: \"C:\\Users\\dev\\AppData\\Local\\JFG\\cache\\sccache\"",
        "max_cache_size": 21474836480,
    }


class StatsDeltaTests(unittest.TestCase):
    def test_counts_only_this_builds_compiles(self) -> None:
        before = stats_document(requests=100, hits=40, misses=50, not_cacheable=4, non_cacheable=1)
        after = stats_document(requests=1107, hits=1020, misses=70, not_cacheable=9, non_cacheable=3)
        delta = stats_delta(before, after)
        self.assertEqual(delta, CompileStats(requests=1007, hits=980, misses=20, non_cacheable=7))
        self.assertAlmostEqual(delta.hit_rate, 0.98)

    def test_hits_are_summed_over_languages(self) -> None:
        after = stats_document(requests=3, hits=0, misses=0, not_cacheable=0, non_cacheable=0)
        after["stats"]["cache_hits"]["counts"] = {"C/C++": 2, "C": 1}
        self.assertEqual(stats_delta({}, after).hits, 3)

    def test_missing_fields_count_as_zero(self) -> None:
        delta = stats_delta({}, {"stats": {"compile_requests": 5}})
        self.assertEqual(delta, CompileStats(requests=5, hits=0, misses=0, non_cacheable=0))
        self.assertIsNone(delta.hit_rate)

    def test_a_restarted_server_counts_from_zero(self) -> None:
        # Stats are cumulative since the server started; a lower count means it restarted mid-build.
        before = stats_document(requests=900, hits=800, misses=90, not_cacheable=0, non_cacheable=0)
        after = stats_document(requests=50, hits=45, misses=5, not_cacheable=0, non_cacheable=0)
        self.assertEqual(stats_delta(before, after), CompileStats(requests=50, hits=45, misses=5, non_cacheable=0))


class BuildStatsLineTests(unittest.TestCase):
    def test_names_every_count_and_the_hit_rate(self) -> None:
        line = describe_build_stats(CompileStats(requests=1007, hits=980, misses=20, non_cacheable=7))
        self.assertEqual(line, "sccache: 1007 compile requests, 980 hits, 20 misses, 7 non-cacheable; hit rate 98.0%")

    def test_without_cacheable_compiles_the_hit_rate_is_unknown(self) -> None:
        line = describe_build_stats(CompileStats(requests=0, hits=0, misses=0, non_cacheable=0))
        self.assertTrue(line.endswith("hit rate n/a"), line)


class ServerSettingsTests(unittest.TestCase):
    ENVIRONMENT = sccache_environment(Path("C:/Users/dev/AppData/Local"))

    def test_server_with_our_cache_and_cap_is_fine(self) -> None:
        document = stats_document(requests=0, hits=0, misses=0, not_cacheable=0, non_cacheable=0)
        self.assertIsNone(server_settings_problem(document, self.ENVIRONMENT))

    def test_server_with_the_default_cache_folder_is_reported(self) -> None:
        document = stats_document(requests=0, hits=0, misses=0, not_cacheable=0, non_cacheable=0)
        document["cache_location"] = 'Local disk: "C:\\Users\\dev\\AppData\\Local\\Mozilla\\sccache\\cache"'
        problem = server_settings_problem(document, self.ENVIRONMENT)
        self.assertIn("Mozilla", problem)
        self.assertIn("sccache --stop-server", problem)

    def test_server_with_another_cap_is_reported(self) -> None:
        document = stats_document(requests=0, hits=0, misses=0, not_cacheable=0, non_cacheable=0)
        document["max_cache_size"] = 10 * 1024 ** 3
        problem = server_settings_problem(document, self.ENVIRONMENT)
        self.assertIn("max size", problem)
        self.assertIn("sccache --stop-server", problem)


if __name__ == "__main__":
    unittest.main()
