"""
ESP32 mDNS discovery and configuration push.

Discovers the ESP32 BirdNet device on the local network via mDNS,
determines the local IP to advertise, and pushes the UDP target
configuration to the ESP32's HTTP config API.
"""

import logging
import os
import socket
import threading
import time

import httpx
from zeroconf import Zeroconf, ServiceBrowser

logger = logging.getLogger("birdnet-listener")

# mDNS service type advertised by the ESP32
MDNS_SERVICE_TYPE = "_birdnet._udp.local."
MDNS_HOSTNAME = "esp32-birdnet"


def discover_esp32(timeout: float = 10.0) -> str | None:
    """
    Use mDNS to find the ESP32 BirdNet device on the local network.
    Returns the IP address string or None if not found.
    """
    found = threading.Event()
    result: list[str] = []

    class Listener:
        def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            info = zc.get_service_info(type_, name)
            if info and info.addresses:
                ip = socket.inet_ntoa(info.addresses[0])
                logger.info(f"[mDNS] Found ESP32 BirdNet at {ip} (service: {name})")
                result.append(ip)
                found.set()

        def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            pass

        def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            pass

    zc = Zeroconf()
    listener = Listener()

    logger.info(f"[mDNS] Searching for {MDNS_SERVICE_TYPE} ...")
    browser = ServiceBrowser(zc, MDNS_SERVICE_TYPE, listener)

    found.wait(timeout=timeout)
    zc.close()

    if result:
        return result[0]

    # Fallback: try resolving the hostname directly
    logger.info(f"[mDNS] Service not found, trying {MDNS_HOSTNAME}.local hostname resolution...")
    try:
        ip = socket.gethostbyname(f"{MDNS_HOSTNAME}.local")
        logger.info(f"[mDNS] Resolved {MDNS_HOSTNAME}.local -> {ip}")
        return ip
    except socket.gaierror:
        logger.warning("[mDNS] Could not resolve ESP32 hostname")
        return None


def get_local_ip() -> str:
    """
    Get the IP address to advertise to the ESP32.

    Priority:
      1. BIRDNET_HOST_IP env var (explicit host IP, useful in Docker bridge mode)
      2. BIRDNET_LISTENER_IP env var (legacy, same purpose)
      3. Auto-detect via UDP socket trick

    In Docker bridge networking, auto-detect returns the container IP (172.x.x.x)
    which is unreachable from the ESP32. Use BIRDNET_HOST_IP to pass the real host IP.
    """
    host_ip = os.environ.get("BIRDNET_HOST_IP")
    if host_ip:
        logger.info(f"[IP] Using BIRDNET_HOST_IP={host_ip}")
        return host_ip

    listener_ip = os.environ.get("BIRDNET_LISTENER_IP")
    if listener_ip:
        logger.info(f"[IP] Using BIRDNET_LISTENER_IP={listener_ip}")
        return listener_ip

    # Auto-detect
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        if ip.startswith("172.") or ip.startswith("10."):
            logger.warning(
                f"[IP] Auto-detected IP is {ip} — this looks like a container/internal IP. "
                f"Set BIRDNET_HOST_IP to the host's real LAN IP if running in Docker bridge mode."
            )
        return ip
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def push_config_to_esp32(esp32_ip: str, listener_ip: str, udp_port: int, timeout: float = 15.0) -> bool:
    """
    POST to the ESP32's /config endpoint to set udp_host to our IP.
    Returns True on success, False on failure.
    """
    url = f"http://{esp32_ip}/config"
    data = {
        "udp_host": listener_ip,
        "udp_port": str(udp_port),
    }

    logger.info(f"[Config] Pushing config to ESP32 at {url}: udp_host={listener_ip}, udp_port={udp_port}")

    try:
        resp = httpx.post(url, data=data, timeout=timeout)
        if resp.status_code == 200:
            logger.info(f"[Config] ESP32 config updated successfully: {resp.json()}")
            return True
        else:
            logger.error(f"[Config] ESP32 returned status {resp.status_code}: {resp.text}")
            return False
    except httpx.RequestError as e:
        logger.error(f"[Config] Failed to reach ESP32: {e}")
        return False


def run_discovery_in_background(
    esp32_ip: str | None,
    listener_ip: str,
    udp_port: int,
    stop_event: threading.Event,
) -> None:
    """
    Run mDNS discovery + config push in a background thread.

    Loops until either the config is pushed successfully or stop_event is set
    (which happens when UDP packets start arriving, meaning discovery is unnecessary).
    """
    while not stop_event.is_set():
        if not esp32_ip:
            logger.info("No ESP32 IP provided, attempting mDNS discovery...")
            esp32_ip = discover_esp32(timeout=10.0)

        if stop_event.is_set():
            break

        if not esp32_ip:
            logger.warning("[Discovery] ESP32 not found, retrying in 5s...")
            if stop_event.wait(timeout=5):
                break
            continue

        if push_config_to_esp32(esp32_ip, listener_ip, udp_port):
            logger.info("[Discovery] Config push successful, discovery thread done.")
            break

        if stop_event.is_set():
            break

        logger.warning("[Discovery] Config push failed, retrying in 5s...")
        if stop_event.wait(timeout=5):
            break

    if stop_event.is_set():
        logger.info("[Discovery] Stopped — UDP packets already arriving, discovery not needed.")
