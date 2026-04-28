# [RFC] Making the lldb-dap test infrastructure more reliable.

## Summary

This RFC proposes changes to the lldb-dap test infrastructure to address persistent flakiness in CI.
The goal is to make tests deterministic and easier to write correctly.

## Motivation

The lldb-dap test suite suffers from well known flakiness in CI.
Tests fail seemingly at random, even for commits that do not touch LLDB at all.
Flaky tests are problematic, because they tend to get ignored over time and eventually disabled, meaning real regressions go undetected.
This is the current situation on Windows where bot owners have disabled lldb-dap tests entirely.

There are structural problems in how the test infrastructure was designed. This RFC identifies those problems and proposes concrete solutions.

## Problem Analysis

### 1. Event Ordering

Tests frequently need to wait for a specific event (such at `stopped`, `module`, or `process`) to validate state.
The current implementation stores all received events in a list and removes an event only when a test explicitly waits for it.

The problem is that `wait_for_event("event_name")` always returns the **oldest** matching event in the list,
with no way to express "give me the X event that arrived **after** Y event or response.

**Example of the failure scenario:**

```py
# History of received events:

{"seq": 1, "event": "module", "name": "main"},  # loaded before process starts
{"seq": 2, "event": "module", "name": "libc"},  # loaded before process starts
{"seq": 3, "event": "process", "id": "322902"}, # process started
{"seq": 4, "event": "module", "name": "libsdl"} # loaded after process starts
```

```py
# The test wants to assert on modules loaded after the process started.
process_event = wait_for_event("process")

# BUG: This returns the "main" module event (seq=1),
# not the "libsdl" event (seq=4) that arrived after the process.
module_event = wait_for_event("module")

# The current way, a test cannot determine if an event happened before or after another event easily
```

The test neither errors nor warns. It silently asserts against stale data, producing false positives or false negatives depending on the timing of the run.

#### Proposed Solution: An Event History

Introduce a sequence number anchor to `wait_for_event`.
Any event wait must now specify the event or response it must come **after**, making the ordering constraint explicit and eliminating the stale-event class of bugs.

```py
# proposed API
process_event = client.wait_for_first_event("process")

# All of these are no longer ambiguous.
stopped_event = client.wait_for_event("stopped", after=process_event)
module_event = client.wait_for_event("module", after=process_event)
output_event = client.wait_for_event("output", after=stopped_event)
```

Events are never discarded in history. so you can make the same query and
always get the same result.

```py
# Fix for the example above.
process_event = event_history.wait_for_first_event("process")

# Explicitly request the next module event *after* the process event.
module_event = event_history.wait_for_event("module", after=process_event)
# event_history correctly returns the "libsdl" event (seq=4).
```

### 2. The Initial Launch Sequence

The DAP protocol defines a strict handshake that must complete before a debug session is in a usable state.
see [launch-sequencing](https://microsoft.github.io/debug-adapter-protocol/overview#launch-sequencing).

Some helper functions perform part of the sequence implicitly,
Some tests skip steps, some perform steps out of order,
and because the synchronous request/response model can mask these errors on fast machines.
These symptoms are more likely to show up in CI, which is often run on slower machines that are under perpetual, heavy load.

**Common failure pattern:**

```python
# Current approach - sequence is implicit and easy to get wrong.
self.build_and_launch(program)
self.create_debug_adaptor()          # starts the server
self.launch(program)                 # sends launch but skips configuration phase
self.continue_to_exit()              # sends configurationDone, continue and waits for exit event.

# The process never stopped and the continue request will fail.
```

On a fast local machine this often works, the process would have exited before we send continue.
In CI, under load lldb-dap will return an error because the process has not stopped.

#### Proposed Solution: Explicit Launch Sequence Helper

Provide a single `client.start_debug_session()` helper that encodes the full DAP initialization handshake.
Tests no longer need to know the protocol internals to get into a valid starting state.
It helps new contributors to write test without knowing how to perform the handshake.

```py
class MyTest(DAPTestCase):
    def test_breakpoint_hit(self):
        self.build()
        program = self.getBuildArtifact("a.out")

        # The breakpoint to set during the handshake.
        client.add_pending_source_breakpoints("main.c",[10])
        # One call performs the full handshake:
        # initialize -> launch -> setXXXBreakpoints -> configurationDone -> launchResponse
        # then waits for the process event.
        process_event, breakpoint_ids = client.start_debug_session(
            LaunchArgs(program=program stop_on_entry=True,)
        )

        # Get the next stoppedEvent after process_event with the reason 'breakpoint' and
        # has breakpointHitIds matching breakpoint_ids.
        stop_event = client.verify_stopped_on_breakpoint(breakpoint_ids, after=process_event)
```

For the rare tests that needs to intervene mid-sequence (for example, to assert on the `initialized` event itself) the tests can do that manually.

### 3. Synchronous Request/Response Blocking

The current client blocks on every request until a response arrives.
This means any event that arrives while waiting for a response is silently queued,
and the test has no way to interleave event handling with ongoing requests.
This makes it impossible to correctly model scenarios where the adapter sends events mid-flight.

#### Proposed Solution: Response Handle

Sending an request now returns a handle that tests can use to get a response.

```py
handle = client.send_request(ContinueArgs(thread_id))
continue_response = client.get_response(handle)

# In cases where you want to handle the response immediately.
continue_response = client.request_and_respond(ContinueArgs(thread_id))
```

## Benefits of this approach.

- No more false positives passing tests due to race conditions.
- The cause of a test failure is more explicit.
- It becomes possible to test multi-session and multi-server scenarios.

## Proposed rollout plan.

The new test infrastructure will be added with a few tests ported. other test will be ported
to the new infrastructure. the old infrastructure is removed. The lldb-dap tests is enabled on windows.

## Other Related changes

### Decouple TestCase from the DAP Session

The current design tightly couples the test case class to a single DAP session. The `TestCase` is the session.
This makes it impossible to test scenarios involving multiple simultaneous connections or to unit-test the session logic itself.

The proposed design separates these concerns:
set_xxx, verify_stopped_xxxx, wait_for_xxxx are not tied to a session instead of a test case.
This makes it easier to write new tests for running lldb-dap in server mode.
or running the entire lldb-dap test under one server.

## Tradeoffs and Concerns

- **Verbosity:** Some tests that previously relied on implicit session state will need to be more explicit.
  This is intentional. The old brevity was hiding incorrect assumptions.

- **Testing the test:** The proposed test infrastructures adds classes and types that
  will also need testing to ensure the actual test works properly.
