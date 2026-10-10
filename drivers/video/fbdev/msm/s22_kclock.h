/* CAT S22 Flip cover clock drawn by the kernel (see s22_kclock.c). */
#ifndef S22_KCLOCK_H
#define S22_KCLOCK_H

#include <linux/types.h>

struct spi_panel_data;

void s22_kclock_attach(struct spi_panel_data *ctrl);
void s22_kclock_compose(struct spi_panel_data *ctrl, u16 *frame);
void s22_kclock_lid(int open);
bool s22_kclock_display_off(struct spi_panel_data *ctrl);
void s22_kclock_display_off_done(int rc);
bool s22_kclock_display_on(struct spi_panel_data *ctrl);
void s22_kclock_display_on_done(int rc);
bool s22_kclock_swallow(struct spi_panel_data *ctrl, bool count);
int s22_kclock_android_view(struct spi_panel_data *ctrl);
void s22_kclock_spi_ready(void);

#endif
