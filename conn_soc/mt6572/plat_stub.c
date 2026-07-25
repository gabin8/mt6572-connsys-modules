/* Stubs for downstream platform hooks absent on the mainline mt6572 port:
 * - cmb_stub: audio-path callback registry (FM/BT PCM routing) - not wired.
 * - deep-idle hints: bus dpidle vote, mainline pm handles idle states.
 * - chipid_query: on-die CONSYS, chip id == SoC id (stock behaviour).
 */
#include <linux/module.h>
#include "osal_typedef.h"
#include <mtk_wcn_cmb_stub.h>
#include "mtk_wcn_consys_hw.h"

int mtk_wcn_cmb_stub_reg(P_CMB_STUB_CB p_stub_cb)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_reg);

int mtk_wcn_cmb_stub_unreg(void)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_unreg);

int mt_combo_plt_enter_deep_idle(COMBO_IF src)
{
	return 0;
}
EXPORT_SYMBOL(mt_combo_plt_enter_deep_idle);

int mt_combo_plt_exit_deep_idle(COMBO_IF src)
{
	return 0;
}
EXPORT_SYMBOL(mt_combo_plt_exit_deep_idle);

int mtk_wcn_wmt_chipid_query(void)
{
	return PLATFORM_SOC_CHIP;
}
EXPORT_SYMBOL(mtk_wcn_wmt_chipid_query);
