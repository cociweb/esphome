import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_NAME, CONF_TYPE
from esphome.types import ConfigType

from .. import CONF_A2DP_ID, A2DP, a2dp_ns

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp_avrcp"]

CONF_ALBUM_ARTIST = "album_artist"

METADATA_TYPES = {
    "title": "ESP_AVRC_MD_ATTR_TITLE",
    "artist": "ESP_AVRC_MD_ATTR_ARTIST",
    "album": "ESP_AVRC_MD_ATTR_ALBUM",
    CONF_ALBUM_ARTIST: "0",
}

A2DPTextSensor = a2dp_ns.class_(
    "A2DPTextSensor",
    text_sensor.TextSensor,
    cg.Component,
)

def add_a2dp_name_suffix(config):
    config = config.copy()
    if CONF_NAME in config and config[CONF_NAME] and "a2dp" not in config[CONF_NAME].lower():
        config[CONF_NAME] = f"{config[CONF_NAME]} A2DP"
    return config


CONFIG_SCHEMA = cv.All(
    add_a2dp_name_suffix,
    text_sensor.text_sensor_schema(A2DPTextSensor).extend(
        {
            cv.GenerateID(CONF_A2DP_ID): cv.use_id(A2DP),
            cv.Required(CONF_TYPE): cv.one_of(*METADATA_TYPES, lower=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config: ConfigType) -> None:
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_A2DP_ID])
    cg.add(var.set_metadata_attr(cg.RawExpression(METADATA_TYPES[config[CONF_TYPE]])))
