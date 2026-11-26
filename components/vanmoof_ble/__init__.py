"""VanMoof BLE custom component for ESPHome."""
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import ble_client
from esphome.const import CONF_ESPHOME, CONF_ID, CONF_NAME, CONF_PROJECT, CONF_VERSION

# Component version. The C++ code gets it as a define. vanmoof/bike.yaml repeats
# it as esphome.project.version (a component cannot set the project itself);
# _final_validate stops the build if the two differ.
COMPONENT_VERSION = "1.11.0"
PROJECT_NAME = "vanmoof_monitor.esp32"

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]

vanmoof_ble_ns = cg.esphome_ns.namespace("vanmoof_ble")
VanMoofBLE = vanmoof_ble_ns.class_(
    "VanMoofBLE", ble_client.BLEClientNode, cg.PollingComponent
)

CONF_ENCRYPTION_KEY = "encryption_key"
CONF_KEY_ID = "key_id"
CONF_KEEP_CONNECTED = "keep_connected"

CONFIG_SCHEMA = cv.All(
    cv.ensure_list(
        cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(VanMoofBLE),
                cv.Required(CONF_ENCRYPTION_KEY): cv.string,
                cv.Required(CONF_KEY_ID): cv.int_range(min=1, max=255),
                cv.Optional(CONF_KEEP_CONNECTED, default=False): cv.boolean,
            }
        )
        .extend(cv.polling_component_schema("60s"))
        .extend(ble_client.BLE_CLIENT_SCHEMA)
    ),
)


def _final_validate(config):
    """Keep the project version of the bike package in step with the code."""
    project = fv.full_config.get()[CONF_ESPHOME].get(CONF_PROJECT)
    if project and project[CONF_NAME] == PROJECT_NAME and project[CONF_VERSION] != COMPONENT_VERSION:
        raise cv.Invalid(
            f"esphome.project.version is '{project[CONF_VERSION]}', but the vanmoof_ble component is "
            f"'{COMPONENT_VERSION}' - update the version in vanmoof/bike.yaml"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    """Generate code for VanMoof BLE component."""
    cg.add_define("VANMOOF_BLE_COMPONENT_VERSION", COMPONENT_VERSION)
    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)
        await ble_client.register_ble_node(var, conf)

        cg.add(var.set_encryption_key(conf[CONF_ENCRYPTION_KEY]))
        cg.add(var.set_key_id(conf[CONF_KEY_ID]))
        cg.add(var.set_keep_connected(conf[CONF_KEEP_CONNECTED]))
