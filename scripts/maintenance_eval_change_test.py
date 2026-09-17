import unittest
from maintenance_eval_change import successful_changes
class ChangeRunTests(unittest.TestCase):
    def test_advisory_error_or_empty_changes_does_not_count(self):
        records=[{'name':'graph_change_context','result':{'isError':True}},
                 {'name':'graph_change_context','result':{'content':[{'text':'{"schema_version":1,"base":{"root":"a"},"target":{"root":"b"},"changes":[]}'}]}}]
        self.assertEqual(successful_changes(records),0)
if __name__=='__main__':unittest.main()
