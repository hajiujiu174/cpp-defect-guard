import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from real_repair_evidence import source_record, capture_evidence, verify_evidence


class EvidenceTests(unittest.TestCase):
    def prepare(self, root):
        records = []
        for group, suffix in [('proposals','.cgfix'),('sets','.cgset')]:
            folder=root/('source '+group);folder.mkdir()
            manifest=folder/('repair'+suffix);manifest.write_bytes(b'wire\x00bytes\r\n')
            (folder/'candidate.diff').write_text('before\n+修改\n',encoding='utf-8')
            records.append(source_record(group,group,manifest))
        return records

    def test_capture_preserves_bytes_and_verifies_after_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sources=self.prepare(root);dest=root/'captured'
            self.assertEqual(capture_evidence(dest,sources)['files'],4)
            self.assertEqual(verify_evidence(dest)['proposals'],1)
            for record in json.loads((dest/'manifest.json').read_text(encoding='utf-8'))['files']:
                original=next(x for s in sources for x in s['files'] if s['group']==record['group'] and x['source'].name==Path(record['path']).name)
                self.assertEqual((dest/record['path']).read_bytes(),original['source'].read_bytes())

    def test_source_mutation_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sources=self.prepare(root);sources[0]['files'][0]['source'].write_bytes(b'changed')
            with self.assertRaises(ValueError):capture_evidence(root/'captured',sources)

    def test_missing_diff_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);manifest=root/'repair.cgfix';manifest.write_bytes(b'wire')
            with self.assertRaises(ValueError):source_record('proposals','p',manifest)

    def test_existing_destination_is_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sources=self.prepare(root);dest=root/'captured';dest.mkdir();(dest/'keep').write_bytes(b'original')
            with self.assertRaises(FileExistsError):capture_evidence(dest,sources)
            self.assertEqual((dest/'keep').read_bytes(),b'original')

    def test_deleted_or_corrupted_capture_is_rejected(self):
        for corrupt in (False,True):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);dest=root/'captured';capture_evidence(dest,self.prepare(root))
                record=json.loads((dest/'manifest.json').read_text(encoding='utf-8'))['files'][0];target=dest/record['path']
                if corrupt:target.write_bytes(b'X'+target.read_bytes()[1:])
                else:target.unlink()
                with self.assertRaises((ValueError,FileNotFoundError)):verify_evidence(dest)

    def test_traversal_is_rejected(self):
        for name in ('../escape.cgfix','/absolute.cgfix','C:/escape.cgfix','a\\..\\escape.cgfix'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);dest=root/'captured';capture_evidence(dest,self.prepare(root))
                manifest=dest/'manifest.json';value=json.loads(manifest.read_text(encoding='utf-8'));value['files'][0]['path']=name;manifest.write_text(json.dumps(value),encoding='utf-8')
                with self.assertRaises(ValueError):verify_evidence(dest)

    def test_unlisted_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dest=root/'captured';capture_evidence(dest,self.prepare(root));(dest/'extra').write_bytes(b'extra')
            with self.assertRaises(ValueError):verify_evidence(dest)


if __name__=='__main__':unittest.main()
