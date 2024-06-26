#include <stdint.h>

#ifndef SPHERO_SCANNER_H
#define SPHERO_SCANNER_H

#ifdef __cplusplus
extern "C" {
#endif

extern uint64_t last_sphero_found;

/**
 * @brief Initalize bluetooth to scan for spheros
 */
int scanner_init(char* names[], int sphero_names_len);

/**
 * @brief Start scanning for spheros
 *
 * @returns 0 on success
 *          Otherwise, error code
 *
 * @note This function is async. You should poll for when all spheros have been found
 */
int scanner_start();

/**
 * @brief Stop scanning for spheros
 *
 * @returns 0 on success
 *          Otherwise, error code
 */
int scanner_stop();

/**
 * @brief Get the number of spheros found
 */
unsigned int scanner_get_sphero_count();

/**
 * @brief Get sphero with specified id
 *
 * @param[in] id The id of the sphero to get
 *
 * @return sphero_client The sphero client
 */
struct bt_sphero_client* scanner_get_sphero(uint8_t id);

/**
 * @brief Release sphero
 *
 * @param[in] sphero_client The sphero client to release
 */
void scanner_release_sphero(struct bt_sphero_client* sphero_client);

#ifdef __cplusplus
}
#endif
#endif // SPHERO_SCANNER_H