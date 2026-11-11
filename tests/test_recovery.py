import tempfile
import unittest
from pathlib import Path
from minivector_store import Store


class RecoveryTest(unittest.TestCase):
    def test_every_truncated_suffix_preserves_last_commit(self):
        with tempfile.TemporaryDirectory() as root:
            db = Store(root)
            db.mutate([('table', ('t', [('id', 'bigint', None)], 0))])
            db.mutate([('put', ('t', 1, [1]))])
            first = db.committed_end
            db.begin()
            db.mutate([('delete', ('t', 1)), ('put', ('t', 2, [2]))])
            db.commit()
            second = db.committed_end
            db.begin()
            db.mutate([('put', ('t', 3, [3]))])
            data = db.path.read_bytes()
            db.close()
            for cut in range(first, len(data) + 1):
                Path(root, 'rows.log').write_bytes(data[:cut])
                recovered = Store(root)
                try:
                    expected = 1 if cut < second else 2
                    self.assertEqual(list(recovered.tables['t'].rows), [expected], cut)
                    self.assertEqual(recovered.read_row(recovered.tables['t'],
                        recovered.tables['t'].rows[expected]), [expected], cut)
                    self.assertEqual(recovered.log_end, first if cut < second else second, cut)
                finally:
                    recovered.close()
