/* Downstream platform hooks on the mainline mt6572 port:
 * - cmb_stub: callback registry; only the audio-interface one is used (FM
 *   switches CONSYS to digital I2S output with it).
 * - deep-idle hints: bus dpidle vote, mainline pm handles idle states.
 * - chipid_query: on-die CONSYS, chip id == SoC id (stock behaviour).
 */
#include <linux/module.h>
#include "osal_typedef.h"
#include <mtk_wcn_cmb_stub.h>
#include "mtk_wcn_consys_hw.h"

static wmt_aif_ctrl_cb cmb_stub_aif_ctrl_cb;

int mtk_wcn_cmb_stub_reg(P_CMB_STUB_CB p_stub_cb)
{
	if (!p_stub_cb || p_stub_cb->size != sizeof(*p_stub_cb))
		return -1;
	cmb_stub_aif_ctrl_cb = p_stub_cb->aif_ctrl_cb;
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_reg);

int mtk_wcn_cmb_stub_unreg(void)
{
	cmb_stub_aif_ctrl_cb = NULL;
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_unreg);

/* Set the CONSYS audio interface (BT PCM / FM analog or I2S), host and chip side. */
int mtk_wcn_cmb_stub_aif_ctrl(CMB_STUB_AIF_X state, CMB_STUB_AIF_CTRL ctrl)
{
	if (!cmb_stub_aif_ctrl_cb)
		return -1;
	return cmb_stub_aif_ctrl_cb(state, ctrl);
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_aif_ctrl);

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
