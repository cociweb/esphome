import esphome.codegen as cg
from esphome.components import media_source, psram
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TASK_STACK_IN_PSRAM
from esphome.types import ConfigType

from .. import CONF_A2DP_SINK_ID, A2DPSink, a2dp_sink_ns

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp_sink"]
AUTO_LOAD = ["audio"]

A2DPSinkMediaSource = a2dp_sink_ns.class_(
    "A2DPSinkMediaSource",
    cg.Component,
    media_source.MediaSource,
)


def validate_task_stack_in_psram(value):
    if hasattr(psram, "validate_task_stack_in_psram"):
        return psram.validate_task_stack_in_psram(value)
    if value := cv.boolean(value):
        return cv.requires_component("psram")(value)
    return value


def request_external_task_stack() -> None:
    if hasattr(psram, "request_external_task_stack"):
        psram.request_external_task_stack()
    else:
        add_idf_sdkconfig_option("CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY", True)


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(A2DPSinkMediaSource)
    .extend(
        {
            cv.GenerateID(CONF_A2DP_SINK_ID): cv.use_id(A2DPSink),
            cv.Optional(CONF_TASK_STACK_IN_PSRAM): validate_task_stack_in_psram,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)
    await cg.register_parented(var, config[CONF_A2DP_SINK_ID])

    if config.get(CONF_TASK_STACK_IN_PSRAM):
        cg.add(var.set_task_stack_in_psram(True))
        request_external_task_stack()
