"""
RFB protocol handshake tests.

Verifies:
- RFB 3.3: security type sent as U32 (not a list)
- RFB 3.3: no-auth server sends security type 1 as U32
- RFB 3.3: auth failure SecurityResult is just U32 (no reason string)
- RFB 3.3: auth success SecurityResult is U32(0)
- RFB 3.7: auth failure SecurityResult is just U32 (no reason string)
- RFB 3.8: security type sent as a list
- RFB 3.8: auth failure SecurityResult includes reason string
- VeNCrypt: type 19 in RFB 3.8 security list
- VeNCrypt: X509_PLAIN auth success with correct credentials
- VeNCrypt: X509_PLAIN auth failure with wrong password
- VeNCrypt: X509_PLAIN auth failure with wrong username
- ExtendedDesktopSize: the layout answers a non-incremental update request
- ExtendedDesktopSize: the layout update carries no framebuffer data
- ExtendedDesktopSize: no layout rect for a client that did not request it
- ExtendedDesktopSize: a server-side resize sends the layout on its own

Usage: python3 test_rfb_handshake.py <path-to-rfb-test-server>
"""

import os
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


SERVER_BIN = None


class ServerProcess:
    def __init__(self, auth_mode='none', password=None, username=None):
        self.port = find_free_port()
        self._tmpdir = None
        args = [SERVER_BIN, '--port', str(self.port), '--auth-mode', auth_mode]
        if password:
            args += ['--password', password]
        if username:
            args += ['--username', username]
        if auth_mode == 'vencrypt':
            self._tmpdir = tempfile.mkdtemp(prefix='rfb-test-tls-')
            cert_path = os.path.join(self._tmpdir, 'cert.pem')
            key_path = os.path.join(self._tmpdir, 'key.pem')
            subprocess.run([
                'openssl', 'req', '-x509', '-newkey', 'rsa:2048',
                '-keyout', key_path, '-out', cert_path,
                '-days', '1', '-nodes', '-subj', '/CN=test',
            ], check=True, capture_output=True)
            args += ['--tls-cert', cert_path, '--tls-key', key_path]
        self.proc = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Read lines until we get the READY line (skip debug output)
        while True:
            line = self.proc.stdout.readline().decode().strip()
            if line.startswith('READY'):
                break
            if not line:
                self.proc.kill()
                stderr = self.proc.stderr.read().decode()
                raise RuntimeError(
                    f'Server exited without printing READY\n'
                    f'stderr: {stderr}')

    def resize(self):
        """Make the server resize its desktop to RESIZED_FB_*."""
        self.proc.send_signal(signal.SIGUSR1)

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        self.proc.wait(timeout=5)
        if self._tmpdir:
            import shutil
            shutil.rmtree(self._tmpdir, ignore_errors=True)


def vnc_des_encrypt(challenge_hex, password):
    """Use rfb-test-server --encrypt-challenge to compute DES response."""
    result = subprocess.run(
        [SERVER_BIN, '--encrypt-challenge', challenge_hex,
         '--password', password],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f'encrypt failed: {result.stderr}')
    return bytes.fromhex(result.stdout.strip())


class RFBConnection:
    def __init__(self, port, version=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect(('127.0.0.1', port))
        if version:
            self.recv_exact(12)  # server version
            self.sock.sendall(version.encode())

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.sock.close()

    def recv_exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError(
                    f'Connection closed, got {len(data)}/{n} bytes')
            data += chunk
        return data

    def recv_all(self, timeout=2):
        """Read all remaining data until EOF or timeout."""
        self.sock.settimeout(timeout)
        data = b''
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data

    def read_security_type_u32(self):
        data = self.recv_exact(4)
        return struct.unpack('!I', data)[0]

    def read_security_type_list(self):
        count = struct.unpack('!B', self.recv_exact(1))[0]
        if count == 0:
            raise RuntimeError('Server sent 0 security types')
        types = list(self.recv_exact(count))
        return types

    def read_des_challenge(self):
        return self.recv_exact(16)

    def send_des_response(self, response):
        self.sock.sendall(response)

    def choose_security_type(self, sec_type):
        self.sock.sendall(struct.pack('!B', sec_type))

    def read_security_result(self):
        data = self.recv_exact(4)
        return struct.unpack('!I', data)[0]

    # VeNCrypt helpers
    def read_vencrypt_version(self):
        data = self.recv_exact(2)
        return struct.unpack('!BB', data)

    def send_vencrypt_version(self, major, minor):
        self.sock.sendall(struct.pack('!BB', major, minor))

    def read_vencrypt_subtypes(self):
        ok = struct.unpack('!B', self.recv_exact(1))[0]
        n = struct.unpack('!B', self.recv_exact(1))[0]
        types = []
        for _ in range(n):
            t = struct.unpack('!I', self.recv_exact(4))[0]
            types.append(t)
        return ok, types

    def send_vencrypt_subtype(self, subtype):
        self.sock.sendall(struct.pack('!I', subtype))

    def read_vencrypt_subtype_ok(self):
        return struct.unpack('!B', self.recv_exact(1))[0]

    def upgrade_to_tls(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        self.sock = ctx.wrap_socket(self.sock)

    def send_vencrypt_plain_auth(self, username, password):
        u = username.encode('utf-8')
        p = password.encode('utf-8')
        self.sock.sendall(struct.pack('!II', len(u), len(p)) + u + p)

    def do_vencrypt_handshake(self):
        """Complete VeNCrypt negotiation through TLS upgrade."""
        types = self.read_security_type_list()
        assert 19 in types, f'VeNCrypt (19) not in security types: {types}'
        self.choose_security_type(19)

        major, minor = self.read_vencrypt_version()
        assert (major, minor) == (0, 2), f'Unexpected VeNCrypt version {major}.{minor}'
        self.send_vencrypt_version(0, 2)

        ok, subtypes = self.read_vencrypt_subtypes()
        assert ok == 0, f'VeNCrypt version not accepted: {ok}'
        assert 262 in subtypes, f'X509_PLAIN (262) not in subtypes: {subtypes}'
        self.send_vencrypt_subtype(262)

        subtype_ok = self.read_vencrypt_subtype_ok()
        assert subtype_ok == 1, f'Subtype not accepted: {subtype_ok}'
        self.upgrade_to_tls()

    def read_security_result_failure(self):
        """Read SecurityResult, expecting failure with reason string."""
        data = self.recv_all()
        assert len(data) > 4, f'Expected SecurityResult with reason, got {len(data)} bytes'
        result = struct.unpack('!I', data[:4])[0]
        assert result == 1, f'Expected SecurityResult 1, got {result}'
        reason_len = struct.unpack('!I', data[4:8])[0]
        assert reason_len > 0, 'Expected non-empty reason string'
        return result, data[8:8 + reason_len].decode('utf-8', errors='replace')

    # Post-authentication helpers

    def send_client_init(self, shared=1):
        self.sock.sendall(struct.pack('!B', shared))

    def read_server_init(self):
        """Read ServerInit, returning (width, height, name).

        The pixel format is retained as bytes_per_pixel, which is needed to
        consume Raw rect payloads.
        """
        width, height = struct.unpack('!HH', self.recv_exact(4))
        pixel_format = self.recv_exact(16)
        self.bytes_per_pixel = pixel_format[0] // 8
        name_len = struct.unpack('!I', self.recv_exact(4))[0]
        name = self.recv_exact(name_len).decode('utf-8', errors='replace')
        return width, height, name

    def send_set_encodings(self, encodings):
        self.sock.sendall(struct.pack('!BBH', 2, 0, len(encodings)))
        for encoding in encodings:
            self.sock.sendall(struct.pack('!i', encoding))

    def send_fb_update_request(self, incremental, x, y, width, height):
        self.sock.sendall(struct.pack('!BBHHHH', 3, incremental, x, y,
                                      width, height))

    def read_framebuffer_update(self):
        """Read one FramebufferUpdate, returning a list of parsed rects.

        Raw is the only pixel encoding handled, since that is all these tests
        ask for. Any other encoding would need its payload consumed by
        encoding, so hitting one here is a test bug rather than something to
        skip past.
        """
        msg_type = struct.unpack('!B', self.recv_exact(1))[0]
        assert msg_type == 0, f'Expected FramebufferUpdate, got message {msg_type}'
        self.recv_exact(1)  # padding
        n_rects = struct.unpack('!H', self.recv_exact(2))[0]

        rects = []
        for _ in range(n_rects):
            x, y, width, height, encoding = struct.unpack('!HHHHi',
                                                          self.recv_exact(12))
            rect = {
                'encoding': encoding,
                'width': width,
                'height': height,
            }
            if encoding == ENCODING_EXTENDED_DESKTOP_SIZE:
                # x and y carry the reason and status for this pseudo-encoding.
                rect['reason'] = x
                rect['status'] = y
                n_screens = struct.unpack('!B', self.recv_exact(1))[0]
                self.recv_exact(3)  # padding
                rect['screens'] = [
                    dict(zip(('id', 'x', 'y', 'width', 'height', 'flags'),
                             struct.unpack('!IHHHHI', self.recv_exact(16))))
                    for _ in range(n_screens)
                ]
            elif encoding == ENCODING_RAW:
                self.recv_exact(width * height * self.bytes_per_pixel)
            elif encoding != ENCODING_DESKTOP_SIZE:
                raise AssertionError(f'Unexpected encoding {encoding} in update')
            rects.append(rect)
        return rects

    def handshake_to_encodings(self):
        """Take a no-auth RFB 3.8 connection as far as ServerInit."""
        assert SECURITY_TYPE_NONE in self.read_security_type_list()
        self.choose_security_type(SECURITY_TYPE_NONE)
        assert self.read_security_result() == 0
        self.send_client_init()
        return self.read_server_init()


# ---------------------------------------------------------------------------
# Test classes
# ---------------------------------------------------------------------------

PASSWORD = 'testpass'

SECURITY_TYPE_NONE = 1
ENCODING_RAW = 0
ENCODING_DESKTOP_SIZE = -223
ENCODING_QEMU_EXT_KEY_EVENT = -258
ENCODING_EXTENDED_DESKTOP_SIZE = -308

# rfb-test-server feeds a single 64x64 frame from one display at 0,0, and
# resizes to 128x96 on SIGUSR1.
TEST_FB_WIDTH = 64
TEST_FB_HEIGHT = 64
RESIZED_FB_WIDTH = 128
RESIZED_FB_HEIGHT = 96


class TestDESAuth(unittest.TestCase):
    auth_server = None
    noauth_server = None

    @classmethod
    def setUpClass(cls):
        cls.auth_server = ServerProcess('des', PASSWORD)
        cls.noauth_server = ServerProcess('none')

    @classmethod
    def tearDownClass(cls):
        cls.auth_server.stop()
        cls.noauth_server.stop()

    def test_rfb33_gets_security_type_u32(self):
        """RFB 3.3: server sends security type as U32, should be 2 (VNC Auth)."""
        with RFBConnection(self.auth_server.port, 'RFB 003.003\n') as c:
            sec_type = c.read_security_type_u32()
            self.assertEqual(sec_type, 2)
            self.assertEqual(len(c.read_des_challenge()), 16)

    def test_rfb33_noauth_gets_security_none(self):
        """RFB 3.3 with no auth: server sends U32(1) = NONE."""
        with RFBConnection(self.noauth_server.port, 'RFB 003.003\n') as c:
            self.assertEqual(c.read_security_type_u32(), 1)

    def test_rfb33_auth_failure_no_reason(self):
        """RFB 3.3 auth failure: SecurityResult is just U32(1), no reason."""
        with RFBConnection(self.auth_server.port, 'RFB 003.003\n') as c:
            c.read_security_type_u32()
            c.read_des_challenge()
            c.send_des_response(b'\x00' * 16)
            data = c.recv_all()
            self.assertEqual(len(data), 4, f'Expected 4 bytes, got {len(data)}')
            self.assertEqual(struct.unpack('!I', data)[0], 1)

    def test_rfb33_auth_success(self):
        """RFB 3.3 auth success: SecurityResult U32(0)."""
        with RFBConnection(self.auth_server.port, 'RFB 003.003\n') as c:
            c.read_security_type_u32()
            challenge = c.read_des_challenge()
            c.send_des_response(vnc_des_encrypt(challenge.hex(), PASSWORD))
            self.assertEqual(c.read_security_result(), 0)

    def test_rfb37_auth_failure_no_reason(self):
        """RFB 3.7 auth failure: SecurityResult is just U32(1), no reason."""
        with RFBConnection(self.auth_server.port, 'RFB 003.007\n') as c:
            self.assertIn(2, c.read_security_type_list())
            c.choose_security_type(2)
            c.read_des_challenge()
            c.send_des_response(b'\x00' * 16)
            data = c.recv_all()
            self.assertEqual(len(data), 4, f'Expected 4 bytes, got {len(data)}')
            self.assertEqual(struct.unpack('!I', data)[0], 1)

    def test_rfb38_gets_security_type_list(self):
        """RFB 3.8: server sends security type as count + list, includes type 2."""
        with RFBConnection(self.auth_server.port, 'RFB 003.008\n') as c:
            self.assertIn(2, c.read_security_type_list())

    def test_rfb38_auth_failure_with_reason(self):
        """RFB 3.8 auth failure: SecurityResult has U32(1) + reason string."""
        with RFBConnection(self.auth_server.port, 'RFB 003.008\n') as c:
            self.assertIn(2, c.read_security_type_list())
            c.choose_security_type(2)
            c.read_des_challenge()
            c.send_des_response(b'\x00' * 16)
            c.read_security_result_failure()

    def test_rfb38_auth_success(self):
        """RFB 3.8 DES auth success: SecurityResult U32(0)."""
        with RFBConnection(self.auth_server.port, 'RFB 003.008\n') as c:
            self.assertIn(2, c.read_security_type_list())
            c.choose_security_type(2)
            challenge = c.read_des_challenge()
            c.send_des_response(vnc_des_encrypt(challenge.hex(), PASSWORD))
            self.assertEqual(c.read_security_result(), 0)

    def test_rfb37_noauth_skips_security_result(self):
        """RFB 3.7 no-auth: choose type 1, server sends ServerInit (no SecurityResult)."""
        with RFBConnection(self.noauth_server.port, 'RFB 003.007\n') as c:
            types = c.read_security_type_list()
            self.assertIn(1, types)
            c.choose_security_type(1)
            # Send ClientInit (shared=1)
            c.sock.sendall(struct.pack('!B', 1))
            # Should get ServerInit directly (width U16 + height U16 = 64x64)
            data = c.recv_exact(4)
            w, h = struct.unpack('!HH', data)
            self.assertEqual(w, 64)
            self.assertEqual(h, 64)

    def test_rfb38_noauth_sends_security_result(self):
        """RFB 3.8 no-auth: choose type 1, server sends SecurityResult then ServerInit."""
        with RFBConnection(self.noauth_server.port, 'RFB 003.008\n') as c:
            types = c.read_security_type_list()
            self.assertIn(1, types)
            c.choose_security_type(1)
            # Should get SecurityResult U32(0) first
            self.assertEqual(c.read_security_result(), 0)
            # Send ClientInit (shared=1)
            c.sock.sendall(struct.pack('!B', 1))
            # Then ServerInit
            data = c.recv_exact(4)
            w, h = struct.unpack('!HH', data)
            self.assertEqual(w, 64)
            self.assertEqual(h, 64)


VENCRYPT_USERNAME = 'testuser'


class TestVeNCrypt(unittest.TestCase):
    server = None

    @classmethod
    def setUpClass(cls):
        cls.server = ServerProcess('vencrypt', PASSWORD,
                                   username=VENCRYPT_USERNAME)

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def _connect_vencrypt(self):
        """RFB 3.8 handshake + full VeNCrypt negotiation through TLS."""
        c = RFBConnection(self.server.port, 'RFB 003.008\n')
        c.do_vencrypt_handshake()
        return c

    def test_vencrypt_in_security_list(self):
        """RFB 3.8: security type list includes VeNCrypt (19)."""
        with RFBConnection(self.server.port, 'RFB 003.008\n') as c:
            self.assertIn(19, c.read_security_type_list())

    def test_vencrypt_auth_success(self):
        """VeNCrypt X509_PLAIN: correct credentials -> SecurityResult 0."""
        with self._connect_vencrypt() as c:
            c.send_vencrypt_plain_auth(VENCRYPT_USERNAME, PASSWORD)
            result = c.read_security_result()
            self.assertEqual(result, 0)

    def test_vencrypt_auth_failure(self):
        """VeNCrypt X509_PLAIN: wrong password -> SecurityResult 1 + reason."""
        with self._connect_vencrypt() as c:
            c.send_vencrypt_plain_auth(VENCRYPT_USERNAME, 'wrongpass')
            c.read_security_result_failure()

    def test_vencrypt_wrong_username(self):
        """VeNCrypt X509_PLAIN: wrong username -> SecurityResult 1 + reason."""
        with self._connect_vencrypt() as c:
            c.send_vencrypt_plain_auth('wronguser', PASSWORD)
            c.read_security_result_failure()


class TestAuthBypass(unittest.TestCase):
    """Test that selecting an unsupported security type is rejected."""
    auth_server = None
    noauth_server = None

    @classmethod
    def setUpClass(cls):
        cls.auth_server = ServerProcess('des', PASSWORD)
        cls.noauth_server = ServerProcess('none')

    @classmethod
    def tearDownClass(cls):
        cls.auth_server.stop()
        cls.noauth_server.stop()

    def test_rfb38_select_none_when_auth_required(self):
        """Selecting NONE (1) on auth server should fail."""
        with RFBConnection(self.auth_server.port, 'RFB 003.008\n') as c:
            types = c.read_security_type_list()
            self.assertNotIn(1, types)
            c.choose_security_type(1)
            c.read_security_result_failure()

    def test_rfb38_select_vnc_auth_when_noauth(self):
        """Selecting VNC Auth (2) on no-auth server should fail."""
        with RFBConnection(self.noauth_server.port, 'RFB 003.008\n') as c:
            types = c.read_security_type_list()
            self.assertNotIn(2, types)
            c.choose_security_type(2)
            c.read_security_result_failure()

    def test_rfb38_select_invalid_type(self):
        """Selecting a completely invalid type (255) should fail."""
        with RFBConnection(self.auth_server.port, 'RFB 003.008\n') as c:
            c.read_security_type_list()
            c.choose_security_type(255)
            c.read_security_result_failure()

    def test_rfb37_select_none_when_auth_required(self):
        """RFB 3.7: selecting NONE (1) on auth server should fail."""
        with RFBConnection(self.auth_server.port, 'RFB 003.007\n') as c:
            types = c.read_security_type_list()
            self.assertNotIn(1, types)
            c.choose_security_type(1)
            # RFB 3.7: failure is just U32, no reason string
            data = c.recv_all()
            self.assertEqual(len(data), 4, f'Expected 4 bytes, got {len(data)}')
            self.assertEqual(struct.unpack('!I', data)[0], 1)


class TestExtendedDesktopSize(unittest.TestCase):
    """A non-incremental update request must be answered with the layout.

    The spec requires the server to send an ExtendedDesktopSize rect in reply
    to a FramebufferUpdateRequest with incremental set to zero, since that is
    the only way a client learns that SetDesktopSize is supported. Clients that
    latch this at their first framebuffer update -- TurboVNC does so
    permanently -- otherwise conclude that resizing is unsupported.
    """

    server = None

    @classmethod
    def setUpClass(cls):
        cls.server = ServerProcess('none')

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_layout_answers_a_non_incremental_request(self):
        """The layout must come before the other pseudo-rect updates.

        QEMUExtendedKeyEvent makes the server want to send an ext-support
        frame, which used to consume the request and answer it with an update
        that had no ExtendedDesktopSize rect in it.
        """
        with RFBConnection(self.server.port, 'RFB 003.008\n') as c:
            width, height, _ = c.handshake_to_encodings()
            self.assertEqual((width, height), (TEST_FB_WIDTH, TEST_FB_HEIGHT))

            c.send_set_encodings([ENCODING_RAW,
                                  ENCODING_EXTENDED_DESKTOP_SIZE,
                                  ENCODING_QEMU_EXT_KEY_EVENT])
            c.send_fb_update_request(0, 0, 0, width, height)

            rects = c.read_framebuffer_update()

            self.assertEqual(len(rects), 1,
                             'the layout must arrive before anything else')
            rect = rects[0]
            self.assertEqual(rect['encoding'], ENCODING_EXTENDED_DESKTOP_SIZE)
            self.assertEqual(rect['reason'], 0, 'should be server-initiated')
            self.assertEqual(rect['status'], 0, 'should report success')
            self.assertEqual((rect['width'], rect['height']),
                             (TEST_FB_WIDTH, TEST_FB_HEIGHT))
            self.assertEqual(len(rect['screens']), 1)
            self.assertEqual(
                (rect['screens'][0]['width'], rect['screens'][0]['height']),
                (TEST_FB_WIDTH, TEST_FB_HEIGHT))

    def test_layout_update_carries_no_framebuffer_data(self):
        """An update holding the layout must contain nothing else."""
        with RFBConnection(self.server.port, 'RFB 003.008\n') as c:
            width, height, _ = c.handshake_to_encodings()

            c.send_set_encodings([ENCODING_RAW, ENCODING_EXTENDED_DESKTOP_SIZE])
            c.send_fb_update_request(0, 0, 0, width, height)

            rects = c.read_framebuffer_update()

            self.assertEqual([rect['encoding'] for rect in rects],
                             [ENCODING_EXTENDED_DESKTOP_SIZE])

    def test_no_layout_rect_without_the_encoding(self):
        """A client that cannot parse the rect must not be sent one."""
        with RFBConnection(self.server.port, 'RFB 003.008\n') as c:
            width, height, _ = c.handshake_to_encodings()

            c.send_set_encodings([ENCODING_RAW])
            c.send_fb_update_request(0, 0, 0, width, height)

            rects = c.read_framebuffer_update()

            self.assertTrue(rects, 'expected framebuffer data')
            for rect in rects:
                self.assertEqual(rect['encoding'], ENCODING_RAW)


class TestServerInitiatedResize(unittest.TestCase):
    """A resize on the server sends the layout in an update of its own.

    The spec forbids an update containing an ExtendedDesktopSize rect from
    carrying any framebuffer data, neither before nor after the rect.
    """

    def test_resize_layout_arrives_without_framebuffer_data(self):
        server = ServerProcess('none')
        try:
            with RFBConnection(server.port, 'RFB 003.008\n') as c:
                width, height, _ = c.handshake_to_encodings()

                c.send_set_encodings([ENCODING_RAW,
                                      ENCODING_EXTENDED_DESKTOP_SIZE])

                # Drain the layout that answers the non-incremental request.
                c.send_fb_update_request(0, 0, 0, width, height)
                self.assertEqual(c.read_framebuffer_update()[0]['width'],
                                 TEST_FB_WIDTH)

                server.resize()

                rect = None
                for _ in range(8):
                    c.send_fb_update_request(1, 0, 0, width, height)
                    rects = c.read_framebuffer_update()
                    if rects[0]['encoding'] != ENCODING_EXTENDED_DESKTOP_SIZE:
                        continue
                    self.assertEqual(len(rects), 1,
                                     'the resize must not carry pixel data')
                    rect = rects[0]
                    break

                self.assertIsNotNone(rect, 'no layout rect after the resize')
                self.assertEqual(rect['reason'], 0, 'should be server-initiated')
                self.assertEqual(rect['status'], 0, 'should report success')
                self.assertEqual((rect['width'], rect['height']),
                                 (RESIZED_FB_WIDTH, RESIZED_FB_HEIGHT))
        finally:
            server.stop()


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <path-to-rfb-test-server>', file=sys.stderr)
        sys.exit(1)
    SERVER_BIN = sys.argv[1]
    # Remove our arg so unittest doesn't see it
    sys.argv = sys.argv[:1]
    unittest.main(verbosity=2)
