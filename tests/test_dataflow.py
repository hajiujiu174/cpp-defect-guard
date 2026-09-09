from __future__ import annotations

import unittest

from defectguard.analysis.dataflow import reaching_definitions
from defectguard.domain import CFGEdge, CFGNode, VariableEvent


def block(name, kind="block", *events):
    return CFGNode(name, 1, kind, name, events=tuple(VariableEvent(*event) for event in events))


def dependencies(blocks, paths):
    edges = tuple(CFGEdge(source, target, "successor") for source, target in paths)
    ids = {event.node for item in blocks for event in item.events}
    return {(edge.source, edge.target, edge.symbol) for edge in reaching_definitions(tuple(blocks), edges, ids)}


class DataflowTests(unittest.TestCase):
    def test_branch_merge_and_unreachable_definition(self):
        blocks = [block("entry", "entry", ("def", "x", "initial")),
                  block("left", "block", ("def", "x", "updated")), block("right"),
                  block("dead", "block", ("def", "x", "unreachable")),
                  block("join", "exit", ("use", "x", "read"))]
        result = dependencies(blocks, [("entry", "left"), ("entry", "right"),
                                      ("left", "join"), ("right", "join"), ("dead", "join")])
        self.assertEqual({("initial", "read", "x"), ("updated", "read", "x")}, result)

    def test_loop_reaches_fixed_point(self):
        blocks = [block("entry", "entry", ("def", "x", "initial")),
                  block("loop", "block", ("use", "x", "read"), ("def", "x", "update")),
                  block("exit", "exit", ("use", "x", "result"))]
        result = dependencies(blocks, [("entry", "loop"), ("loop", "loop"), ("loop", "exit")])
        self.assertEqual({("initial", "read", "x"), ("update", "read", "x"),
                          ("update", "result", "x")}, result)

    def test_write_kills_old_definition_and_shadow_symbols_are_distinct(self):
        blocks = [block("entry", "entry", ("def", "outer", "old"), ("def", "inner", "shadow"),
                        ("def", "outer", "new"), ("use", "outer", "read"),
                        ("undef", "inner", "declaration"), ("use", "inner", "unknown"))]
        self.assertEqual({("new", "read", "outer")}, dependencies(blocks, []))

    def test_dangling_cfg_edges_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "不存在"):
            reaching_definitions((block("entry", "entry"),),
                                 (CFGEdge("entry", "missing", "successor"),), set())


if __name__ == "__main__":
    unittest.main()
