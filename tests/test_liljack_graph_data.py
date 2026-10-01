import sys
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolbox'))
from liljack_app.graph_data import snapshot


class GraphTests(unittest.TestCase):
    def test_merge_parents_refs_and_boundary_are_preserved(self):
        # Build fields explicitly: Python octal escapes must not eat the separator.
        output = '\n'.join('\0'.join(row) for row in [
            ['merge','left right','HEAD -> topic','2026-09-09','merge subject'],
            ['left','base','','2026-09-08','left subject']])
        with patch('liljack_app.graph_data.subprocess.run', return_value=subprocess.CompletedProcess([],0,output,'')) as run:
            value=snapshot('/chosen/repo',1)
        self.assertEqual(value['nodes'][0]['parents'],['left','right'])
        self.assertEqual(value['nodes'][0]['refs'],'HEAD -> topic')
        self.assertTrue(value['truncated'])
        self.assertIn('/chosen/repo',run.call_args.args[0])

    def test_errors_are_explicit(self):
        for result in [subprocess.CompletedProcess([],1,'','not a repository'),subprocess.CompletedProcess([],0,'malformed','')]:
            with patch('liljack_app.graph_data.subprocess.run',return_value=result):
                value=snapshot('/bad')
            self.assertTrue(value['error']);self.assertEqual(value['nodes'],[])

if __name__=='__main__':unittest.main()
