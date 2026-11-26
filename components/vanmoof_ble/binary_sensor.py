"""Binary sensor platform for VanMoof BLE."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, DEVICE_CLASS_BATTERY_CHARGING

from . import VanMoofBLE, vanmoof_ble_ns

DEPENDENCIES = ["vanmoof_ble"]

CONF_VANMOOF_BLE_ID = "vanmoof_ble_id"
CONF_LOCK_STATE = "lock_state"
CONF_CHARGING = "charging"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VANMOOF_BLE_ID): cv.use_id(VanMoofBLE),
        cv.Optional(CONF_LOCK_STATE): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_CHARGING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY_CHARGING,
        ),
    }
)


async def to_code(config):
    """Generate code for VanMoof BLE binary sensors."""
    parent = await cg.get_variable(config[CONF_VANMOOF_BLE_ID])

    if lock_state_config := config.get(CONF_LOCK_STATE):
        sens = await binary_sensor.new_binary_sensor(lock_state_config)
        cg.add(parent.set_lock_state_sensor(sens))

    if charging_config := config.get(CONF_CHARGING):
        sens = await binary_sensor.new_binary_sensor(charging_config)
        cg.add(parent.set_charging_sensor(sens))
