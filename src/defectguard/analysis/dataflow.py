"""函数内局部标量的 may-reaching-definitions；不做指针别名或调用副作用分析。"""
from __future__ import annotations

from collections import deque

from defectguard.domain import CFGEdge, CFGNode, ProgramEdge


def reaching_definitions(
    blocks: tuple[CFGNode, ...], edges: tuple[CFGEdge, ...], ast_ids: set[str]
) -> tuple[ProgramEdge, ...]:
    by_id = {block.node_id: block for block in blocks}
    if len(by_id) != len(blocks):
        raise ValueError("CFG 中存在重复节点 ID")
    successors: dict[str, set[str]] = {key: set() for key in by_id}
    predecessors: dict[str, set[str]] = {key: set() for key in by_id}
    for edge in edges:
        if edge.source not in by_id or edge.target not in by_id:
            raise ValueError("CFG 边引用了不存在的节点")
        successors[edge.source].add(edge.target)
        predecessors[edge.target].add(edge.source)
    for block in blocks:
        for event in block.events:
            if event.node not in ast_ids or event.kind not in {"def", "use", "undef"} or not event.symbol:
                raise ValueError("CFG 读写事件无效或缺少 AST 映射")

    # Unreachable blocks must not contribute definitions at joins.
    reachable = {block.node_id for block in blocks if block.kind == "entry"}
    pending = list(reachable)
    while pending:
        current = pending.pop()
        for following in successors[current] - reachable:
            reachable.add(following)
            pending.append(following)
    incoming: dict[str, dict[str, frozenset[str]]] = {key: {} for key in by_id}
    outgoing: dict[str, dict[str, frozenset[str]]] = {key: {} for key in by_id}
    queue = deque(sorted(reachable))
    queued = set(queue)
    while queue:
        current = queue.popleft()
        queued.remove(current)
        merged: dict[str, frozenset[str]] = {}
        for predecessor in sorted(predecessors[current] & reachable):
            for variable, definitions in outgoing[predecessor].items():
                merged[variable] = merged.get(variable, frozenset()) | definitions
        incoming[current] = merged
        state = dict(merged)
        for event in by_id[current].events:
            if event.kind == "def":
                state[event.symbol] = frozenset((event.node,))
            elif event.kind == "undef":
                state.pop(event.symbol, None)
        if outgoing[current] != state:
            outgoing[current] = state
            for following in sorted(successors[current] - queued):
                queue.append(following)
                queued.add(following)

    dependencies: set[ProgramEdge] = set()
    for block_id in sorted(reachable):
        state = dict(incoming[block_id])
        for event in by_id[block_id].events:
            if event.kind == "use":
                for definition in state.get(event.symbol, ()):
                    dependencies.add(ProgramEdge(definition, event.node, "def-use", event.symbol))
            elif event.kind == "def":
                state[event.symbol] = frozenset((event.node,))
            else:
                state.pop(event.symbol, None)
    return tuple(sorted(dependencies, key=lambda edge: (edge.source, edge.target, edge.symbol)))
