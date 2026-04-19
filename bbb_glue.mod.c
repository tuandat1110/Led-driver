#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

KSYMTAB_DATA(led_gpios, "_gpl", "");
KSYMTAB_FUNC(glue_register_led, "_gpl", "");
KSYMTAB_FUNC(glue_unregister_led, "_gpl", "");
KSYMTAB_FUNC(glue_handle_button_event, "_gpl", "");

MODULE_INFO(depends, "");

