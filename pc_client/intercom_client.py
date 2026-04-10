#!/usr/bin/env python3
"""
ESP32 Intercom Client for Windows/Linux/macOS
=============================================

PC client for ESP32 Walkie-Talkie intercoms.
Each intercom is bound to a specific key on keyboard.

Usage:
    # Single intercom (quick start):
    python intercom_client.py --ip 192.168.1.100

    # Multiple intercoms via config:
    python intercom_client.py --config intercoms.json

    # Custom key binding:
    python intercom_client.py --ip 192.168.1.100 --key f1

Install dependencies:
    pip install -r requirements.txt

Protocol (same as ESP32 firmware):
    Control: JSON over UDP port 8080
        {"t":"tx_on","f":"PC"}
        {"t":"tx_off","f":"PC"}
        {"t":"ping","f":"PC"}
        {"t":"pong","f":"PC"}

    Audio: raw PCM over UDP port 8081
        [seq: uint16 LE][timestamp: uint32 LE][pcm_data: N bytes]
        PCM: 16-bit signed, mono, 16000 Hz

Author: ShtirlizVD
License: MIT
"""

import argparse
import json
import socket
import struct
import threading
import time
import sys
import os

# --- Check dependencies ---
try:
    import numpy as np
except ImportError:
    print("[ERROR] numpy not installed. Run: pip install numpy")
    sys.exit(1)

try:
    import sounddevice as sd
except ImportError:
    print("[ERROR] sounddevice not installed. Run: pip install sounddevice")
    sys.exit(1)

try:
    import keyboard
except ImportError:
    print("[ERROR] keyboard not installed. Run: pip install keyboard")
    sys.exit(1)

# --- Constants ---
SAMPLE_RATE = 16000
CHANNELS = 1
DTYPE = np.int16
FRAME_SAMPLES = 320   # 20 ms at 16 kHz
FRAME_BYTES = FRAME_SAMPLES * 2  # 640 bytes
CTRL_TIMEOUT = 0.5     # seconds
AUDIO_TIMEOUT = 0.1    # seconds
PING_INTERVAL = 5      # seconds
PEER_ONLINE_TIMEOUT = 15  # seconds
RECV_TIMEOUT = 0.5     # seconds - no audio = peer stopped


class Intercom:
    """Represents one ESP32 intercom device."""

    def __init__(self, name, ip, ctrl_port=8080, audio_port=8081, key='space'):
        self.name = name
        self.ip = ip
        self.ctrl_port = ctrl_port
        self.audio_port = audio_port
        self.key = key.lower()

        # State
        self.online = False
        self.last_pong = 0
        self.transmitting = False  # We are sending audio to this intercom
        self.receiving = False     # This intercom is sending audio to us
        self.last_audio_seen = 0
        self.send_seq = 0
        self.tx_start_time = 0


class IntercomClient:
    """Main client: manages multiple intercoms, audio I/O, keyboard."""

    def __init__(self, config):
        self.intercoms = []
        self.ctrl_sock = None
        self.audio_sock = None
        self.running = False
        self.mic_stream = None
        self.speaker_stream = None
        self.mic_lock = threading.Lock()  # protects audio sends
        self._status_dirty = True

        # Parse config
        self.sample_rate = config.get('sample_rate', SAMPLE_RATE)
        self.local_ctrl_port = config.get('local_ctrl_port', 8080)
        self.local_audio_port = config.get('local_audio_port', 8081)
        self.pc_name = config.get('pc_name', 'PC')

        for ic_cfg in config.get('intercoms', []):
            self.intercoms.append(Intercom(
                name=ic_cfg['name'],
                ip=ic_cfg['ip'],
                ctrl_port=ic_cfg.get('ctrl_port', 8080),
                audio_port=ic_cfg.get('audio_port', 8081),
                key=ic_cfg.get('key', 'space'),
            ))

        if not self.intercoms:
            print("[ERROR] No intercoms configured!")
            sys.exit(1)

    # ================================================================
    #  Start / Stop
    # ================================================================

    def start(self):
        """Initialize sockets, audio, threads and enter main loop."""
        # --- Sockets ---
        self.ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ctrl_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.ctrl_sock.bind(('0.0.0.0', self.local_ctrl_port))
        except OSError as e:
            print(f"[WARN] Cannot bind ctrl port {self.local_ctrl_port}: {e}")
            print(f"[WARN] Another instance running? Trying port {self.local_ctrl_port + 1}")
            self.local_ctrl_port += 1
            self.ctrl_sock.bind(('0.0.0.0', self.local_ctrl_port))

        self.audio_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.audio_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.audio_sock.bind(('0.0.0.0', self.local_audio_port))
        except OSError as e:
            print(f"[WARN] Cannot bind audio port {self.local_audio_port}: {e}")
            self.local_audio_port += 1
            self.audio_sock.bind(('0.0.0.0', self.local_audio_port))

        self.running = True

        # --- Threads ---
        threading.Thread(target=self._ctrl_receiver, daemon=True, name="CtrlRx").start()
        threading.Thread(target=self._audio_receiver, daemon=True, name="AudioRx").start()
        threading.Thread(target=self._ping_thread, daemon=True, name="Ping").start()
        threading.Thread(target=self._status_printer, daemon=True, name="Status").start()

        # --- Audio streams ---
        self.mic_stream = sd.InputStream(
            samplerate=self.sample_rate,
            channels=CHANNELS,
            dtype='int16',
            blocksize=FRAME_SAMPLES,
            latency='low',
            callback=self._mic_callback,
        )
        self.mic_stream.start()

        self.speaker_stream = sd.OutputStream(
            samplerate=self.sample_rate,
            channels=CHANNELS,
            dtype='int16',
            blocksize=FRAME_SAMPLES * 8,
            latency='low',
        )
        self.speaker_stream.start()

        # Print banner
        print()
        print("=" * 56)
        print("  ESP32 Intercom Client")
        print("=" * 56)
        print(f"  PC name: {self.pc_name}")
        print(f"  Ctrl port: {self.local_ctrl_port} | Audio port: {self.local_audio_port}")
        print(f"  Sample rate: {self.sample_rate} Hz")
        print("-" * 56)
        for ic in self.intercoms:
            print(f"  [{ic.key.upper():>8s}]  ->  {ic.name:<12s}  {ic.ip}")
        print("-" * 56)
        print("  [Q / Esc]  Quit")
        print("=" * 56)
        print()

        # Initial pings
        for ic in self.intercoms:
            self._send_ctrl(ic.ip, 'ping')

        # --- Main loop (keyboard) ---
        self._main_loop()

    def stop(self):
        """Graceful shutdown."""
        self.running = False
        print("\n[INFO] Shutting down...")

        # Stop all transmissions
        for ic in self.intercoms:
            if ic.transmitting:
                self._send_ctrl(ic.ip, 'tx_off')

        # Close audio
        if self.mic_stream:
            try:
                self.mic_stream.stop()
                self.mic_stream.close()
            except Exception:
                pass
        if self.speaker_stream:
            try:
                self.speaker_stream.stop()
                self.speaker_stream.close()
            except Exception:
                pass

        # Close sockets
        for sock in (self.ctrl_sock, self.audio_sock):
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass

        print("[INFO] Stopped.")

    # ================================================================
    #  Microphone callback (called from audio thread)
    # ================================================================

    def _mic_callback(self, indata, frames, time_info, status):
        """sounddevice callback: mic data ready -> send to active intercom."""
        if not self.running or status:
            return

        # indata shape: (frames, 1), dtype int16
        pcm_bytes = indata.tobytes()

        with self.mic_lock:
            for ic in self.intercoms:
                if ic.transmitting:
                    try:
                        # Header: [seq:u16_LE][timestamp:u32_LE]
                        timestamp_ms = int((time.time() - ic.tx_start_time) * 1000)
                        header = struct.pack('<HI', ic.send_seq, timestamp_ms)
                        ic.send_seq = (ic.send_seq + 1) & 0xFFFF
                        self.audio_sock.sendto(
                            header + pcm_bytes,
                            (ic.ip, ic.audio_port)
                        )
                    except Exception:
                        pass

    # ================================================================
    #  Control receiver thread
    # ================================================================

    def _ctrl_receiver(self):
        """Receive JSON control messages from intercoms."""
        self.ctrl_sock.settimeout(CTRL_TIMEOUT)
        buf = bytearray(512)

        while self.running:
            try:
                n, addr = self.ctrl_sock.recvfrom_into(buf)
                if n == 0:
                    continue

                try:
                    msg = json.loads(buf[:n].decode('utf-8', errors='replace'))
                except json.JSONDecodeError:
                    continue

                msg_type = msg.get('t', '')
                from_name = msg.get('f', '?')
                sender_ip = addr[0]
                ic = self._find_by_ip(sender_ip)

                if msg_type == 'tx_on':
                    if ic:
                        ic.receiving = True
                        ic.online = True
                        ic.last_pong = time.time()
                    self._set_status_dirty()
                    print(f"\n  >> [{from_name or sender_ip}] transmitting...")

                elif msg_type == 'tx_off':
                    if ic:
                        ic.receiving = False
                    self._set_status_dirty()
                    print(f"\n  << [{from_name or sender_ip}] stopped.")

                elif msg_type == 'ping':
                    if ic:
                        ic.online = True
                        ic.last_pong = time.time()
                    # Reply pong
                    self._send_ctrl(sender_ip, 'pong')

                elif msg_type == 'pong':
                    if ic:
                        if not ic.online:
                            print(f"\n  ** [{ic.name}] ONLINE")
                            self._set_status_dirty()
                        ic.online = True
                        ic.last_pong = time.time()

            except socket.timeout:
                continue
            except OSError:
                if self.running:
                    continue
                break

    # ================================================================
    #  Audio receiver thread
    # ================================================================

    def _audio_receiver(self):
        """Receive PCM audio from intercoms and play through speakers."""
        self.audio_sock.settimeout(AUDIO_TIMEOUT)

        while self.running:
            try:
                data, addr = self.audio_sock.recvfrom(4096)
                if len(data) <= 6:
                    continue

                pcm_data = data[6:]
                if len(pcm_data) < 2:
                    continue

                ic = self._find_by_ip(addr[0])
                if ic:
                    ic.last_audio_seen = time.time()
                    if not ic.receiving:
                        ic.receiving = True
                        self._set_status_dirty()

                # Convert to numpy array and play
                samples = np.frombuffer(pcm_data, dtype=np.int16).copy()
                try:
                    self.speaker_stream.write(samples.reshape(-1, 1))
                except Exception:
                    pass  # Buffer overflow - drop frame

            except socket.timeout:
                # Check receive timeouts
                now = time.time()
                for ic in self.intercoms:
                    if ic.receiving and (now - ic.last_audio_seen > RECV_TIMEOUT):
                        ic.receiving = False
                        self._set_status_dirty()
                continue
            except OSError:
                if self.running:
                    continue
                break

    # ================================================================
    #  Ping thread
    # ================================================================

    def _ping_thread(self):
        """Periodically ping intercoms to check online status."""
        # First ping after 1 second
        time.sleep(1)

        while self.running:
            for ic in self.intercoms:
                self._send_ctrl(ic.ip, 'ping')

            # Check online timeout
            now = time.time()
            for ic in self.intercoms:
                if ic.last_pong > 0 and ic.online and (now - ic.last_pong > PEER_ONLINE_TIMEOUT):
                    ic.online = False
                    print(f"\n  !! [{ic.name}] OFFLINE")
                    self._set_status_dirty()

            time.sleep(PING_INTERVAL)

    # ================================================================
    #  Status printer thread
    # ================================================================

    def _status_printer(self):
        """Periodically reprint status line."""
        while self.running:
            if self._status_dirty:
                self._print_status_line()
                self._status_dirty = False
            time.sleep(0.5)

    def _set_status_dirty(self):
        self._status_dirty = True

    def _print_status_line(self):
        """Print one-line status of all intercoms."""
        parts = []
        for ic in self.intercoms:
            if ic.transmitting:
                tag = f"TX->{ic.name}"
            elif ic.receiving:
                tag = f"<-{ic.name}-RX"
            elif ic.online:
                tag = f"{ic.name}:ok"
            else:
                tag = f"{ic.name}:--"
            parts.append(f"[{ic.key.upper()}]{tag}")
        line = "  " + " | ".join(parts)
        # Print with carriage return to overwrite
        sys.stdout.write(f"\r{line:<60}\n")
        sys.stdout.flush()

    # ================================================================
    #  Main keyboard loop
    # ================================================================

    def _main_loop(self):
        """Poll keyboard and manage PTT state."""
        try:
            while self.running:
                # Check each intercom's key
                for ic in self.intercoms:
                    is_pressed = keyboard.is_pressed(ic.key)

                    if is_pressed and not ic.transmitting:
                        # Key just pressed -> start transmitting
                        if not ic.online:
                            print(f"\n  !! [{ic.name}] is OFFLINE - cannot transmit")
                            continue
                        ic.transmitting = True
                        ic.tx_start_time = time.time()
                        ic.send_seq = 0
                        self._send_ctrl(ic.ip, 'tx_on')
                        self._set_status_dirty()

                    elif not is_pressed and ic.transmitting:
                        # Key released -> stop transmitting
                        ic.transmitting = False
                        self._send_ctrl(ic.ip, 'tx_off')
                        self._set_status_dirty()

                # Quit key
                if keyboard.is_pressed('q') or keyboard.is_pressed('esc'):
                    print("\n")
                    break

                time.sleep(0.02)  # 50 Hz poll rate

        except KeyboardInterrupt:
            print("\n")
        finally:
            self.stop()

    # ================================================================
    #  Helpers
    # ================================================================

    def _find_by_ip(self, ip):
        for ic in self.intercoms:
            if ic.ip == ip:
                return ic
        return None

    def _send_ctrl(self, ip, msg_type):
        """Send a JSON control message to an intercom."""
        msg = json.dumps({"t": msg_type, "f": self.pc_name})
        try:
            ic = self._find_by_ip(ip)
            port = ic.ctrl_port if ic else 8080
            self.ctrl_sock.sendto(msg.encode('utf-8'), (ip, port))
        except Exception:
            pass


# ================================================================
#  Config helpers
# ================================================================

DEFAULT_CONFIG = {
    "pc_name": "PC",
    "sample_rate": 16000,
    "local_ctrl_port": 8080,
    "local_audio_port": 8081,
    "intercoms": [
        {
            "name": "Garage",
            "ip": "192.168.1.100",
            "ctrl_port": 8080,
            "audio_port": 8081,
            "key": "space"
        },
        {
            "name": "Home",
            "ip": "192.168.1.101",
            "ctrl_port": 8080,
            "audio_port": 8081,
            "key": "enter"
        }
    ]
}


def create_default_config(path):
    """Write example config file."""
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(DEFAULT_CONFIG, f, indent=4, ensure_ascii=False)
    print(f"[INFO] Created default config: {path}")
    print("[INFO] Edit it with your intercom IPs and run again.")


def load_config(path):
    """Load and validate config file."""
    with open(path, 'r', encoding='utf-8') as f:
        config = json.load(f)

    # Validate
    for i, ic in enumerate(config.get('intercoms', [])):
        if 'ip' not in ic:
            print(f"[ERROR] Intercom #{i} missing 'ip' field")
            sys.exit(1)
        if 'name' not in ic:
            ic['name'] = ic['ip']
        if 'key' not in ic:
            ic['key'] = 'space'

    # Check for duplicate keys
    keys = [ic['key'].lower() for ic in config['intercoms']]
    if len(keys) != len(set(keys)):
        print("[ERROR] Duplicate key bindings! Each intercom must have a unique key.")
        sys.exit(1)

    return config


# ================================================================
#  Entry point
# ================================================================

def main():
    parser = argparse.ArgumentParser(
        description='ESP32 Intercom Client - Talk to your intercoms from PC',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --ip 192.168.1.100                      Talk to one intercom (Space=PTT)
  %(prog)s --ip 192.168.1.100 --key f1             Use F1 as PTT key
  %(prog)s --config intercoms.json                 Multiple intercoms with config
  %(prog)s --config intercoms.json --create         Create default config and exit

Supported keys: space, enter, tab, f1-f12, a-z, 0-9, and more.
See keyboard module docs for full list.
"""
    )
    parser.add_argument('--config', '-c', type=str,
                        help='Path to JSON config file')
    parser.add_argument('--ip', type=str,
                        help='Intercom IP address (single intercom mode)')
    parser.add_argument('--key', type=str, default='space',
                        help='PTT key binding (default: space)')
    parser.add_argument('--name', type=str, default='Intercom',
                        help='Intercom display name (single mode)')
    parser.add_argument('--ctrl-port', type=int, default=8080,
                        help='Intercom control UDP port (default: 8080)')
    parser.add_argument('--audio-port', type=int, default=8081,
                        help='Intercom audio UDP port (default: 8081)')
    parser.add_argument('--create', action='store_true',
                        help='Create default config file and exit')
    parser.add_argument('--pc-name', type=str, default='PC',
                        help='Your PC name shown to intercoms')

    args = parser.parse_args()

    # --- Create default config ---
    if args.create:
        config_path = args.config or 'intercoms.json'
        DEFAULT_CONFIG['pc_name'] = args.pc_name
        create_default_config(config_path)
        return

    # --- Load config ---
    if args.config:
        if not os.path.exists(args.config):
            print(f"[ERROR] Config file not found: {args.config}")
            print(f"[HINT] Run with --create to generate one:")
            print(f"  python {sys.argv[0]} --config {args.config} --create")
            sys.exit(1)
        config = load_config(args.config)
    elif args.ip:
        config = {
            'pc_name': args.pc_name,
            'sample_rate': SAMPLE_RATE,
            'intercoms': [{
                'name': args.name,
                'ip': args.ip,
                'ctrl_port': args.ctrl_port,
                'audio_port': args.audio_port,
                'key': args.key,
            }]
        }
    else:
        parser.print_help()
        print()
        print("[HINT] Quick start:")
        print(f"  python {sys.argv[0]} --ip 192.168.1.100")
        print()
        print("[HINT] Or create a config file:")
        print(f"  python {sys.argv[0]} --config intercoms.json --create")
        sys.exit(0)

    # Override PC name if specified
    if args.pc_name != 'PC':
        config['pc_name'] = args.pc_name

    # --- Start client ---
    client = IntercomClient(config)
    try:
        client.start()
    except Exception as e:
        print(f"\n[ERROR] {e}")
        client.stop()
        sys.exit(1)


if __name__ == '__main__':
    main()
