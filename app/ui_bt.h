#pragma once

/* Modal Bluetooth (scan BLE + pair/connect/forget + toggle console serie SPP).
 * Detache de ui.c. Toutes les statics restent file-local. */
void ui_bt_modal_open(void);
