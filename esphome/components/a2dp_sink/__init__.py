from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SAMPLE_RATE
from esphome.components.a2dp import CONF_A2DP_ID, A2DP
from esphome.core import ID
from esphome.cpp_generator import TemplateArgsType
from esphome.types import ConfigType

AUTO_LOAD = ["a2dp"]
CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp"]
DOMAIN = "a2dp_sink"

CONF_A2DP_SINK_ID = "a2dp_sink_id"
CONF_PCM_DRAIN_THROTTLE = "pcm_drain_throttle"
CONF_SPEAKER_OUTPUT_DELAY = "speaker_output_delay"
CONF_SPEAKER_PIPELINE_DELAY = "speaker_pipeline_delay"

a2dp_sink_ns = cg.esphome_ns.namespace("a2dp_sink")
A2DPSink = a2dp_sink_ns.class_("A2DPSink", cg.Component, cg.Parented.template(A2DP))

A2DPSinkEnableAction = a2dp_sink_ns.class_(
    "A2DPSinkEnableAction",
    automation.Action,
    cg.Parented.template(A2DPSink),
)
A2DPSinkDisableAction = a2dp_sink_ns.class_(
    "A2DPSinkDisableAction",
    automation.Action,
    cg.Parented.template(A2DPSink),
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(A2DPSink),
            cv.GenerateID(CONF_A2DP_ID): cv.use_id(A2DP),
            cv.Optional(CONF_SAMPLE_RATE, default=44100): cv.one_of(
                8000, 11025, 16000, 22050, 32000, 44100, 48000, int=True
            ),
            cv.Optional(CONF_PCM_DRAIN_THROTTLE, default="500ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SPEAKER_OUTPUT_DELAY, default="200ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SPEAKER_PIPELINE_DELAY, default="200ms"): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)

A2DP_SINK_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.Schema({cv.GenerateID(): cv.use_id(A2DPSink)})
)


@automation.register_action(
    "a2dp_sink.enable",
    A2DPSinkEnableAction,
    A2DP_SINK_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.disable",
    A2DPSinkDisableAction,
    A2DP_SINK_ACTION_SCHEMA,
    synchronous=True,
)
async def a2dp_sink_action_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: TemplateArgsType,
):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_A2DP_ID])

    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_pcm_drain_throttle_ms(config[CONF_PCM_DRAIN_THROTTLE].total_milliseconds))
    cg.add(var.set_output_delay_ms(config[CONF_SPEAKER_OUTPUT_DELAY].total_milliseconds))
    cg.add(var.set_pipeline_delay_ms(config[CONF_SPEAKER_PIPELINE_DELAY].total_milliseconds))

    cg.add_define("USE_A2DP_SINK")
