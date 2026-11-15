import unittest
from unittest.mock import MagicMock

from smem_test_common import TestClient


class ClientStatusTest(unittest.TestCase):
    def make_client(self, response):
        client = TestClient.__new__(TestClient)
        client._client = MagicMock()
        client.execute = MagicMock(return_value=response)
        return client

    def test_init_returns_integer_status(self):
        for response, expected in [("0", 0), ("-1", -1)]:
            with self.subTest(response=response):
                client = self.make_client(response)
                self.assertEqual(client.init_smem_bm(), expected)
                self.assertIsInstance(client.init_smem_bm(), int)

    def test_malformed_status_is_rejected(self):
        with self.assertRaises(ValueError):
            self.make_client("invalid-status").init_smem_bm()


if __name__ == "__main__":
    unittest.main()
