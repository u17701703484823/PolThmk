###################################################################################################
# h2 end-to-end test environment class
#
# (c) 2018 greenbytes GmbH
###################################################################################################

import json
import pytest
import re
import os
import shutil
import subprocess
import sys
import time
import requests

from datetime import datetime
from datetime import tzinfo
from datetime import timedelta
from ConfigParser import SafeConfigParser
from shutil import copyfile
from urlparse import urlparse

class TestEnv:

    @classmethod
    def init( cls ) :
        cls.config = SafeConfigParser()
        cls.config.read('config.ini')
        
        cls.PREFIX      = cls.config.get('global', 'prefix')
        cls.GEN_DIR     = cls.config.get('global', 'gen_dir')
        cls.WEBROOT     = cls.config.get('global', 'server_dir')
        cls.CURL        = cls.config.get('global', 'curl_bin')
        cls.TEST_DIR    = cls.config.get('global', 'test_dir')

        cls.HTTP_PORT   = cls.config.get('httpd', 'http_port')
        cls.HTTPS_PORT  = cls.config.get('httpd', 'https_port')
        cls.HTTP_TLD    = cls.config.get('httpd', 'http_tld')

        cls.APACHECTL  = os.path.join(cls.PREFIX, 'bin', 'apachectl')

        cls.HTTPD_ADDR = "127.0.0.1"
        cls.HTTP_URL   = "http://" + cls.HTTPD_ADDR + ":" + cls.HTTP_PORT
        cls.HTTPS_URL  = "https://" + cls.HTTPD_ADDR + ":" + cls.HTTPS_PORT
        
        cls.HTTPD_CONF_DIR = os.path.join(cls.WEBROOT, "conf")
        cls.HTTPD_TEST_CONF = os.path.join(cls.HTTPD_CONF_DIR, "test.conf")
        cls.E2E_DIR    = os.path.join(cls.TEST_DIR, "e2e")

        cls.VERIFY_CERTIFICATES = False
        
        if not os.path.exists(cls.GEN_DIR):
            os.makedirs(cls.GEN_DIR)

###################################################################################################
# path construction
#
    @classmethod
    def e2e_src( cls, path ) :
        return os.path.join(cls.E2E_DIR, path)

###################################################################################################
# command execution
#
    @classmethod
    def run( cls, args, input=None ) :
        print ("execute: %s" % " ".join(args))
        p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (output, errput) = p.communicate(input)
        rv = p.wait()
        print ("stderr: %s" % errput)
        try:
            jout = json.loads(output)
        except:
            jout = None
            print ("stdout: %s" % output)
        return { 
            "rv": rv,
            "out" : {
                "text" : output,
                "err" : errput,
                "json" : jout
            } 
        }

    @classmethod
    def mkurl( cls, scheme, hostname, path='/' ) :
        port = cls.HTTPS_PORT if scheme == 'https' else cls.HTTP_PORT
        return "%s://%s.%s:%s%s" % (scheme, hostname, cls.HTTP_TLD, port, path)

###################################################################################################
# http methods
#
    @classmethod
    def is_live( cls, url, timeout ) :
        s = requests.Session()
        try_until = time.time() + timeout
        print("checking reachability of %s" % url)
        while time.time() < try_until:
            try:
                req = requests.Request('HEAD', url).prepare()
                resp = s.send(req, verify=cls.VERIFY_CERTIFICATES, timeout=timeout)
                return True
            except IOError:
                print ("connect error: %s" % sys.exc_info()[0])
                time.sleep(.2)
            except:
                print ("Unexpected error: %s" % sys.exc_info()[0])
                time.sleep(.2)
        print ("Unable to contact '%s' after %d sec" % (url, timeout))
        return False

    @classmethod
    def is_dead( cls, url, timeout ) :
        s = requests.Session()
        try_until = time.time() + timeout
        print("checking reachability of %s" % url)
        while time.time() < try_until:
            try:
                req = requests.Request('HEAD', url).prepare()
                resp = s.send(req, verify=cls.VERIFY_CERTIFICATES, timeout=timeout)
                time.sleep(.2)
            except IOError:
                return True
            except:
                return True
        print ("Server still responding after %d sec" % timeout)
        return False

    @classmethod
    def get_json( cls, url, timeout ) :
        data = cls.get_plain( url, timeout )
        if data:
            return json.loads(data)
        return None

    @classmethod
    def get_plain( cls, url, timeout ) :
        s = requests.Session()
        try_until = time.time() + timeout
        while time.time() < try_until:
            try:
                req = requests.Request('GET', url).prepare()
                resp = s.send(req, verify=cls.VERIFY_CERTIFICATES, timeout=timeout)
                return resp.text
            except IOError:
                print ("connect error: %s" % sys.exc_info()[0])
                time.sleep(.1)
            except:
                print ("Unexpected error: %s" % sys.exc_info()[0])
                return None
        print ("Unable to contact server after %d sec" % timeout)
        return None

###################################################################################################
# apachectl
#
    @classmethod
    def apachectl( cls, cmd, conf=None, check_live=True ) :
        if conf:
            cls.install_test_conf(conf)
        args = [cls.APACHECTL, "-d", cls.WEBROOT, "-k", cmd]
        print ("execute: %s" % " ".join(args))
        cls.apachectl_stderr = ""
        p = subprocess.Popen(args, stderr=subprocess.PIPE)
        (output, cls.apachectl_stderr) = p.communicate()
        sys.stderr.write(cls.apachectl_stderr)
        rv = p.wait()
        if rv == 0:
            if check_live:
                rv = 0 if cls.is_live(cls.HTTP_URL, 10) else -1
            else:
                rv = 0 if cls.is_dead(cls.HTTP_URL, 10) else -1
                print ("waited for a apache.is_dead, rv=%d" % rv)
        return rv

    @classmethod
    def apache_restart( cls ) :
        return cls.apachectl( "graceful" )
        
    @classmethod
    def apache_start( cls ) :
        return cls.apachectl( "start" )

    @classmethod
    def apache_stop( cls ) :
        return cls.apachectl( "stop", check_live=False )

    @classmethod
    def apache_fail( cls ) :
        rv = cls.apachectl( "graceful", check_live=False )
        if rv != 0:
            print ("check, if dead: %s" % cls.HTTPD_CHECK_URL)
            return 0 if cls.is_dead(cls.HTTPD_CHECK_URL, 5) else -1
        return rv
        
    @classmethod
    def install_test_conf( cls, conf=None) :
        if conf is None:
            conf_src = os.path.join("conf", "test.conf")
        elif os.path.isabs(conf):
            conf_src = conf
        else:
            conf_src = os.path.join("data", conf + ".conf")
        copyfile(conf_src, cls.HTTPD_TEST_CONF)

###################################################################################################
# curl
#
    @classmethod
    def curl_raw( cls, url, timeout, options ) :
        u = urlparse(url)
        headerfile = ("%s/curl.headers" % cls.GEN_DIR)
        if os.path.isfile(headerfile):
            os.remove(headerfile)

        args = [ 
            cls.CURL,
            "-ks", "-D", headerfile, 
            "--resolve", ("%s:%s:%s" % (u.hostname, u.port, cls.HTTPD_ADDR)),
            "--connect-timeout", ("%d" % timeout) 
        ]
        if options:
            args.extend(options)
        args.append( url )
        r = cls.run( args )
        if r["rv"] == 0:
            lines = open(headerfile).readlines()
            exp_stat = True
            header = {}
            for line in lines:
                if exp_stat:
                    m = re.match(r'(\S+) (\d+) (.*)\r\n', line)
                    assert m
                    r["response"] = {
                        "protocol"    : m.group(1), 
                        "status"      : int(m.group(2)), 
                        "description" : m.group(3),
                        "body"        : r["out"]["text"]
                    }
                    exp_stat = False
                    header = {}
                elif line == "\r\n":
                    exp_stat = True
                else:
                    m = re.match(r'([^:]+):\s*(.*)\r\n', line)
                    assert m
                    header[ m.group(1).lower() ] = m.group(2)
            r["response"]["header"] = header
            if r["out"]["json"]:
                r["response"]["json"] = r["out"]["json"] 
        return r

    @classmethod
    def curl_get( cls, url, timeout=5, options=None ) :
        return cls.curl_raw( url, timeout=timeout, options=options )

    @classmethod
    def curl_upload( cls, url, fpath, timeout=5, options=None ) :
        fname = os.path.basename(fpath)
        if not options:
            options = []
        options.extend([
            "--form", ("file=@%s" % (fpath))
        ])
        return cls.curl_raw( url, timeout, options )
        
###################################################################################################
# some standard config setups
#
    @classmethod
    def vhost_cgi_install( cls ) :
        conf = HttpdConf()
        conf.start_vhost( TestEnv.HTTPS_PORT, "cgi", aliasList=[], docRoot="htdocs/cgi", withSSL=True)
        conf.add_line("      Protocols h2 http/1.1")
        conf.add_line("      SSLOptions +StdEnvVars")
        conf.add_line("      AddHandler cgi-script .py")
        conf.end_vhost()
        conf.install()
    

###################################################################################################
# write apache config file
#
class HttpdConf(object):

    def __init__(self, path=None):
        if path:
            self.path = path
        else:
            self.path = os.path.join(TestEnv.GEN_DIR, "auto.conf")
        if os.path.isfile(self.path):
            os.remove(self.path)
        open(self.path, "a").write("")

    def add_line(self, line):
        open(self.path, "a").write(line + "\n")

    def add_vhost(self, port, name, aliasList, docRoot="htdocs", withSSL=True):
        self.start_vhost(port, name, aliasList, docRoot, withSSL)
        self.end_vhost()

    def start_vhost(self, port, name, aliasList, docRoot="htdocs", withSSL=True):
        f = open(self.path, "a") 
        f.write("<VirtualHost *:%s>\n" % port)
        f.write("    ServerName %s.%s\n" % (name, TestEnv.HTTP_TLD) )
        if len(aliasList) > 0:
            for alias in aliasList:
                f.write("    ServerAlias %s.%s\n" % (alias, TestEnv.HTTP_TLD) )
        f.write("    DocumentRoot %s\n\n" % docRoot)
        if withSSL:
            f.write("    SSLEngine on\n")
                  
    def end_vhost(self):
        self.add_line("</VirtualHost>\n\n")

    def install(self):
        TestEnv.install_test_conf(self.path)
