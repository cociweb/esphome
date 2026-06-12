import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from .. import CONF_A2DP_SINK_ID, A2DPSink, a2dp_sink_ns

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp_sink"]

A2DPSinkTextSensor = a2dp_sink_ns.class_(
    "A2DPSinkTextSensor",
    text_sensor.TextSensor,
    cg.Component,
)

CONFIG_SCHEMA = text_sensor.text_sensor_schema(A2DPSinkTextSensor).extend(
    {
        cv.GenerateID(CONF_A2DP_SINK_ID): cv.use_id(A2DPSink),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_A2DP_SINK_ID])
