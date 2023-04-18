import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, lock, output, text_sensor, uart
from esphome.const import (
    CONF_EVENT,
    CONF_ID,
    CONF_OUTPUT,
    CONF_UART_ID,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_BATTERY,
    ICON_ACCOUNT_CHECK
)


CODEOWNERS = ["@ravngr"]

CONF_LOW_BATTERY = "low_battery"


yale_smart_lock_ns = cg.esphome_ns.namespace("esphome::yale")
YaleSmartLock = yale_smart_lock_ns.class_("YaleSmartLock", lock.Lock, cg.Component, uart.UARTDevice)


CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(YaleSmartLock),
    cv.Required(CONF_OUTPUT): cv.use_id(output.BinaryOutput),
    cv.Optional(CONF_LOW_BATTERY): binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_BATTERY
    ),
    cv.Optional(CONF_EVENT): text_sensor.text_sensor_schema(
        icon=ICON_ACCOUNT_CHECK,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    )
}).extend(lock.LOCK_SCHEMA).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)


async def to_code(config):
    uart_component = await cg.get_variable(config[CONF_UART_ID])
    output_component = await cg.get_variable(config[CONF_OUTPUT])

    var = cg.new_Pvariable(config[CONF_ID], uart_component, output_component)

    await cg.register_component(var, config)
    await lock.register_lock(var, config)

    if CONF_LOW_BATTERY in config:
        low_battery_sensor = await binary_sensor.new_binary_sensor(config[CONF_LOW_BATTERY])
        cg.add(var.set_low_battery_sensor(low_battery_sensor))

    if CONF_EVENT in config:
        event_sensor = await text_sensor.new_text_sensor(config[CONF_EVENT])
        cg.add(var.set_event_text_sensor(event_sensor))
