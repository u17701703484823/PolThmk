from typing import Dict, Any

from pyhttpd.env import HttpdTestEnv


class HttpdConf(object):

    def __init__(self, env: HttpdTestEnv, extras: Dict[str, Any] = None):
        """ Create a new httpd configuration.
        :param env: then environment this operates in
        :param extras: extra configuration directive with ServerName as key and
                       'base' as special key for global configuration additions.
        """
        self.env = env
        self._indents = 0
        self._lines = []
        self._extras = extras.copy() if extras else {}
        if 'base' in self._extras:
            self.add(self._extras['base'])

    def install(self):
        self.env.install_test_conf(self._lines)

    def add(self, line: Any):
        if isinstance(line, str):
            if self._indents > 0:
                line = f"{'  ' * self._indents}{line}"
            self._lines.append(line)
        else:
            if self._indents > 0:
                line = [f"{'  ' * self._indents}{l}" for l in line]
            self._lines.extend(line)
        return self

    def add_certificate(self, cert_file, key_file):
        if self.env.ssl_module == "ssl":
            self.add([
                f"SSLCertificateFile {cert_file}",
                f"SSLCertificateKeyFile {key_file}",
            ])
        elif self.env.ssl_module == "tls":
            self.add(f"""
                TLSCertificate {cert_file} {key_file}
            """)

    def add_vhost(self, domains, port=None, doc_root="htdocs", with_ssl=True):
        self.start_vhost(domains=domains, port=port, doc_root=doc_root, with_ssl=with_ssl)
        self.end_vhost()
        return self

    def start_vhost(self, domains, port=None, doc_root="htdocs", with_ssl=False):
        if not isinstance(domains, list):
            domains = [domains]
        if port is None:
            port = self.env.https_port
        self.add("")
        self.add(f"<VirtualHost *:{port}>")
        self._indents += 1
        self.add(f"ServerName {domains[0]}")
        for alias in domains[1:]:
            self.add(f"ServerAlias {alias}")
        self.add(f"DocumentRoot {doc_root}")
        if self.env.https_port == port or with_ssl:
            if self.env.ssl_module == "ssl":
                self.add("SSLEngine on")
            for cred in self.env.get_credentials_for_name(domains[0]):
                self.add_certificate(cred.cert_file, cred.pkey_file)
        if domains[0] in self._extras:
            self.add(self._extras[domains[0]])
        return self
                  
    def end_vhost(self):
        self._indents -= 1
        self.add("</VirtualHost>")
        self.add("")
        return self

    def add_proxies(self, host, proxy_self=False, h2proxy_self=False):
        if proxy_self or h2proxy_self:
            self.add("ProxyPreserveHost on")
        if proxy_self:
            self.add([
                f"ProxyPass /proxy/ http://127.0.0.1:{self.env.http_port}/",
                f"ProxyPassReverse /proxy/ http://{host}.{self.env.http_tld}:{self.env.http_port}/",
            ])
        if h2proxy_self:
            self.add([
                f"ProxyPass /h2proxy/ h2://127.0.0.1:{self.env.https_port}/",
                f"ProxyPassReverse /h2proxy/ https://{host}.{self.env.http_tld}:self.env.https_port/",
            ])
        return self
    
    def add_vhost_test1(self, proxy_self=False, h2proxy_self=False):
        domain = f"test1.{self.env.http_tld}"
        self.start_vhost(domains=[domain, f"www1.{self.env.http_tld}"],
                         port=self.env.http_port, doc_root="htdocs/test1")
        self.end_vhost()
        self.start_vhost(domains=[domain, f"www1.{self.env.http_tld}"],
                         port=self.env.https_port, doc_root="htdocs/test1")
        self.add([
            "<Location /006>",
            "    Options +Indexes",
            "    HeaderName /006/header.html",
            "</Location>",
        ])
        self.add_proxies("test1", proxy_self, h2proxy_self)
        self.end_vhost()
        return self

    def add_vhost_test2(self):
        domain = f"test2.{self.env.http_tld}"
        self.start_vhost(domains=[domain, f"www2.{self.env.http_tld}"],
                         port=self.env.http_port, doc_root="htdocs/test2")
        self.end_vhost()
        self.start_vhost(domains=[domain, f"www2.{self.env.http_tld}"],
                         port=self.env.https_port, doc_root="htdocs/test2")
        self.add([
            "<Location /006>",
            "    Options +Indexes",
            "    HeaderName /006/header.html",
            "</Location>",
        ])
        self.end_vhost()
        return self

    def add_vhost_cgi(self, proxy_self=False, h2proxy_self=False):
        domain = f"cgi.{self.env.http_tld}"
        if proxy_self:
            self.add(["ProxyStatus on", "ProxyTimeout 5",
                      "SSLProxyEngine on", "SSLProxyVerify none"])
        if h2proxy_self:
            self.add(["SSLProxyEngine on", "SSLProxyCheckPeerName off"])
        self.start_vhost(domains=[domain, f"cgi-alias.{self.env.http_tld}"],
                         port=self.env.https_port, doc_root="htdocs/cgi")
        self.add_proxies("cgi", proxy_self=proxy_self, h2proxy_self=h2proxy_self)
        self.add("<Location \"/h2test/echo\">")
        self.add("    SetHandler h2test-echo")
        self.add("</Location>")
        self.add("<Location \"/h2test/delay\">")
        self.add("    SetHandler h2test-delay")
        self.add("</Location>")
        if domain in self._extras:
            self.add(self._extras[domain])
        self.end_vhost()
        self.start_vhost(domains=[domain, f"cgi-alias.{self.env.http_tld}"],
                         port=self.env.http_port, doc_root="htdocs/cgi")
        self.add("AddHandler cgi-script .py")
        self.add_proxies("cgi", proxy_self=proxy_self, h2proxy_self=h2proxy_self)
        self.end_vhost()
        self.add("LogLevel proxy:info")
        self.add("LogLevel proxy_http:info")
        return self

    @staticmethod
    def merge_extras(e1: Dict[str, Any], e2: Dict[str, Any]) -> Dict[str, Any]:
        def _concat(v1, v2):
            if isinstance(v1, str):
                v1 = [v1]
            if isinstance(v2, str):
                v2 = [v2]
            v1.extend(v2)
            return v1

        if e1 is None:
            return e2.copy() if e2 else None
        if e2 is None:
            return e1.copy()
        e3 = e1.copy()
        for name, val in e2.items():
            if name in e3:
                e3[name] = _concat(e3[name], val)
            else:
                e3[name] = val
        return e3
