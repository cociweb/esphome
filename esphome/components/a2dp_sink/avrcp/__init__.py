from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import ID
from esphome.cpp_generator import TemplateArgsType
from esphome.types import ConfigType

from .. import CONF_A2DP_SINK_ID, A2DPSink, a2dp_sink_ns

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp_sink"]

CONF_ON_VOLUME_CHANGED = "on_volume_changed"

A2DPSinkAVRCP = a2dp_sink_ns.class_(
    "A2DPSinkAVRCP",
    cg.Component,
    cg.Parented.template(A2DPSink),
)

A2DPSinkAVRCPPlayAction      = a2dp_sink_ns.class_("A2DPSinkAVRCPPlayAction",      automation.Action)
A2DPSinkAVRCPPauseAction     = a2dp_sink_ns.class_("A2DPSinkAVRCPPauseAction",     automation.Action)
A2DPSinkAVRCPNextAction      = a2dp_sink_ns.class_("A2DPSinkAVRCPNextAction",      automation.Action)
A2DPSinkAVRCPPreviousAction  = a2dp_sink_ns.class_("A2DPSinkAVRCPPreviousAction",  automation.Action)
A2DPSinkAVRCPStopAction      = a2dp_sink_ns.class_("A2DPSinkAVRCPStopAction",      automation.Action)
A2DPSinkAVRCPVolumeUpAction  = a2dp_sink_ns.class_("A2DPSinkAVRCPVolumeUpAction",  automation.Action)
A2DPSinkAVRCPVolumeDownAction = a2dp_sink_ns.class_("A2DPSinkAVRCPVolumeDownAction", automation.Action)

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(
        CONF_ON_VOLUME_CHANGED,
        "add_on_volume_callback",
        [(cg.uint8, "x")],
    ),
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(A2DPSinkAVRCP),
            cv.GenerateID(CONF_A2DP_SINK_ID): cv.use_id(A2DPSink),
            cv.Optional(CONF_ON_VOLUME_CHANGED): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

AVRCP_SIMPLE_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.Schema({cv.GenerateID(): cv.use_id(A2DPSinkAVRCP)})
)


@automation.register_action(
    "a2dp_sink.avrcp.play",
    A2DPSinkAVRCPPlayAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.avrcp.pause",
    A2DPSinkAVRCPPauseAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.avrcp.next",
    A2DPSinkAVRCPNextAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.avrcp.previous",
    A2DPSinkAVRCPPreviousAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.avrcp.stop",
    A2DPSinkAVRCPStopAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.avrcp.volume_up",
    A2DPSinkAVRCPVolumeUpAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_sink.avrcp.volume_down",
    A2DPSinkAVRCPVolumeDownAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
async def avrcp_action_to_code(
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
    await cg.register_parented(var, config[CONF_A2DP_SINK_ID])
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)
    cg.add_define("USE_A2DP_AVRCP")
