import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import (
    CONF_ID,
    CONF_WIDTH,
    CONF_HEIGHT,
    CONF_TRIGGER_ID,
)

CODEOWNERS = ["@ghidosoft"]
DEPENDENCIES = ["psram", "http_request"]
AUTO_LOAD = ["image"]

async_online_image_ns = cg.esphome_ns.namespace("async_online_image")
AsyncOnlineImageComponent = async_online_image_ns.class_(
    "AsyncOnlineImageComponent", cg.Component
)
SlotReadyTrigger = async_online_image_ns.class_(
    "SlotReadyTrigger", automation.Trigger.template(cg.size_t)
)
AllReadyTrigger = async_online_image_ns.class_(
    "AllReadyTrigger", automation.Trigger.template()
)
SlotErrorTrigger = async_online_image_ns.class_(
    "SlotErrorTrigger", automation.Trigger.template(cg.size_t)
)

CONF_SLOTS = "slots"
CONF_HTTP_TIMEOUT = "http_timeout"
CONF_ON_SLOT_READY = "on_slot_ready"
CONF_ON_ALL_READY = "on_all_ready"
CONF_ON_ERROR = "on_error"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AsyncOnlineImageComponent),
        cv.Optional(CONF_SLOTS, default=1): cv.int_range(min=1, max=16),
        cv.Optional(CONF_WIDTH, default=256): cv.positive_int,
        cv.Optional(CONF_HEIGHT, default=256): cv.positive_int,
        cv.Optional(
            CONF_HTTP_TIMEOUT, default="10s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ON_SLOT_READY): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SlotReadyTrigger),
            }
        ),
        cv.Optional(CONF_ON_ALL_READY): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AllReadyTrigger),
            }
        ),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SlotErrorTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_slots_count(config[CONF_SLOTS]))
    cg.add(var.set_dimensions(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_http_timeout(config[CONF_HTTP_TIMEOUT]))

    for conf in config.get(CONF_ON_SLOT_READY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.size_t, "x")], conf)

    for conf in config.get(CONF_ON_ALL_READY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.size_t, "x")], conf)

    cg.add_library("pngle", None)
