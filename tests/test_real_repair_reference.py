import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from real_repair_reference import canonical_ast, command_arguments


class ReferenceAstTests(unittest.TestCase):
    def test_parentheses_do_not_erase_operator_structure(self):
        ref = {'kind': 'DeclRefExpr', 'referencedDecl': {'kind': 'VarDecl', 'name': 'error', 'type': {'qualType': 'int'}}}
        value = {'kind': 'IntegerLiteral', 'value': '0', 'type': {'qualType': 'int'}}
        bad = {'kind': 'BinaryOperator', 'opcode': '=', 'inner': [ref, {'kind': 'BinaryOperator', 'opcode': '<', 'inner': [value, value]}]}
        good = {'kind': 'BinaryOperator', 'opcode': '<', 'inner': [{'kind': 'BinaryOperator', 'opcode': '=', 'inner': [ref, value]}, value]}
        self.assertNotEqual(canonical_ast(bad, Path('a')), canonical_ast(good, Path('a')))
        self.assertEqual(canonical_ast(good, Path('a')), canonical_ast({'kind': 'ParenExpr', 'inner': [good]}, Path('a')))

    def test_types_conversions_and_names_remain_significant(self):
        a = {'kind': 'ImplicitCastExpr', 'castKind': 'IntegralToBoolean', 'type': {'qualType': 'bool'}, 'inner': []}
        b = dict(a, castKind='IntegralCast')
        self.assertNotEqual(canonical_ast(a, Path('a')), canonical_ast(b, Path('a')))
        self.assertNotEqual(canonical_ast({'kind': 'MemberExpr', 'name': 'first'}, Path('a')),
                            canonical_ast({'kind': 'MemberExpr', 'name': 'second'}, Path('a')))

    def test_only_recorded_source_root_and_nonsemantic_locations_change(self):
        a = {'kind': 'StringLiteral', 'value': 'D:/one/source/main.c', 'id': '0x1', 'range': {'begin': {'line': 2}}}
        b = {'kind': 'StringLiteral', 'value': 'D:/two/source/main.c', 'id': '0x2', 'range': {'begin': {'line': 9}}}
        self.assertEqual(canonical_ast(a, Path('D:/one/source')), canonical_ast(b, Path('D:/two/source')))
        b['value'] = 'a different literal'
        self.assertNotEqual(canonical_ast(a, Path('D:/one/source')), canonical_ast(b, Path('D:/two/source')))

    def test_command_line_quoted_paths_and_definitions(self):
        if sys.platform == 'win32':
            command = r'"C:\Program Files\tool\gcc.exe" -DVALUE=\"\" -I"D:\source files" -c "D:\source files\main.c"'
            values = command_arguments(command)
            self.assertEqual(values[0], r'C:\Program Files\tool\gcc.exe')
            self.assertEqual(values[1], '-DVALUE=""')
            self.assertEqual(values[2], r'-ID:\source files')
            self.assertEqual(values[-1], r'D:\source files\main.c')


if __name__ == '__main__':
    unittest.main()
