from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SAMPLE_RATE
from esphome.core import ID
from esphome.cpp_generator import TemplateArgsType
from esphome.types import ConfigType

AUTO_LOAD = ["a2dp", "ring_buffer"]
CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["esp32"]
DOMAIN = "a2dp_sink"

CONF_A2DP_SINK_ID = "a2dp_sink_id"

CONF_DEVICE_NAME = "device_name"
CONF_AUTO_START = "auto_start"
CONF_RING_BUFFER_SIZE = "ring_buffer_size"
CONF_USE_PSRAM = "use_psram"
CONF_PCM_DRAIN_THROTTLE = "pcm_drain_throttle"
CONF_SPEAKER_OUTPUT_DELAY = "speaker_output_delay"
CONF_SPEAKER_PIPELINE_DELAY = "speaker_pipeline_delay"
CONF_COEXISTENCE = "coexistence"
CONF_SOFTWARE_COEXISTENCE = "software_coexistence"
CONF_PREFER_BT_WHILE_STREAMING = "prefer_bt_while_streaming"
CONF_PREFER_BT_WHILE_DISCOVERABLE = "prefer_bt_while_discoverable"
CONF_PAUSE_WIFI_SOURCES_ON_CONNECT = "pause_wifi_sources_on_connect"

a2dp_sink_ns = cg.esphome_ns.namespace("a2dp_sink")
A2DPSink = a2dp_sink_ns.class_("A2DPSink", cg.Component)

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

COEXISTENCE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SOFTWARE_COEXISTENCE, default=True): cv.boolean,
        cv.Optional(CONF_PREFER_BT_WHILE_STREAMING, default=True): cv.boolean,
        cv.Optional(CONF_PREFER_BT_WHILE_DISCOVERABLE, default=False): cv.boolean,
        cv.Optional(CONF_PAUSE_WIFI_SOURCES_ON_CONNECT, default=False): cv.boolean,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(A2DPSink),
            cv.Optional(CONF_DEVICE_NAME, default="ESPHome"): cv.string,
            cv.Optional(CONF_AUTO_START, default=False): cv.boolean,
            cv.Optional(CONF_RING_BUFFER_SIZE, default=131072): cv.int_range(
                min=16384, max=4194304
            ),
            cv.Optional(CONF_USE_PSRAM, default=False): cv.boolean,
            cv.Optional(CONF_SAMPLE_RATE, default=44100): cv.one_of(
                8000, 11025, 16000, 22050, 32000, 44100, 48000, int=True
            ),
            cv.Optional(CONF_PCM_DRAIN_THROTTLE, default="500ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SPEAKER_OUTPUT_DELAY, default="200ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SPEAKER_PIPELINE_DELAY, default="200ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_COEXISTENCE): COEXISTENCE_SCHEMA,
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

    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))
    cg.add(var.set_ring_buffer_size(config[CONF_RING_BUFFER_SIZE]))
    cg.add(var.set_use_psram(config[CONF_USE_PSRAM]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_pcm_drain_throttle_ms(config[CONF_PCM_DRAIN_THROTTLE].total_milliseconds))
    cg.add(var.set_output_delay_ms(config[CONF_SPEAKER_OUTPUT_DELAY].total_milliseconds))
    cg.add(var.set_pipeline_delay_ms(config[CONF_SPEAKER_PIPELINE_DELAY].total_milliseconds))

    if coex := config.get(CONF_COEXISTENCE):
        cg.add(var.set_software_coexistence(coex[CONF_SOFTWARE_COEXISTENCE]))
        cg.add(var.set_prefer_bt_while_streaming(coex[CONF_PREFER_BT_WHILE_STREAMING]))
        cg.add(var.set_prefer_bt_while_discoverable(coex[CONF_PREFER_BT_WHILE_DISCOVERABLE]))
        cg.add(var.set_pause_wifi_sources_on_connect(coex[CONF_PAUSE_WIFI_SOURCES_ON_CONNECT]))

    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_A2DP_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_AVRC_TG_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BT_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)

    cg.add_define("USE_A2DP_SINK")
