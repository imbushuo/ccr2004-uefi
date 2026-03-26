/*
 * Stubs for PBS utility functions needed by the SPI HAL.
 * On Alpine V2, dev_id = 1 (PBS_UNIT_CHIP_ID_DEV_ID_ALPINE_V2), rev_id = 0.
 */
#include "al_hal_pbs_regs.h"

unsigned int al_pbs_dev_id_get(void *pbs_regs)
{
	(void)pbs_regs;
	return PBS_UNIT_CHIP_ID_DEV_ID_ALPINE_V2;
}

unsigned int al_pbs_dev_rev_id_get(void *pbs_regs)
{
	(void)pbs_regs;
	return 0;
}
