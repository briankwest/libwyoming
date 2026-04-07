/*
 * wyoming_config.c — Minimal INI config parser for Wyoming server
 *
 * Supports: [sections], key = value, ; and # comments, whitespace trimming.
 * No reload, no iteration, no duration parsing. Just load + get.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define WY_CFG_MAX_ENTRIES 128
#define WY_CFG_MAX_SECTION 64
#define WY_CFG_MAX_KEY     64
#define WY_CFG_MAX_VALUE   512

typedef struct {
	char section[WY_CFG_MAX_SECTION];
	char key[WY_CFG_MAX_KEY];
	char value[WY_CFG_MAX_VALUE];
} wy_cfg_entry_t;

typedef struct wyoming_config {
	wy_cfg_entry_t entries[WY_CFG_MAX_ENTRIES];
	int count;
} wyoming_config_t;

static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s)) s++;
	if (!*s) return s;
	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
	return s;
}

wyoming_config_t *wyoming_config_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return NULL;

	wyoming_config_t *cfg = calloc(1, sizeof(*cfg));
	if (!cfg) { fclose(f); return NULL; }

	char section[WY_CFG_MAX_SECTION] = "general";
	char line[1024];

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);

		/* Skip empty lines and comments */
		if (!*s || *s == ';' || *s == '#') continue;

		/* Section header */
		if (*s == '[') {
			char *end = strchr(s, ']');
			if (end) {
				*end = '\0';
				snprintf(section, sizeof(section), "%s", s + 1);
			}
			continue;
		}

		/* key = value */
		char *eq = strchr(s, '=');
		if (!eq) continue;

		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);

		/* Strip inline comment (but not inside quoted values) */
		if (val[0] != '"') {
			char *comment = strpbrk(val, ";#");
			if (comment) {
				*comment = '\0';
				/* Re-trim after removing comment */
				char *vend = val + strlen(val) - 1;
				while (vend > val && isspace((unsigned char)*vend))
					*vend-- = '\0';
			}
		}

		if (cfg->count >= WY_CFG_MAX_ENTRIES) continue;

		wy_cfg_entry_t *e = &cfg->entries[cfg->count];
		snprintf(e->section, sizeof(e->section), "%s", section);
		snprintf(e->key, sizeof(e->key), "%s", key);
		snprintf(e->value, sizeof(e->value), "%s", val);
		cfg->count++;
	}

	fclose(f);
	return cfg;
}

const char *wyoming_config_get(const wyoming_config_t *cfg,
                               const char *section, const char *key)
{
	if (!cfg || !section || !key) return NULL;
	for (int i = 0; i < cfg->count; i++) {
		if (strcmp(cfg->entries[i].section, section) == 0 &&
		    strcmp(cfg->entries[i].key, key) == 0)
			return cfg->entries[i].value;
	}
	return NULL;
}

int wyoming_config_get_int(const wyoming_config_t *cfg,
                           const char *section, const char *key, int def)
{
	const char *v = wyoming_config_get(cfg, section, key);
	return v ? atoi(v) : def;
}

int wyoming_config_get_bool(const wyoming_config_t *cfg,
                            const char *section, const char *key, int def)
{
	const char *v = wyoming_config_get(cfg, section, key);
	if (!v) return def;
	return (strcmp(v, "on") == 0 || strcmp(v, "yes") == 0 ||
	        strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
}

void wyoming_config_free(wyoming_config_t *cfg)
{
	free(cfg);
}
