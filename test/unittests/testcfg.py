# Copyright 2017 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os

from testrunner.local import command
from testrunner.local import utils
from testrunner.local import testsuite
from testrunner.objects import testcase


ADDITIONAL_VARIANTS = set(["minor_mc"])


class VariantsGenerator(testsuite.VariantsGenerator):

  def __init__(self, variants):
    super().__init__(variants)
    self._supported_variants = self._standard_variant + [
        v for v in variants if v in ADDITIONAL_VARIANTS
    ]

  def _get_variants(self, test):
    if test.only_standard_variant:
      return self._standard_variant
    return self._supported_variants


class TestLoader(testsuite.TestLoader):
  def _list_test_filenames(self):
    shell = os.path.abspath(
      os.path.join(self.test_config.shell_dir, "unittests"))
    if utils.IsWindows():
      shell += ".exe"

    output = None
    for i in range(3): # Try 3 times in case of errors.
      cmd = self.ctx.command(
          cmd_prefix=self.test_config.command_prefix,
          shell=shell,
          args=['--gtest_list_tests'] + self.test_config.extra_flags)
      output = cmd.execute()
      if output.exit_code == 0:
        break

      print("Test executable failed to list the tests (try %d).\n\nCmd:" % i)
      print(cmd)
      print("\nStdout:")
      print(output.stdout)
      print("\nStderr:")
      print(output.stderr)
      print("\nExit code: %d" % output.exit_code)
    else:
      raise Exception("Test executable failed to list the tests.")

    # TODO create an ExecutableTestLoader for refactoring this similar to
    # JSTestLoader.
    test_names = []
    test_case = ''
    for line in output.stdout.splitlines():
      test_desc = line.strip().split()[0]
      if test_desc.endswith('.'):
        test_case = test_desc
      elif test_case and test_desc:
        test_names.append(test_case + test_desc)

    return sorted(test_names)


class TestSuite(testsuite.TestSuite):
  def _test_loader_class(self):
    return TestLoader

  def _test_class(self):
    return TestCase

  def _variants_gen_class(self):
    return VariantsGenerator


class TestCase(testcase.TestCase):
  def _get_suite_flags(self):
    return (
        ["--gtest_filter=" + self.path] +
        ["--gtest_random_seed=%s" % self.random_seed] +
        ["--gtest_print_time=0"]
    )

  def get_shell(self):
    return self.suite.name

  def _get_resources(self):
    # Bytecode-generator tests are the only ones requiring extra files on
    # Android.
    parts = self.name.split('.')
    if parts[0] == 'BytecodeGeneratorTest':
      expectation_file = os.path.join(self.suite.root, 'interpreter',
                                      'bytecode_expectations',
                                      '%s.golden' % parts[1])
      if os.path.exists(expectation_file):
        return [expectation_file]
    return []
