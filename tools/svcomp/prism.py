# This file is part of PRISM (roadmap 6.3): a BenchExec tool-info module.
#
# BenchExec loads it as ``tools.svcomp.prism`` from this repository, or as
# ``benchexec.tools.prism`` once copied into BenchExec's tool directory. It is
# self-contained on purpose (BenchExec and the standard library only).
"""BenchExec tool-info module for PRISM (SV-COMP).

The executable is ``prism_svcomp.py`` (next to this file), which runs::

    prism TASK.c --no-llm --stage inventory,classify,bmc,pir --out DIR

maps report.json to an SV-COMP answer and writes ``witness.yml`` (format
2.0: a violation witness for ``false``, a correctness witness for ``true``). See docs/SVCOMP.md for the mapping rules.

Benchmark definitions should pass ``<option name="--allow-exec"/>``: a
``false`` answer needs the counterexample to replay, and replay compiles and
runs the task (PRISM Law 9: never without an explicit opt-in). Without the
option every refutation is reported as ``unknown``.
"""

import benchexec.result as result
import benchexec.tools.template

RESULT_PREFIX = "PRISM-SVCOMP-RESULT: "

_ANSWERS = {
    "true": result.RESULT_TRUE_PROP,
    "false(no-overflow)": result.RESULT_FALSE_OVERFLOW,
    "false(unreach-call)": result.RESULT_FALSE_REACH,
    "false(valid-deref)": result.RESULT_FALSE_DEREF,
    "false(valid-free)": result.RESULT_FALSE_FREE,
    "false(valid-memtrack)": result.RESULT_FALSE_MEMTRACK,
    "unknown": result.RESULT_UNKNOWN,
}


class Tool(benchexec.tools.template.BaseTool2):
    """PRISM (Performance, Regression, Integration and Security Module)."""

    REQUIRED_PATHS = ["prism_svcomp.py", "witness.py", "prism"]

    def executable(self, tool_locator):
        return tool_locator.find_executable("prism_svcomp.py")

    def name(self):
        return "PRISM"

    def project_url(self):
        return None

    def version(self, executable):
        return self._version_from_tool(executable, arg="--version")

    def cmdline(self, executable, options, task, rlimits):
        cmd = [executable, *options]
        if task.property_file:
            cmd += ["--prop", task.property_file]
        data_model = (task.options or {}).get("data_model") if isinstance(task.options, dict) else None
        if data_model:
            cmd += ["--data-model", data_model]
        return [*cmd, task.single_input_file]

    def determine_result(self, run):
        answer = None
        for line in run.output:
            line = line.strip()
            if line.startswith(RESULT_PREFIX):
                answer = line[len(RESULT_PREFIX):].strip()
        if answer is None:
            if run.was_terminated and "time" in str(run.termination_reason):
                return result.RESULT_TIMEOUT
            code = run.exit_code.value if run.exit_code is not None else None
            return result.RESULT_ERROR + (f" (exit {code})" if code else "")
        if answer in _ANSWERS:
            return _ANSWERS[answer]
        if answer.startswith("error"):
            return result.RESULT_ERROR
        return result.RESULT_UNKNOWN
