from __future__ import annotations

import ast
import pathlib

import bookreplay as br

# pybind11 puts these on every enum it makes; no stub lists them per class.
ENUM_BUILTINS = {"name", "value"}


def stub_path() -> pathlib.Path:
    return pathlib.Path(br.__file__).with_name("bookreplay.pyi")


def bound_name(node: ast.stmt) -> str | None:
    if isinstance(node, ast.FunctionDef):
        return node.name
    if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
        return node.target.id
    return None


def public(names: object) -> set[str]:
    return {name for name in names if not name.startswith("_")}  # type: ignore[attr-defined]


def declared() -> tuple[set[str], dict[str, set[str]]]:
    tree = ast.parse(stub_path().read_text(encoding="utf-8"))
    top: set[str] = set()
    members: dict[str, set[str]] = {}
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            top.add(node.name)
            members[node.name] = public(
                name for name in map(bound_name, node.body) if name is not None
            )
        else:
            name = bound_name(node)
            if name is not None:
                top.add(name)
    return top, members


def own_members(obj: type) -> set[str]:
    names = public(vars(obj))
    return names - ENUM_BUILTINS if hasattr(obj, "__members__") else names


def test_the_stub_ships_beside_the_module() -> None:
    assert stub_path().is_file()


def test_the_stub_names_exactly_what_the_module_exports() -> None:
    top, _ = declared()
    assert top == public(dir(br))


def test_the_stub_names_exactly_the_members_of_every_class() -> None:
    _, members = declared()
    drift = {
        name: sorted(names ^ own_members(getattr(br, name)))
        for name, names in members.items()
        if names != own_members(getattr(br, name))
    }
    assert drift == {}
