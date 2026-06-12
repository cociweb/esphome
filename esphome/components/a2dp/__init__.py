import esphome.codegen as cg

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["esp32"]

CONF_A2DP_ID = "a2dp_id"

a2dp_ns = cg.esphome_ns.namespace("a2dp")

# This component is a pure namespace/utility component.  It is AUTO_LOADED by
# a2dp_sink, which is responsible for setting all sdkconfig options and defines.
