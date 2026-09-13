import unittest
from maintenance_eval_report import retries,populated

class ReportTests(unittest.TestCase):
    def test_retry_requires_matching_failed_attempt(self):
        rows=[{'name':'read','arguments':{'path':'a'},'result':{'isError':True}},
              {'name':'read','arguments':{'path':'b'},'result':{}},
              {'name':'read','arguments':{'path':'a'},'result':{}},
              {'name':'read','arguments':{'path':'a'},'result':{}}]
        self.assertEqual(retries(rows),1)
    def test_empty_or_failed_response_is_not_warm_success(self):
        self.assertFalse(populated({'content':[{'type':'text','text':'No matching nodes found.'}]}))
        self.assertFalse(populated({'content':[{'type':'text','text':'{"focus":null,"included":[]}'}]}))
        self.assertFalse(populated({'isError':True,'content':[{'type':'text','text':'3 nodes found'}]}))
        self.assertTrue(populated({'content':[{'type':'text','text':'{"focus":{"id":"f"}}'}]}))

if __name__=='__main__':unittest.main()
