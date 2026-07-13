"""
Home Assistant MQTT integration using kellerza/mqtt_entity.

Publishes ESP32 BirdNet telemetry data as Home Assistant entities via MQTT
discovery. Creates a device with sensors for battery, solar, WiFi, and
stream diagnostics.
"""

import asyncio
import logging
from dataclasses import dataclass

from mqtt_entity import MQTTClient, MQTTDevice, MQTTSensorEntity, MQTTBinarySensorEntity

logger = logging.getLogger("birdnet-listener")

TOPIC_PREFIX = "birdnet"


@dataclass
class MQTTConfig:
    """MQTT configuration."""

    host: str = ""
    port: int = 1883
    username: str = ""
    password: str = ""
    entity_name: str = "BirdNet Streamer"
    update_interval: float = 30.0  # seconds between telemetry publishes

    @property
    def enabled(self) -> bool:
        """MQTT is only enabled if a server host is configured."""
        return bool(self.host)


def _build_device(entity_name: str) -> tuple[MQTTDevice, dict[str, MQTTSensorEntity | MQTTBinarySensorEntity]]:
    """Build the MQTTDevice with all sensor entities for the BirdNet streamer."""
    device_id = "birdnet_streamer"
    state_topic = f"{TOPIC_PREFIX}/{device_id}/state"
    attrs_topic = f"{TOPIC_PREFIX}/{device_id}/attributes"

    # Define all sensor entities
    sensors: dict[str, MQTTSensorEntity | MQTTBinarySensorEntity] = {}

    # Power sensors
    sensors["battery_voltage"] = MQTTSensorEntity(
        name="Battery Voltage",
        unique_id=f"{device_id}_battery_voltage",
        state_topic=f"{state_topic}/battery_voltage",
        device_class="voltage",
        unit_of_measurement="V",
        state_class="measurement",
        suggested_display_precision=2,
        icon="mdi:battery",
    )
    sensors["battery_percent"] = MQTTSensorEntity(
        name="Battery",
        unique_id=f"{device_id}_battery_percent",
        state_topic=f"{state_topic}/battery_percent",
        device_class="battery",
        unit_of_measurement="%",
        state_class="measurement",
    )
    sensors["solar_voltage"] = MQTTSensorEntity(
        name="Solar Voltage",
        unique_id=f"{device_id}_solar_voltage",
        state_topic=f"{state_topic}/solar_voltage",
        device_class="voltage",
        unit_of_measurement="V",
        state_class="measurement",
        suggested_display_precision=2,
        icon="mdi:solar-power",
    )
    sensors["is_charging"] = MQTTBinarySensorEntity(
        name="Charging",
        unique_id=f"{device_id}_is_charging",
        state_topic=f"{state_topic}/is_charging",
        device_class="battery_charging",
    )

    # WiFi / connectivity sensors
    sensors["esp_rssi"] = MQTTSensorEntity(
        name="WiFi Signal",
        unique_id=f"{device_id}_rssi",
        state_topic=f"{state_topic}/esp_rssi",
        device_class="signal_strength",
        unit_of_measurement="dBm",
        state_class="measurement",
        icon="mdi:wifi",
    )
    sensors["esp_wifi_connected"] = MQTTBinarySensorEntity(
        name="WiFi Connected",
        unique_id=f"{device_id}_wifi_connected",
        state_topic=f"{state_topic}/esp_wifi_connected",
        device_class="connectivity",
    )

    # Stream statistics
    sensors["packets_received"] = MQTTSensorEntity(
        name="Packets Received",
        unique_id=f"{device_id}_packets_received",
        state_topic=f"{state_topic}/packets_received",
        state_class="total_increasing",
        icon="mdi:package-down",
    )
    sensors["stream_uptime"] = MQTTSensorEntity(
        name="Stream Uptime",
        unique_id=f"{device_id}_stream_uptime",
        state_topic=f"{state_topic}/stream_uptime",
        device_class="duration",
        unit_of_measurement="s",
        state_class="measurement",
        icon="mdi:timer-outline",
    )

    # ESP32 UDP stats
    sensors["esp_udp_errors"] = MQTTSensorEntity(
        name="UDP Errors",
        unique_id=f"{device_id}_udp_errors",
        state_topic=f"{state_topic}/esp_udp_errors",
        state_class="total_increasing",
        icon="mdi:alert-circle-outline",
        entity_category="diagnostic",
    )
    sensors["esp_udp_dropped"] = MQTTSensorEntity(
        name="UDP Dropped",
        unique_id=f"{device_id}_udp_dropped",
        state_topic=f"{state_topic}/esp_udp_dropped",
        state_class="total_increasing",
        icon="mdi:package-variant-remove",
        entity_category="diagnostic",
    )

    device = MQTTDevice(
        identifiers=[device_id],
        components=sensors,
        name=entity_name,
        manufacturer="ESP32 BirdNet",
        model="BirdNet Streamer",
        sw_version="1.0",
    )

    return device, sensors


class MQTTHAIntegration:
    """Manages the MQTT connection and telemetry publishing to Home Assistant."""

    def __init__(self, config: MQTTConfig):
        self.config = config
        self._client: MQTTClient | None = None
        self._device: MQTTDevice | None = None
        self._sensors: dict[str, MQTTSensorEntity | MQTTBinarySensorEntity] = {}
        self._task: asyncio.Task | None = None
        self._udp_protocol = None

    async def start(self, udp_protocol_getter) -> None:
        """Start the MQTT client and begin publishing telemetry.

        Args:
            udp_protocol_getter: A callable that returns the UDPReceiverProtocol
                instance (may return None if not yet started).
        """
        if not self.config.enabled:
            return

        self._udp_protocol_getter = udp_protocol_getter
        self._device, self._sensors = _build_device(self.config.entity_name)

        device_id = "birdnet_streamer"
        availability_topic = f"{TOPIC_PREFIX}/{device_id}/availability"

        self._client = MQTTClient(
            availability_topic=availability_topic,
            devs=[self._device],
            origin_name="birdnet-listener",
            origin_url="https://github.com/your-repo/esp32-birdnet-streamer",
        )

        await self._client.connect(
            host=self.config.host,
            port=self.config.port,
            username=self.config.username,
            password=self.config.password,
            wait_connected=True,
        )

        self._client.monitor_homeassistant_status()
        await self._client.publish_discovery_info()

        logger.info(
            f"[MQTT] Connected to {self.config.host}:{self.config.port}, "
            f"publishing as '{self.config.entity_name}'"
        )

        # Start the periodic telemetry publishing task
        self._task = asyncio.create_task(self._publish_loop())

    async def stop(self) -> None:
        """Stop the MQTT client and publishing task."""
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass

        if self._client:
            await self._client.disconnect()
            logger.info("[MQTT] Disconnected")

    async def _publish_loop(self) -> None:
        """Periodically publish telemetry data to MQTT."""
        while True:
            try:
                await asyncio.sleep(self.config.update_interval)
                await self._publish_telemetry()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"[MQTT] Error publishing telemetry: {e}")
                await asyncio.sleep(5)

    async def _publish_telemetry(self) -> None:
        """Read current telemetry from the UDP protocol and publish to MQTT."""
        protocol = self._udp_protocol_getter()
        if protocol is None or self._client is None:
            return

        # Power sensors
        await self._sensors["battery_voltage"].send_state(
            self._client, round(protocol.battery_voltage, 2)
        )
        await self._sensors["battery_percent"].send_state(
            self._client, protocol.battery_percent
        )
        await self._sensors["solar_voltage"].send_state(
            self._client, round(protocol.solar_voltage, 2)
        )
        await self._sensors["is_charging"].send_state(
            self._client, protocol.is_charging
        )

        # WiFi / connectivity
        await self._sensors["esp_rssi"].send_state(
            self._client, protocol.esp_rssi
        )
        await self._sensors["esp_wifi_connected"].send_state(
            self._client, protocol.esp_wifi_connected
        )

        # Stream stats
        await self._sensors["packets_received"].send_state(
            self._client, protocol.packets_received
        )
        await self._sensors["stream_uptime"].send_state(
            self._client, int(protocol.stream_uptime_seconds)
        )

        # ESP32 UDP diagnostics
        await self._sensors["esp_udp_errors"].send_state(
            self._client, protocol.esp_udp_errors
        )
        await self._sensors["esp_udp_dropped"].send_state(
            self._client, protocol.esp_udp_dropped
        )
