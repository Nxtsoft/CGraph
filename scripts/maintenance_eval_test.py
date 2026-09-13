"""Grader checks against real broken and corrected fixture programs."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from maintenance_eval import grade,FIXTURES,RESEARCH,sandbox_profile
import subprocess

class EvalTests(unittest.TestCase):
    def test_real_policy_bug_is_rejected_then_corrected(self):
        task=next(t for t in json.loads((RESEARCH/'tasks.json').read_text()) if t['id']=='payments-policy')
        ref=json.loads((RESEARCH/'references.json').read_text())[task['id']]
        answer={'relationships':[{'source':a,'target':b} for a,b in ref['relationships']]}
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as temp:
            root=Path(temp);workspace=root/'workspace';shutil.copytree(FIXTURES/'payments',workspace)
            failed=grade(task,ref,workspace,answer,[RESEARCH],root/'broken')
            self.assertFalse(failed['completed'])
            (workspace/'pricing.py').write_text('def amount_due(subtotal_cents, shipping_cents, discount_percent):\n    return subtotal_cents - subtotal_cents * discount_percent // 100 + shipping_cents\n')
            passed=grade(task,ref,workspace,answer,[RESEARCH],root/'corrected')
            self.assertTrue(passed['completed'])
            answer['relationships'].append({'source':'invented.caller','target':'pricing.amount_due'})
            self.assertFalse(grade(task,ref,workspace,answer,[RESEARCH],root/'false-edge')['completed'])

    def test_hidden_reference_is_os_denied(self):
        reference=RESEARCH/'references.json'
        result=subprocess.run(['/usr/bin/sandbox-exec','-p',sandbox_profile([RESEARCH]),'/bin/cat',str(reference)],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Operation not permitted',result.stderr)
        self.assertEqual(result.stdout,'')

if __name__ == '__main__': unittest.main()
