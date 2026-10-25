from __future__ import annotations

import logging
import unittest
from pathlib import Path

from scripts.sccache_launcher import (
    PortRange, choose_server_port, parse_excluded_port_ranges, sccache_environment, server_start_problem,
)

# `netsh int ipv4 show excludedportrange protocol=tcp` on a machine with Hyper-V/WSL reservations.
NETSH_OUTPUT = """
Protocol tcp Port Exclusion Ranges

Start Port    End Port
----------    --------
      4158        4257
      4258        4357
      5357        5357
     50000       50059     *

* - Administered port exclusions.
"""


class ExcludedPortRangeParsingTests(unittest.TestCase):
    def test_reads_every_range_including_administered_ones(self) -> None:
        self.assertEqual(parse_excluded_port_ranges(NETSH_OUTPUT), [
            PortRange(4158, 4257), PortRange(4258, 4357), PortRange(5357, 5357), PortRange(50000, 50059),
        ])

    def test_output_without_ranges_has_none(self) -> None:
        output = "\nProtocol tcp Port Exclusion Ranges\n\nStart Port    End Port\n----------    --------\n\n"
        self.assertEqual(parse_excluded_port_ranges(output), [])


class ServerPortChoiceTests(unittest.TestCase):
    def test_default_port_is_used_when_no_exclusion_covers_it(self) -> None:
        choice = choose_server_port(configured=None, excluded=[PortRange(5357, 5357)])
        self.assertEqual(choice.port, 4226)
        self.assertEqual(choice.level, logging.INFO)
        self.assertIn("default", choice.message)

    def test_excluded_default_moves_to_the_first_free_candidate(self) -> None:
        choice = choose_server_port(configured=None, excluded=[PortRange(4158, 4257)])
        self.assertEqual(choice.port, 4400)
        self.assertEqual(choice.level, logging.INFO)
        self.assertIn("4158-4257", choice.message)

    def test_excluded_candidates_are_skipped_in_order(self) -> None:
        choice = choose_server_port(configured=None, excluded=[PortRange(4158, 4257), PortRange(4380, 4479)])
        self.assertEqual(choice.port, 7226)

    def test_port_the_user_set_is_respected(self) -> None:
        choice = choose_server_port(configured="4511", excluded=[])
        self.assertEqual(choice.port, 4511)
        self.assertIn("SCCACHE_SERVER_PORT", choice.message)
        self.assertEqual(choice.level, logging.INFO)

    def test_port_the_user_set_inside_an_exclusion_is_kept_with_a_warning(self) -> None:
        choice = choose_server_port(configured="4226", excluded=[PortRange(4158, 4257)])
        self.assertEqual(choice.port, 4226)
        self.assertEqual(choice.level, logging.WARNING)
        self.assertIn("4158-4257", choice.message)

    def test_unknown_exclusions_keep_the_default_with_a_warning(self) -> None:
        choice = choose_server_port(configured=None, excluded=None)
        self.assertEqual(choice.port, 4226)
        self.assertEqual(choice.level, logging.WARNING)
        self.assertIn("SCCACHE_SERVER_PORT", choice.message)

    def test_every_candidate_excluded_keeps_the_default_with_a_warning(self) -> None:
        choice = choose_server_port(configured=None, excluded=[PortRange(1, 65535)])
        self.assertEqual(choice.port, 4226)
        self.assertEqual(choice.level, logging.WARNING)


class ServerPortEnvironmentTests(unittest.TestCase):
    def test_chosen_port_goes_into_the_build_environment(self) -> None:
        environment = sccache_environment(Path("D:/Profile/AppData/Local"), server_port=4400)
        self.assertEqual(environment["SCCACHE_SERVER_PORT"], "4400")
        self.assertIn("SCCACHE_DIR", environment)


# What `sccache --start-server` (0.18) prints when Windows excludes its port; it exits with 2.
EXCLUDED_PORT_OUTPUT = """sccache: Starting the server...
sccache: error: Server startup failed: A Windows port exclusion is blocking use of the configured port.
Try setting SCCACHE_SERVER_PORT to a new value.
An attempt was made to access a socket in a way forbidden by its access permissions. (os error 10013)
"""
ALREADY_RUNNING_OUTPUT = "sccache: Starting the server...\nsccache: error: Server startup failed: Address in use\n"


class ServerStartTests(unittest.TestCase):
    def test_started_server_is_fine(self) -> None:
        output = "sccache: Starting the server...\nsccache: Listening on address 127.0.0.1:4400\n"
        self.assertIsNone(server_start_problem(0, output, server_answers=True, port=4400))

    def test_server_already_running_is_fine(self) -> None:
        self.assertIsNone(server_start_problem(2, ALREADY_RUNNING_OUTPUT, server_answers=True, port=4400))

    def test_server_that_could_not_start_stops_the_build_naming_cause_and_ways_out(self) -> None:
        problem = server_start_problem(2, EXCLUDED_PORT_OUTPUT, server_answers=False, port=4226)
        self.assertIn("4226", problem)
        self.assertIn("A Windows port exclusion is blocking use of the configured port", problem)
        self.assertIn("--no-sccache", problem)
        self.assertIn("SCCACHE_SERVER_PORT", problem)

    def test_silent_failure_still_stops_the_build(self) -> None:
        problem = server_start_problem(1, "", server_answers=False, port=4226)
        self.assertIn("exit code 1", problem)
        self.assertIn("--no-sccache", problem)


if __name__ == "__main__":
    unittest.main()
