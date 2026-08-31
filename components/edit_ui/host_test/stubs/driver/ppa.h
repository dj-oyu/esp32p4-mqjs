#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct ppa_client_t *ppa_client_handle_t;
typedef enum { PPA_OPERATION_SRM, PPA_OPERATION_BLEND, PPA_OPERATION_FILL } ppa_operation_t;
typedef struct { ppa_operation_t oper_type; uint32_t max_pending_trans_num; int data_burst_length; } ppa_client_config_t;
esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *ret_client);
esp_err_t ppa_unregister_client(ppa_client_handle_t c);
