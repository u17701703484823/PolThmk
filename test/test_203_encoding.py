import time

import pytest

from h2_conf import HttpdConf


class TestEncoding:

    @pytest.fixture(autouse=True, scope='class')
    def _class_scope(self, env):
        extras = {
            'base': f"""
        <Directory />
            require all granted
        </Directory>
        <Directory "{env.gen_dir}">
            AllowOverride None
            Options +ExecCGI -MultiViews +SymLinksIfOwnerMatch
            Require all granted
        </Directory>
        """,
        }
        conf = HttpdConf(env)
        conf.add_vhost_test1(extras=extras)
        conf.add_vhost_test2(extras={
            f"test2.{env.http_tld}": "AllowEncodedSlashes on",
        })
        conf.add_vhost_cgi(extras={
            f"cgi.{env.http_tld}": f"ScriptAlias /cgi-bin/ {env.gen_dir}",
        })
        conf.install()
        assert env.apache_restart() == 0

    # check handling of url encodings that are accepted
    @pytest.mark.parametrize("path", [
        "/006/006.css",
        "/%30%30%36/%30%30%36.css",
        "/nothing/../006/006.css",
        "/nothing/./../006/006.css",
    ])
    def test_203_01(self, env, path):
        url = env.mkurl("https", "test1", path)
        r = env.curl_get(url)
        assert r.response["status"] == 200

    # check handling of / normalization
    @pytest.mark.parametrize("path", [
        "/006//006.css",
        "/006//////////006.css",
        "/006////.//////006.css",
        "/006/../006/006.css",
    ])
    def test_203_03(self, env, path):
        url = env.mkurl("https", "test1", path)
        r = env.curl_get(url)
        assert r.response["status"] == 200

    # check path traversals
    @pytest.mark.parametrize(["path", "status"], [
        ["/../cgi/echo.py", 404],
        ["/nothing/%%32%65%%32%65/echo.py", 404],
        ["/nothing/%%32%65%%32%65/echo.py", 404],
        ["/cgi-bin/%%32%65%%32%65/echo.py", 404],
        ["/cgi-bin/%%32%65%%32%65/%%32%65%%32%65/h2_env.py", 404],
    ])
    def test_203_04(self, env, path, status):
        url = env.mkurl("https", "cgi", path)
        r = env.curl_get(url)
        assert r.response["status"] == status

    # check handling of %2f url encodings that are not decoded by default
    @pytest.mark.parametrize(["host", "path", "status"], [
        ["test1", "/006%2f006.css", 404],
        ["test2", "/006%2f006.css", 200],
    ])
    def test_203_20(self, env, host, path, status):
        url = env.mkurl("https", host, path)
        r = env.curl_get(url)
        assert r.response["status"] == status

