"""ESPHome external component for Balboa GS510SZ / GS501SZ spa controllers.

Based on https://github.com/MagnusPer/Balboa-GS510SZ (protocol decoding and button injection).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, button, climate, light, select, sensor, switch, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_OUTPUT_ID,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    PLATFORM_ESP8266,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_SECOND,
)

CODEOWNERS = []
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "button", "climate", "light", "select", "switch"]

balboa_ns = cg.esphome_ns.namespace("balboa_gs")
BalboaGS = balboa_ns.class_("BalboaGS", cg.Component)
BalboaButton = balboa_ns.class_(
    "BalboaButton", button.Button, cg.Parented.template(BalboaGS)
)
BalboaClimate = balboa_ns.class_(
    "BalboaClimate", climate.Climate, cg.Parented.template(BalboaGS)
)
BalboaModeSelect = balboa_ns.class_(
    "BalboaModeSelect", select.Select, cg.Parented.template(BalboaGS)
)
MODE_OPTIONS = ["Standard", "Economy", "Sleep"]  # order must match SpaMode in balboa_gs.h
BalboaPumpSelect = balboa_ns.class_(
    "BalboaPumpSelect", select.Select, cg.Parented.template(BalboaGS)
)
PUMP_OPTIONS = ["Off", "Low", "High"]  # order must match PumpSpeed in balboa_gs.h
BalboaFilterSelect = balboa_ns.class_(
    "BalboaFilterSelect", select.Select, cg.Parented.template(BalboaGS)
)
FILTER_OPTIONS = ["2 hours", "4 hours", "6 hours", "8 hours", "Continuous"]  # must match FILTER_OPTION_LABELS in balboa_gs.cpp
# EXPERIMENTAL (untested): pump 2 on/off for tubs with a second pump
BalboaPump2Switch = balboa_ns.class_(
    "BalboaPump2Switch", switch.Switch, cg.Parented.template(BalboaGS)
)
BalboaLight = balboa_ns.class_(
    "BalboaLight", light.LightOutput, cg.Parented.template(BalboaGS)
)

# On/off light schema. Newer ESPHome has light_schema(); older uses BINARY_LIGHT_SCHEMA + output_id.
if hasattr(light, "light_schema") and hasattr(light, "LightType"):
    _LIGHT_SCHEMA = light.light_schema(BalboaLight, light.LightType.BINARY)
else:
    _LIGHT_SCHEMA = light.BINARY_LIGHT_SCHEMA.extend(
        {cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(BalboaLight)}
    )

CONF_CLOCK_PIN = "clock_pin"
CONF_DATA_PIN = "data_pin"
CONF_BUTTON_PIN = "button_pin"
CONF_MIN_TARGET = "min_target_temperature"
CONF_MAX_TARGET = "max_target_temperature"
CONF_LEARN_TARGET = "learn_target_on_boot"
CONF_POWER_SENSOR = "power_sensor"
CONF_PUMP_HIGH_MIN_POWER = "pump_high_min_power"
CONF_PUMP_HIGH_MAX_POWER = "pump_high_max_power"
CONF_PUMP_POWER_CHECK = "pump_power_check"

CONF_CLIMATE = "climate"
CONF_WATER_TEMPERATURE = "water_temperature"
CONF_TARGET_TEMPERATURE = "target_temperature"
CONF_HEATER = "heater"
CONF_PUMP = "pump"
CONF_LIGHTS = "lights"
CONF_DISPLAY = "display"
CONF_LIGHT = "light"
CONF_LAST_ACTION = "last_action"
CONF_MODE = "mode"
CONF_PUMP_SPEED = "pump_speed"
CONF_WATER_TEMPERATURE_AGE = "water_temperature_age"
CONF_STATUS_BITS = "status_bits"
CONF_FILTER_SETTING = "filter_setting"
CONF_PUMP2 = "pump2"                # EXPERIMENTAL, untested
CONF_PUMP2_SWITCH = "pump2_switch"  # EXPERIMENTAL, untested

# (yaml key, ButtonAction value in balboa_gs.h, icon)
BUTTONS = (
    ("mode_button", 0, "mdi:auto-mode"),
    ("temp_up_button", 1, "mdi:thermometer-chevron-up"),
    ("temp_down_button", 2, "mdi:thermometer-chevron-down"),
    ("lights_button", 3, "mdi:lightbulb-group"),
    ("pump_button", 4, "mdi:pump"),
    ("stop_button", 5, "mdi:close-octagon"),
    ("read_filter_button", 6, "mdi:air-filter"),
    ("pump2_button", 8, "mdi:pump"),  # EXPERIMENTAL, untested
    ("pump3_button", 9, "mdi:pump"),  # EXPERIMENTAL, untested (no pump 3 status bit is known)
    ("test_filter_button", 7, "mdi:test-tube"),
)


def _temp_sensor(icon=None):
    kwargs = dict(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )
    if icon:
        kwargs["icon"] = icon
    return sensor.sensor_schema(**kwargs)


_schema = {
    cv.GenerateID(): cv.declare_id(BalboaGS),
    cv.Required(CONF_CLOCK_PIN): pins.internal_gpio_input_pin_schema,
    cv.Required(CONF_DATA_PIN): pins.internal_gpio_input_pin_schema,
    cv.Required(CONF_BUTTON_PIN): pins.internal_gpio_output_pin_schema,
    cv.Optional(CONF_MIN_TARGET, default=26.0): cv.temperature,
    cv.Optional(CONF_MAX_TARGET, default=40.0): cv.temperature,
    cv.Optional(CONF_LEARN_TARGET, default=True): cv.boolean,
    cv.Optional(CONF_POWER_SENSOR): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_PUMP_HIGH_MIN_POWER, default=1000.0): cv.positive_float,
    cv.Optional(CONF_PUMP_HIGH_MAX_POWER, default=2100.0): cv.positive_float,
    cv.Optional(CONF_CLIMATE): climate.climate_schema(BalboaClimate),
    cv.Optional(CONF_WATER_TEMPERATURE): _temp_sensor(),
    cv.Optional(CONF_TARGET_TEMPERATURE): _temp_sensor("mdi:temperature-celsius"),
    cv.Optional(CONF_HEATER): binary_sensor.binary_sensor_schema(
        icon="mdi:heating-coil"
    ),
    cv.Optional(CONF_PUMP): binary_sensor.binary_sensor_schema(icon="mdi:pump"),
    cv.Optional(CONF_PUMP2): binary_sensor.binary_sensor_schema(icon="mdi:pump"),
    cv.Optional(CONF_PUMP2_SWITCH): switch.switch_schema(BalboaPump2Switch, icon="mdi:pump"),
    cv.Optional(CONF_LIGHTS): binary_sensor.binary_sensor_schema(
        icon="mdi:lightbulb-group"
    ),
    cv.Optional(CONF_DISPLAY): text_sensor.text_sensor_schema(icon="mdi:hot-tub"),
    cv.Optional(CONF_LIGHT): _LIGHT_SCHEMA,
    cv.Optional(CONF_MODE): select.select_schema(BalboaModeSelect, icon="mdi:auto-mode"),
    cv.Optional(CONF_PUMP_SPEED): select.select_schema(BalboaPumpSelect, icon="mdi:pump"),
    cv.Optional(CONF_WATER_TEMPERATURE_AGE): sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:timer-sand",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    cv.Optional(CONF_STATUS_BITS): text_sensor.text_sensor_schema(
        icon="mdi:bug-outline", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    cv.Optional(CONF_FILTER_SETTING): select.select_schema(BalboaFilterSelect, icon="mdi:air-filter"),
    cv.Optional(CONF_PUMP_POWER_CHECK): text_sensor.text_sensor_schema(
        icon="mdi:flash", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    cv.Optional(CONF_LAST_ACTION): text_sensor.text_sensor_schema(
        icon="mdi:history", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}
for _key, _action, _icon in BUTTONS:
    _schema[cv.Optional(_key)] = button.button_schema(BalboaButton, icon=_icon)

CONFIG_SCHEMA = cv.All(
    cv.Schema(_schema).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP8266]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_clock_pin(await cg.gpio_pin_expression(config[CONF_CLOCK_PIN])))
    cg.add(var.set_data_pin(await cg.gpio_pin_expression(config[CONF_DATA_PIN])))
    cg.add(var.set_button_pin(await cg.gpio_pin_expression(config[CONF_BUTTON_PIN])))
    cg.add(var.set_min_target_temperature(config[CONF_MIN_TARGET]))
    cg.add(var.set_max_target_temperature(config[CONF_MAX_TARGET]))
    cg.add(var.set_learn_target_on_boot(config[CONF_LEARN_TARGET]))
    cg.add(var.set_pump_high_power(config[CONF_PUMP_HIGH_MIN_POWER], config[CONF_PUMP_HIGH_MAX_POWER]))
    if CONF_POWER_SENSOR in config:
        cg.add(var.set_power_sensor(await cg.get_variable(config[CONF_POWER_SENSOR])))

    if conf := config.get(CONF_CLIMATE):
        c = await climate.new_climate(conf)
        await cg.register_parented(c, var)
        cg.add(var.set_climate(c))

    for key, setter in (
        (CONF_WATER_TEMPERATURE, var.set_water_temperature_sensor),
        (CONF_TARGET_TEMPERATURE, var.set_target_temperature_sensor),
    ):
        if conf := config.get(key):
            cg.add(setter(await sensor.new_sensor(conf)))

    for key, setter in (
        (CONF_HEATER, var.set_heater_binary_sensor),
        (CONF_PUMP, var.set_pump_binary_sensor),
        (CONF_PUMP2, var.set_pump2_binary_sensor),
        (CONF_LIGHTS, var.set_lights_binary_sensor),
    ):
        if conf := config.get(key):
            cg.add(setter(await binary_sensor.new_binary_sensor(conf)))

    if conf := config.get(CONF_DISPLAY):
        cg.add(var.set_display_text_sensor(await text_sensor.new_text_sensor(conf)))

    if conf := config.get(CONF_WATER_TEMPERATURE_AGE):
        cg.add(var.set_water_age_sensor(await sensor.new_sensor(conf)))

    if conf := config.get(CONF_STATUS_BITS):
        cg.add(var.set_status_bits_text_sensor(await text_sensor.new_text_sensor(conf)))

    if conf := config.get(CONF_PUMP_SPEED):
        sel = await select.new_select(conf, options=PUMP_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_pump_select(sel))

    if conf := config.get(CONF_PUMP2_SWITCH):
        sw = await switch.new_switch(conf)
        await cg.register_parented(sw, var)
        cg.add(var.set_pump2_switch(sw))

    if conf := config.get(CONF_MODE):
        sel = await select.new_select(conf, options=MODE_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_mode_select(sel))

    if conf := config.get(CONF_FILTER_SETTING):
        sel = await select.new_select(conf, options=FILTER_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_filter_select(sel))

    if conf := config.get(CONF_PUMP_POWER_CHECK):
        cg.add(var.set_power_check_text_sensor(await text_sensor.new_text_sensor(conf)))

    if conf := config.get(CONF_LAST_ACTION):
        cg.add(var.set_last_action_text_sensor(await text_sensor.new_text_sensor(conf)))

    if conf := config.get(CONF_LIGHT):
        if hasattr(light, "new_light"):
            out = await light.new_light(conf)
        else:
            out = cg.new_Pvariable(conf[CONF_OUTPUT_ID])
            await light.register_light(out, conf)
        await cg.register_parented(out, var)
        cg.add(var.set_light_output(out))

    for key, action, _icon in BUTTONS:
        if conf := config.get(key):
            btn = await button.new_button(conf)
            await cg.register_parented(btn, var)
            cg.add(btn.set_action(action))
