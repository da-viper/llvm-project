"""
Test SBCommandInterpreter.HandleCompletionWithDescriptions.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class CommandInterpreterCompletionTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def complete(self, line, exe_ctx=None):
        """Returns the completions of `line` without the common prefix."""
        interpreter = self.dbg.GetCommandInterpreter()
        matches = lldb.SBStringList()
        descriptions = lldb.SBStringList()
        if exe_ctx is None:
            num_matches = interpreter.HandleCompletionWithDescriptions(
                line, len(line), 0, -1, matches, descriptions
            )
        else:
            num_matches = interpreter.HandleCompletionWithDescriptions(
                line, len(line), -1, exe_ctx, matches, descriptions
            )
        self.assertEqual(matches.GetSize(), num_matches + 1)
        return list(matches)

    def test_completion_in_execution_context(self):
        """Completes in the given frame, without selecting it."""
        self.build()
        _, _, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break in callee", lldb.SBFileSpec("main.c")
        )
        caller_frame = thread.GetFrameAtIndex(1)
        caller_ctx = lldb.SBExecutionContext(caller_frame)

        self.assertIn("caller_local", self.complete("frame variable call", caller_ctx))
        self.assertIn("caller_local", self.complete("expression -- caller_l", caller_ctx))

        # Without a context, the selected frame (the callee's) is used.
        self.assertIn("callee_arg", self.complete("frame variable call"))
        self.assertNotIn("caller_local", self.complete("frame variable call"))
        self.assertIn(
            "callee_arg", self.complete("frame variable call", lldb.SBExecutionContext())
        )

        self.assertEqual(thread.GetSelectedFrame().GetFrameID(), 0)
