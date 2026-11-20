"""Local development stack: session and room recovery across process/routing loss."""
import argparse
import hashlib
import subprocess
import time
import urllib.request
from session_continuity_common import ChatClient, MSG_CHAT_SEND, lp_utf8
from verify_whisper_cross_instance import resolve_backend_with_retry


def command(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError('recovery command failed: ' + args[0])
    return result.stdout.strip()


def wait_ready(port):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/readyz', timeout=2) as response:
                if response.status == 200:
                    return
        except OSError:
            pass
        time.sleep(.3)
    raise AssertionError('service readiness timeout')


def run(scenario):
    suffix = str(time.time_ns())
    user, room = 'recover-' + suffix, 'room-' + suffix
    first, second = ChatClient(port=36100), ChatClient(port=36101)
    stopped = None
    try:
        first.connect()
        login = first.login(user, '')
        assert login['logical_session_id'] and login['resume_token']
        first.join_room(room, user)
        backend = resolve_backend_with_retry(user)
        assert backend in {'server-1', 'server-2'}
        first.close()
        time.sleep(.3)
        routing = 'resume-hash:' + hashlib.sha256(login['resume_token'].encode()).hexdigest()
        if scenario == 'gateway-restart':
            command('docker', 'restart', '-t', '20', 'dynaxis-stack-gateway-1-1')
            wait_ready(36001)
        elif scenario == 'server-restart':
            command('docker', 'restart', '-t', '20', f'dynaxis-stack-{backend}-1')
            wait_ready(39091 if backend == 'server-1' else 39092)
        elif scenario == 'server-failover':
            stopped = f'dynaxis-stack-{backend}-1'
            command('docker', 'stop', '-t', '20', stopped)
            time.sleep(2)
        elif scenario == 'room-state-missing':
            removed = command('docker', 'exec', 'dynaxis-stack-redis-1', 'redis-cli', 'DEL', 'dynaxis:continuity:room:' + login['logical_session_id'])
            assert removed == '1', 'room state must exist before testing lobby recovery'
            room = 'lobby'
        elif scenario == 'locator-fallback':
            removed = command('docker', 'exec', 'dynaxis-stack-redis-1', 'redis-cli', 'DEL', 'gateway/session/' + routing)
            assert removed == '1', 'resume alias must exist before testing locator fallback'
        second.connect()
        resumed = second.login('ignored', 'resume:' + login['resume_token'])
        assert resumed['resumed'] and resumed['effective_user'] == user
        assert resumed['logical_session_id'] == login['logical_session_id']
        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8('after-recovery'))
        second.wait_for_self_chat(room, 'after-recovery', 5)
        if scenario == 'server-failover':
            recovered_backend = resolve_backend_with_retry(routing)
            assert recovered_backend in {'server-1', 'server-2'} and recovered_backend != backend
        print('PASS:', scenario, 'preserves identity and logical session; expected room accepts chat')
    finally:
        first.close()
        second.close()
        if stopped:
            command('docker', 'start', stopped)
            wait_ready(39091 if 'server-1-' in stopped else 39092)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('gateway-restart', 'server-restart', 'server-failover', 'locator-fallback', 'room-state-missing'), required=True)
    run(parser.parse_args().scenario)
