/*
 * wyoming_config.h — Minimal INI config parser
 */

#ifndef WYOMING_CONFIG_H
#define WYOMING_CONFIG_H

typedef struct wyoming_config wyoming_config_t;

wyoming_config_t *wyoming_config_load(const char *path);

const char *wyoming_config_get(const wyoming_config_t *cfg,
                               const char *section, const char *key);

int wyoming_config_get_int(const wyoming_config_t *cfg,
                           const char *section, const char *key, int def);

int wyoming_config_get_bool(const wyoming_config_t *cfg,
                            const char *section, const char *key, int def);

void wyoming_config_free(wyoming_config_t *cfg);

#endif /* WYOMING_CONFIG_H */
