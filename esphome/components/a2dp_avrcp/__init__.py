from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.a2dp import CONF_A2DP_ID, A2DP, a2dp_ns
from esphome.const import CONF_ID
from esphome.core import ID
from esphome.cpp_generator import TemplateArgsType
from esphome.types import ConfigType

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp"]

CONF_ON_VOLUME_CHANGED = "on_volume_changed"

A2DPAVRCP = a2dp_ns.class_(
    "A2DPAVRCP",
    cg.Component,
    cg.Parented.template(A2DP),
)

A2DPAVRCPPlayAction       = a2dp_ns.class_("A2DPAVRCPPlayAction",       automation.Action)
A2DPAVRCPPauseAction      = a2dp_ns.class_("A2DPAVRCPPauseAction",      automation.Action)
A2DPAVRCPPlayPauseAction  = a2dp_ns.class_("A2DPAVRCPPlayPauseAction",  automation.Action)
A2DPAVRCPNextAction       = a2dp_ns.class_("A2DPAVRCPNextAction",       automation.Action)
A2DPAVRCPPreviousAction   = a2dp_ns.class_("A2DPAVRCPPreviousAction",   automation.Action)
A2DPAVRCPStopAction       = a2dp_ns.class_("A2DPAVRCPStopAction",       automation.Action)
A2DPAVRCPVolumeUpAction   = a2dp_ns.class_("A2DPAVRCPVolumeUpAction",   automation.Action)
A2DPAVRCPVolumeDownAction = a2dp_ns.class_("A2DPAVRCPVolumeDownAction", automation.Action)

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
            cv.GenerateID(): cv.declare_id(A2DPAVRCP),
            cv.GenerateID(CONF_A2DP_ID): cv.use_id(A2DP),
            cv.Optional(CONF_ON_VOLUME_CHANGED): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

AVRCP_SIMPLE_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.Schema({cv.GenerateID(): cv.use_id(A2DPAVRCP)})
)


@automation.register_action(
    "a2dp_avrcp.play",
    A2DPAVRCPPlayAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.pause",
    A2DPAVRCPPauseAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.play_pause",
    A2DPAVRCPPlayPauseAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.next",
    A2DPAVRCPNextAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.previous",
    A2DPAVRCPPreviousAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.stop",
    A2DPAVRCPStopAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.volume_up",
    A2DPAVRCPVolumeUpAction,
    AVRCP_SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_avrcp.volume_down",
    A2DPAVRCPVolumeDownAction,
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
    await cg.register_parented(var, config[CONF_A2DP_ID])
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)
    cg.add_define("USE_A2DP_AVRCP")
