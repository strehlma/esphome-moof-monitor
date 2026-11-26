"""Text sensor platform for VanMoof BLE."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import VanMoofBLE, vanmoof_ble_ns

DEPENDENCIES = ["vanmoof_ble"]

CONF_VANMOOF_BLE_ID = "vanmoof_ble_id"
CONF_FRAME_NUMBER = "frame_number"
CONF_VERSION = "version"
CONF_ALARM = "alarm"
CONF_ALARM_SETTING = "alarm_setting"
CONF_MODULE_STATE = "module_state"
CONF_FIRMWARE = "firmware"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VANMOOF_BLE_ID): cv.use_id(VanMoofBLE),
        cv.Optional(CONF_FRAME_NUMBER): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_VERSION): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_ALARM): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_ALARM_SETTING): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_MODULE_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FIRMWARE): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    """Generate code for VanMoof BLE text sensors."""
    parent = await cg.get_variable(config[CONF_VANMOOF_BLE_ID])

    if frame_number_config := config.get(CONF_FRAME_NUMBER):
        sens = await text_sensor.new_text_sensor(frame_number_config)
        cg.add(parent.set_frame_number_sensor(sens))

    if version_config := config.get(CONF_VERSION):
        sens = await text_sensor.new_text_sensor(version_config)
        cg.add(parent.set_version_sensor(sens))

    for key, setter in (
        (CONF_ALARM, parent.set_alarm_sensor),
        (CONF_ALARM_SETTING, parent.set_alarm_setting_sensor),
        (CONF_MODULE_STATE, parent.set_module_state_sensor),
        (CONF_FIRMWARE, parent.set_firmware_sensor),
    ):
        if conf := config.get(key):
            sens = await text_sensor.new_text_sensor(conf)
            cg.add(setter(sens))
