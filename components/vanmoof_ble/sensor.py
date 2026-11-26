"""Sensor platform for VanMoof BLE."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_DISTANCE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_PERCENT,
    UNIT_KILOMETER,
)

from . import VanMoofBLE, vanmoof_ble_ns

DEPENDENCIES = ["vanmoof_ble"]

CONF_VANMOOF_BLE_ID = "vanmoof_ble_id"
CONF_BATTERY_LEVEL = "battery_level"
CONF_DISTANCE = "distance"
CONF_MODULE_BATTERY_LEVEL = "module_battery_level"
CONF_ERROR_CODE = "error_code"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VANMOOF_BLE_ID): cv.use_id(VanMoofBLE),
        cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_DISTANCE): sensor.sensor_schema(
            unit_of_measurement=UNIT_KILOMETER,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_DISTANCE,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
        cv.Optional(CONF_MODULE_BATTERY_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ERROR_CODE): sensor.sensor_schema(
            accuracy_decimals=0,
        ),
    }
)


async def to_code(config):
    """Generate code for VanMoof BLE sensors."""
    parent = await cg.get_variable(config[CONF_VANMOOF_BLE_ID])

    if battery_config := config.get(CONF_BATTERY_LEVEL):
        sens = await sensor.new_sensor(battery_config)
        cg.add(parent.set_battery_sensor(sens))

    if distance_config := config.get(CONF_DISTANCE):
        sens = await sensor.new_sensor(distance_config)
        cg.add(parent.set_distance_sensor(sens))

    if module_battery_config := config.get(CONF_MODULE_BATTERY_LEVEL):
        sens = await sensor.new_sensor(module_battery_config)
        cg.add(parent.set_module_battery_sensor(sens))

    if error_code_config := config.get(CONF_ERROR_CODE):
        sens = await sensor.new_sensor(error_code_config)
        cg.add(parent.set_error_code_sensor(sens))
