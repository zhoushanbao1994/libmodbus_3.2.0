#!/usr/bin/env python3
"""通过真实 TCP 和 Linux PTY 验证命令行、协议请求及回调输出。"""

import os
from pathlib import Path
import pty
import select
import signal
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest


# make check 默认从构建目录运行 tests，直接测试此处的 libtool 包装入口。
# 也可手动传入回传 ELF 路径，以验证部署包；该参数不传给 unittest。
PROGRAM = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else './frame-callback-test')


def run_client(*args):
    return subprocess.run([PROGRAM, *args], capture_output=True, text=True, timeout=8)


def crc16(data):
    """独立的从站协议实现；测试端不调用被测库的组帧或校验代码。"""
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return struct.pack('<H', crc)


class Peer:
    """只服务临时回环端口或 PTY，不访问任何真实 Modbus 设备。"""

    def __init__(self, rtu=False, mismatch=False, exception=False):
        self.rtu = rtu
        self.mismatch = mismatch
        self.exception = exception
        self.requests = []
        self.coils = {}
        self.registers = {}
        self.errors = []
        self.stop = threading.Event()
        self.responded = threading.Event()
        self.connection = None
        if rtu:
            self.master, self.slave = pty.openpty()
            self.args = ['rtu', '--device', os.ttyname(self.slave)]
        else:
            self.listener = socket.socket()
            self.listener.bind(('127.0.0.1', 0))
            self.listener.listen(1)
            self.listener.settimeout(0.1)
            self.args = ['tcp', '--ip', '127.0.0.1', '--port',
                         str(self.listener.getsockname()[1])]

    def __enter__(self):
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(2)
        if self.rtu:
            os.close(self.master)
            os.close(self.slave)
        else:
            if self.connection is not None:
                self.connection.close()
            self.listener.close()
        if self.thread.is_alive():
            raise AssertionError('临时从站未能退出')
        if self.errors:
            raise AssertionError(self.errors)

    def read_exact(self, count):
        data = b''
        fd = self.master if self.rtu else self.connection.fileno()
        while len(data) < count and not self.stop.is_set():
            if not select.select([fd], [], [], 0.1)[0]:
                continue
            chunk = os.read(fd, count - len(data))
            if not chunk:
                return None
            data += chunk
        return data if len(data) == count else None

    def response(self, unit, pdu):
        function, address, count = struct.unpack('>BHH', pdu[:5])
        self.requests.append((unit, function, address, count))
        if self.exception:
            return bytes([function | 0x80, 2])
        if function in (1, 2):
            values = [self.coils.get(address + i, (address + i) % 2)
                      if function == 1 else (address + i + 1) % 2
                      for i in range(count)]
            data = bytearray((count + 7) // 8)
            for i, value in enumerate(values):
                data[i // 8] |= value << (i % 8)
            return bytes([function, len(data)]) + data
        if function in (3, 4):
            values = [self.registers.get(address + i, 1000 + address + i)
                      if function == 3 else 2000 + address + i
                      for i in range(count)]
            return bytes([function, count * 2]) + struct.pack('>' + 'H' * count, *values)
        if function == 15:
            assert pdu[5] == (count + 7) // 8
            if not self.mismatch:
                for i in range(count):
                    self.coils[address + i] = (pdu[6 + i // 8] >> (i % 8)) & 1
        elif function == 16:
            assert pdu[5] == count * 2
            values = struct.unpack('>' + 'H' * count, pdu[6:])
            if not self.mismatch:
                self.registers.update((address + i, v) for i, v in enumerate(values))
        else:
            raise AssertionError(f'未预期的功能码 {function}')
        return pdu[:5]

    def serve(self):
        try:
            if not self.rtu:
                while not self.stop.is_set():
                    try:
                        self.connection, _ = self.listener.accept()
                        break
                    except socket.timeout:
                        continue
                if self.connection is None:
                    return
            while not self.stop.is_set():
                if self.rtu:
                    prefix = self.read_exact(2)
                    if prefix is None:
                        return
                    unit, function = prefix
                    body = self.read_exact(5 if function in (15, 16) else 4)
                    if body is None:
                        return
                    if function in (15, 16):
                        tail = self.read_exact(body[4] + 2)
                    else:
                        tail = self.read_exact(2)
                    if tail is None:
                        return
                    request = prefix + body + tail
                    assert request[-2:] == crc16(request[:-2])
                    pdu = request[1:-2]
                else:
                    header = self.read_exact(7)
                    if header is None:
                        return
                    tid, protocol, length, unit = struct.unpack('>HHHB', header)
                    assert protocol == 0 and 2 <= length <= 254
                    pdu = self.read_exact(length - 1)
                    if pdu is None:
                        return
                reply = self.response(unit, pdu)
                if self.rtu:
                    frame = bytes([unit]) + reply
                    frame += crc16(frame)
                    send = lambda data: os.write(self.master, data)
                else:
                    frame = struct.pack('>HHHB', tid, 0, len(reply) + 1, unit) + reply
                    send = self.connection.sendall
                # 分片返回，验证 CLI 显示的是整帧回调，而非 recv 的零散片段。
                send(frame[:3])
                time.sleep(0.002)
                send(frame[3:])
                self.responded.set()
        except Exception as exc:
            self.errors.append(repr(exc))


class CliTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux ELF 运行目录测试')
    def test_relocated_binary_uses_adjacent_lib(self):
        """从不同工作目录直接执行搬移后的 ELF，必须加载其自身 lib 内的库。"""
        program = Path(PROGRAM)
        if program.name == 'run-frame-callback-test.sh':
            binary = program.parent / 'bin' / 'frame-callback-test'
        else:
            candidate = program.parent / '.libs' / program.name
            binary = candidate if candidate.is_file() else program
        libraries = binary.parent / 'lib'
        if not libraries.is_dir():
            libraries = binary.parents[2] / 'src' / '.libs'
        with tempfile.TemporaryDirectory(prefix='modbus runtime ') as temporary:
            root = Path(temporary)
            application = root / 'moved application'
            application.mkdir()
            (application / 'lib').mkdir()
            executable = application / 'frame-callback-test'
            shutil.copy2(binary, executable)
            # 保留 SONAME 符号链接关系，部署目录不依赖 install/usr/lib 或原构建目录。
            for library in libraries.glob('libmodbus.so*'):
                target = application / 'lib' / library.name
                if library.is_symlink():
                    target.symlink_to(os.readlink(library))
                else:
                    shutil.copy2(library, target)
            environment = os.environ.copy()
            for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_TRACE_LOADED_OBJECTS'):
                environment.pop(name, None)
            trace = subprocess.run([str(executable), '--help'], cwd=root,
                                   env=dict(environment, LD_TRACE_LOADED_OBJECTS='1'),
                                   capture_output=True, text=True, timeout=5)
            self.assertEqual(trace.returncode, 0, trace.stdout + trace.stderr)
            self.assertIn(str(application / 'lib' / 'libmodbus.so.5'), trace.stdout)
            # 再执行一次真实操作，确保加载的库提供本工具需要的回调 API。
            with Peer() as peer:
                result = subprocess.run([str(executable), *peer.args, '--cycles', '1'],
                                        cwd=root, env=environment, capture_output=True,
                                        text=True, timeout=8)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(peer.requests, [(1, 3, 0, 1)])
                self.assertEqual(result.stdout.count('[TX]'), 1)
                self.assertEqual(result.stdout.count('[RX]'), 1)

    def test_invalid_arguments(self):
        cases = [[], ['bad-mode'], ['rtu'], ['tcp', '--port', '0'],
                 ['tcp', '--port', '65536'], ['tcp', '--ip', 'not-an-ip'],
                 ['tcp', '--slave', '0'], ['tcp', '--cycles', '-1'],
                 ['tcp', '--read-holding', '65535:2'],
                 ['tcp', '--read-holding', '0:0'], ['tcp', '--read-holding', '0:126'],
                 ['tcp', '--write-registers', '0:65536'],
                 ['tcp', '--write-registers', '0:1,'],
                 ['tcp', '--write-coils', '0:2'], ['tcp', '--unknown', '1'],
                 ['tcp', '--device', '/dev/unused'],
                 ['rtu', '--device', '/dev/unused', '--baud', 'abc'],
                 ['rtu', '--device', '/dev/unused', '--data-bits', '9'],
                 ['rtu', '--device', '/dev/unused', '--stop-bits', '0'],
                 ['rtu', '--device', '/dev/unused', '--parity', 'X']]
        for args in cases:
            with self.subTest(args=args):
                result = run_client(*args)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_tcp_all_point_types_and_readback(self):
        with Peer() as peer:
            result = run_client(*peer.args, '--slave', '7', '--cycles', '2',
                                '--interval', '0', '--read-coils', '0:2',
                                '--read-discrete', '10:2', '--read-holding', '20:2',
                                '--read-input', '30:2', '--write-coils', '40:1,0,1',
                                '--write-registers', '0x32:100,65535')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            expected = [(7, 1, 0, 2), (7, 2, 10, 2), (7, 3, 20, 2), (7, 4, 30, 2),
                        (7, 15, 40, 3), (7, 1, 40, 3), (7, 16, 50, 2), (7, 3, 50, 2)]
            self.assertEqual(peer.requests, expected * 2)
            self.assertEqual(peer.registers, {50: 100, 51: 65535})
            self.assertEqual(peer.coils, {40: 1, 41: 0, 42: 1})
            self.assertEqual(result.stdout.count('[TX]'), 16)
            self.assertEqual(result.stdout.count('[RX]'), 16)
            self.assertIn('1020', result.stdout)
            self.assertIn('2030', result.stdout)
            self.assertIn('错误=0', result.stdout)

    def test_frame_output_disabled_and_default_read(self):
        with Peer() as peer:
            result = run_client(*peer.args, '--cycles', '1', '--frame-output', '0')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(peer.requests, [(1, 3, 0, 1)])
            self.assertNotIn('[TX]', result.stdout)
            self.assertNotIn('[RX]', result.stdout)
            self.assertIn('1000', result.stdout)

    def test_rtu_serial_and_station(self):
        with Peer(rtu=True) as peer:
            result = run_client(*peer.args, '--slave', '9', '--cycles', '1',
                                '--baud', '9600', '--data-bits', '8', '--stop-bits', '1',
                                '--parity', 'n', '--read-holding', '2:2',
                                '--write-registers', '10:123,456')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(peer.requests, [(9, 3, 2, 2), (9, 16, 10, 2), (9, 3, 10, 2)])
            self.assertEqual(result.stdout.count('[TX]'), 3)
            self.assertEqual(result.stdout.count('[RX]'), 3)
            self.assertEqual(peer.registers, {10: 123, 11: 456})

    def test_readback_mismatch_is_failure(self):
        with Peer(mismatch=True) as peer:
            result = run_client(*peer.args, '--cycles', '1', '--write-registers', '10:123')
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn('不一致', result.stderr)

    def test_protocol_exception_is_failure(self):
        with Peer(exception=True) as peer:
            result = run_client(*peer.args, '--cycles', '1')
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn('错误', result.stderr)
            self.assertEqual(result.stdout.count('[RX]'), 1)

    def test_signal_interrupts_poll_delay(self):
        with Peer() as peer:
            process = subprocess.Popen([PROGRAM, *peer.args, '--interval', '60000'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True)
            try:
                self.assertTrue(peer.responded.wait(4), '未收到第一轮请求')
                process.send_signal(signal.SIGINT)
                output, error = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0, output + error)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()


if __name__ == '__main__':
    # 先验证入口，旧自测程序会忽略 --help；避免后续测试等待不存在的连接。
    help_result = run_client('--help')
    assert help_result.returncode == 0
    for expected in ('tcp', 'rtu', '192.168.9.60', '502', '9600', '--read-input',
                     '--write-registers', '--frame-output'):
        assert expected in help_result.stdout, f'命令行帮助缺少 {expected}'
    unittest.main(verbosity=2)
