import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import component
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_USERNAME

CONF_ROOT_PATH = "root_path"

ftp_server_ns = cg.esphome_ns.namespace("ftp_server")
FTPServer = ftp_server_ns.class_("FTPServer", component.Component)

CONFIG_SCHEMA = component.Schema(
    {
        cv.GenerateID(): cv.declare_id(FTPServer),
        cv.Optional(CONF_PORT, default=21): cv.port,
        cv.Optional(CONF_USERNAME, default="admin"): cv.string,
        cv.Optional(CONF_PASSWORD, default="admin"): cv.string,
        cv.Optional(CONF_ROOT_PATH, default="/sdcard"): cv.string,
    }
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await component.register_component(var, config)
    
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_root_path(config[CONF_ROOT_PATH]))
