/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <libgen.h>

#include "keyd.h"
#include "ini.h"
#include "keys.h"
#include "error.h"
#include "string.h"
#include "unicode.h"

#define MAX_FILE_SZ 65536
#define MAX_LINE_LEN 256

#undef warn
#define warn(fmt, ...) fprintf(stderr, "\t\033[31;1mERROR:\033[0m "fmt"\n", ##__VA_ARGS__)

static struct {
	const char *name;
	uint8_t op;
	enum {
		ARG_EMPTY,

		ARG_MACRO,
		ARG_LAYER,
		ARG_LAYOUT,
		ARG_TIMEOUT,
		ARG_DESCRIPTOR,
	} args[MAX_DESCRIPTOR_ARGS];
} actions[] =  {
	{ "swap",	OP_SWAP,	{ ARG_LAYER } },
	{ "swap2",	OP_SWAP2,	{ ARG_LAYER, ARG_MACRO } },
	{ "clear",	OP_CLEAR,	{} },
	{ "oneshot",	OP_ONESHOT,	{ ARG_LAYER } },
	{ "toggle",	OP_TOGGLE,	{ ARG_LAYER } },
	{ "toggle2",	OP_TOGGLE2,	{ ARG_LAYER, ARG_MACRO } },
	{ "layer",	OP_LAYER,	{ ARG_LAYER } },
	{ "overload",	OP_OVERLOAD,	{ ARG_LAYER, ARG_DESCRIPTOR } },
	{ "timeout",	OP_TIMEOUT,	{ ARG_DESCRIPTOR, ARG_TIMEOUT, ARG_DESCRIPTOR } },
	{ "macro2",	OP_MACRO2,	{ ARG_TIMEOUT, ARG_TIMEOUT, ARG_MACRO } },
	{ "setlayout",	OP_LAYOUT,	{ ARG_LAYOUT } },
};


static const char *resolve_include_path(const char *path, const char *include_path)
{
	static char resolved_path[PATH_MAX];
	char tmp[PATH_MAX];
	const char *dir;

	if (strstr(include_path, "."))
		return NULL;

	strcpy(tmp, path);
	dir = dirname(tmp);
	snprintf(resolved_path, sizeof resolved_path, "%s/%s", dir, include_path);

	if (!access(resolved_path, F_OK))
		return resolved_path;

	snprintf(resolved_path, sizeof resolved_path, "/usr/share/keyd/%s", include_path);

	if (!access(resolved_path, F_OK))
		return resolved_path;

	return NULL;
}

static char *read_file(const char *path)
{
	const char include_prefix[] = "include ";

	static char buf[MAX_FILE_SZ];
	char line[MAX_LINE_LEN];
	int sz = 0;

	FILE *fh = fopen(path, "r");
	if (!fh) {
		err("failed to open %s", path);
		return NULL;
	}

	while (fgets(line, sizeof line, fh)) {
		int len = strlen(line);

		if (line[len-1] != '\n') {
			err("maximum line length exceed (%d)", MAX_LINE_LEN);
			goto fail;
		}

		if ((len+sz) > MAX_FILE_SZ) {
			err("maximum file size exceed (%d)", MAX_FILE_SZ);
			goto fail;
		}

		if (strstr(line, include_prefix) == line) {
			int fd;
			const char *resolved_path;
			char *include_path = line+sizeof(include_prefix)-1;

			line[len-1] = 0;

			resolved_path = resolve_include_path(path, include_path);

			if (!resolved_path) {
				warn("failed to resolve include path: %s", include_path);
				continue;
			}

			fd = open(resolved_path, O_RDONLY);

			if (fd < 0) {
				warn("failed to include %s", include_path);
				perror("open");
			} else {
				int n;
				while ((n = read(fd, buf+sz, sizeof(buf)-sz)) > 0)
					sz += n;
				close(fd);
			}
		} else {
			strcpy(buf+sz, line);
			sz += len;
		}
	}

	fclose(fh);
	return buf;

fail:
	fclose(fh);
	return NULL;
}


/* Return up to two keycodes associated with the given name. */
static uint8_t lookup_keycode(const char *name)
{
	size_t i;

	for (i = 0; i < 256; i++) {
		const struct keycode_table_ent *ent = &keycode_table[i];

		if (ent->name &&
		    (!strcmp(ent->name, name) ||
		     (ent->alt_name && !strcmp(ent->alt_name, name)))) {
			return i;
		}
	}

	return 0;
}

/*
 * Consumes a string of the form `[<layer>.]<key> = <descriptor>` and adds the
 * mapping to the corresponding layer in the config.
 */

static int set_layer_entry(const struct config *config, struct layer *layer,
	const char *key, const struct descriptor *d)
{
	size_t i;
	int found = 0;

	for (i = 0; i < 256; i++) {
		if (!strcmp(config->aliases[i], key)) {
			layer->keymap[i] = *d;
			found = 1;
		}
	}

	if (!found) {
		uint8_t code;

		if (!(code = lookup_keycode(key))) {
			err("%s is not a valid keycode or alias", key);
			return -1;
		}

		layer->keymap[code] = *d;

	}

	return 0;
}

static int new_layer(char *s, const struct config *config, struct layer *layer)
{
	uint8_t mods;
	char *name;
	char *type;

	name = strtok(s, ":");
	type = strtok(NULL, ":");

	strcpy(layer->name, name);

	if (strchr(name, '+')) {
		char *layername;
		int n = 0;

		layer->type = LT_COMPOSITE;
		layer->nr_constituents = 0;

		if (type) {
			err("composite layers cannot have a type.");
			return -1;
		}

		for (layername = strtok(name, "+"); layername; layername = strtok(NULL, "+")) {
			int idx = config_get_layer_index(config, layername);

			if (idx < 0) {
				err("%s is not a valid layer", layername);
				return -1;
			}

			if (n >= ARRAY_SIZE(layer->constituents)) {
				err("max composite layers (%d) exceeded", ARRAY_SIZE(layer->constituents));
				return -1;
			}

			layer->constituents[layer->nr_constituents++] = idx;
		}

	} else if (type && !strcmp(type, "layout")) {
			layer->type = LT_LAYOUT;
	} else if (type && !parse_modset(type, &mods)) {
			layer->type = LT_NORMAL;
			layer->mods = mods;
	} else {
		if (type)
			fprintf(stderr, "\tWARNING: \"%s\" is not a valid layer type, ignoring\n", type);

		layer->type = LT_NORMAL;
		layer->mods = 0;
	}


	return 0;
}

/*
 * Returns:
 * 	1 if the layer exists
 * 	0 if the layer was created successfully
 * 	< 0 on error
 */
static int config_add_layer(struct config *config, const char *s)
{
	int ret;
	char buf[MAX_LAYER_NAME_LEN];
	char *name;

	strcpy(buf, s);
	name = strtok(buf, ":");

	if (config_get_layer_index(config, name) != -1)
			return 1;

	if (config->nr_layers >= MAX_LAYERS) {
		err("max layers (%d) exceeded", MAX_LAYERS);
		return -1;
	}

	strcpy(buf, s);
	ret = new_layer(buf, config, &config->layers[config->nr_layers]);

	if (ret < 0)
		return -1;

	config->nr_layers++;
	return 0;
}

static void config_init(struct config *config)
{
	size_t i;

	memset(config, 0, sizeof *config);

	config_add_layer(config, "main");

	config_add_layer(config, "control:C");
	config_add_layer(config, "meta:M");
	config_add_layer(config, "shift:S");
	config_add_layer(config, "altgr:G");
	config_add_layer(config, "alt:A");

	/* Add default modifier bindings to the main layer. */
	for (i = 0; i < MAX_MOD; i++) {
		const struct modifier_table_ent *mod = &modifier_table[i];

		struct descriptor *d1 = &config->layers[0].keymap[mod->code1];
		struct descriptor *d2 = &config->layers[0].keymap[mod->code2];

		int idx = config_get_layer_index(config, mod->name);

		assert(idx != -1);

		d1->op = OP_LAYER;
		d1->args[0].idx = idx;

		d2->op = OP_LAYER;
		d2->args[0].idx = idx;

		strcpy(config->aliases[mod->code1], mod->name);
		strcpy(config->aliases[mod->code2], mod->name);
	}

	/* In ms */
	config->macro_timeout = 600;
	config->macro_repeat_timeout = 50;

}

/* Modifies the input string */
static int parse_fn(char *s,
		    char **name,
		    char *args[MAX_DESCRIPTOR_ARGS],
		    size_t *nargs)
{
	char *c, *arg;

	c = s;
	while (*c && *c != '(')
		c++;

	if (!*c)
		return -1;

	*name = s;
	*c++ = 0;

	while (*c == ' ')
		c++;

	*nargs = 0;
	arg = c;
	while (1) {
		int plvl = 0;

		while (*c) {
			switch (*c) {
			case '\\':
				if (*(c+1)) {
					c+=2;
					continue;
				}
				break;
			case '(':
				plvl++;
				break;
			case ')':
				plvl--;

				if (plvl == -1)
					goto exit;
				break;
			case ',':
				if (plvl == 0)
					goto exit;
				break;
			}

			c++;
		}
exit:

		if (!*c)
			return -1;

		if (arg != c) {
			assert(*nargs < MAX_DESCRIPTOR_ARGS);
			args[(*nargs)++] = arg;
		}

		if (*c == ')') {
			*c = 0;
			return 0;
		}

		*c++ = 0;
		while (*c == ' ')
			c++;
		arg = c;
	}
}

/*
 * Parses macro expression placing the result
 * in the supplied macro struct.
 *
 * Returns:
 *   0 on success
 *   -1 in the case of an invalid macro expression
 *   > 0 for all other errors
 */

int parse_macro_expression(const char *s, struct macro *macro)
{
	uint8_t code, mods;

	#define ADD_ENTRY(t, d) do { \
		if (macro->sz >= ARRAY_SIZE(macro->entries)) { \
			err("maxium macro size (%d) exceeded", ARRAY_SIZE(macro->entries)); \
			return 1; \
		} \
		macro->entries[macro->sz].type = t; \
		macro->entries[macro->sz].data = d; \
		macro->sz++; \
	} while(0)

	char *tok;
	size_t len = strlen(s);

	char buf[1024];
	char *ptr = buf;

	if (len >= sizeof(buf)) {
		err("macro size exceeded maximum size (%ld)\n", sizeof(buf));
		return -1;
	}

	strcpy(buf, s);

	if (!strncmp(ptr, "macro(", 6) && ptr[len-1] == ')') {
		ptr[len-1] = 0;
		ptr += 6;
	} else if (parse_key_sequence(ptr, &code, &mods) && utf8_strlen(ptr) != 1) {
		err("Invalid macro");
		return -1;
	}

	str_escape(ptr);

	macro->sz = 0;
	for (tok = strtok(ptr, " "); tok; tok = strtok(NULL, " ")) {
		size_t len = strlen(tok);

		if (!parse_key_sequence(tok, &code, &mods)) {
			ADD_ENTRY(MACRO_KEYSEQUENCE, (mods << 8) | code);
		} else if (strchr(tok, '+')) {
			char *saveptr;
			char *key;

			for (key = strtok_r(tok, "+", &saveptr); key; key = strtok_r(NULL, "+", &saveptr)) {
				size_t len = strlen(key);

				if (len > 1 && key[len-2] == 'm' && key[len-1] == 's')
					ADD_ENTRY(MACRO_TIMEOUT, atoi(key));
				else if (!parse_key_sequence(key, &code, &mods))
					ADD_ENTRY(MACRO_HOLD, code);
				else {
					err("%s is not a valid key", key);
					return -1;
				}
			}

			ADD_ENTRY(MACRO_RELEASE, 0);
		} else if (len > 1 && tok[len-2] == 'm' && tok[len-1] == 's') {
			ADD_ENTRY(MACRO_TIMEOUT, atoi(tok));
		} else {
			uint32_t codepoint;
			int chrsz;

			while ((chrsz=utf8_read_char(tok, &codepoint))) {
				int i;
				int xcode;

				if (chrsz == 1 && codepoint < 128) {
					for (i = 0; i < 256; i++) {
						const char *name = keycode_table[i].name;
						const char *shiftname = keycode_table[i].shifted_name;

						if (name && name[0] == tok[0] && name[1] == 0) {
							ADD_ENTRY(MACRO_KEYSEQUENCE, i);
							break;
						}

						if (shiftname && shiftname[0] == tok[0] && shiftname[1] == 0) {
							ADD_ENTRY(MACRO_KEYSEQUENCE, (MOD_SHIFT << 8) | i);
							break;
						}
					}
				} else if ((xcode = lookup_xcompose_index(codepoint)) > 0)
					ADD_ENTRY(MACRO_UNICODE, xcode);

				tok += chrsz;
			}
		}
	}

	return 0;

	#undef ADD_ENTRY
}

static int parse_command(const char *s, struct command *command)
{
	int len = strlen(s);

	if (len == 0 || strstr(s, "command(") != s || s[len-1] != ')')
		return -1;

	if (len > (int)sizeof(command->cmd)) {
		err("max command length (%ld) exceeded\n", sizeof(command->cmd));
		return 1;
	}

	strcpy(command->cmd, s+8);
	command->cmd[len-9] = 0;
	str_escape(command->cmd);

	return 0;
}

static int parse_descriptor(char *s,
			    struct descriptor *d,
			    struct config *config)
{
	char *fn = NULL;
	char *args[MAX_DESCRIPTOR_ARGS];
	size_t nargs = 0;
	uint8_t code, mods;
	int ret;
	struct macro macro;
	struct command cmd;

	if (!s || !s[0]) {
		d->op = 0;
		return 0;
	}

	if (!parse_key_sequence(s, &code, &mods)) {
		d->op = OP_KEYSEQUENCE;
		d->args[0].code = code;
		d->args[1].mods = mods;

		/* TODO: fixme. */
		if (keycode_to_mod(code))
			fprintf(stderr,
				"\t\033[31;1mNOTE:\033[0m mapping modifier keycodes directly may produce unintended results.\n");

		return 0;
	} else if ((ret=parse_command(s, &cmd)) >= 0) {
		if (ret) {
			return -1;
		}

		if (config->nr_commands >= ARRAY_SIZE(config->commands)) {
			err("max commands (%d), exceeded", ARRAY_SIZE(config->commands));
			return -1;
		}


		d->op = OP_COMMAND;
		d->args[0].idx = config->nr_commands;

		config->commands[config->nr_commands++] = cmd;

		return 0;
	} else if ((ret=parse_macro_expression(s, &macro)) >= 0) {
		if (ret)
			return -1;

		if (config->nr_macros >= ARRAY_SIZE(config->macros)) {
			err("max macros (%d), exceeded", ARRAY_SIZE(config->macros));
			return -1;
		}

		d->op = OP_MACRO;
		d->args[0].idx = config->nr_macros;

		config->macros[config->nr_macros++] = macro;

		return 0;
	} else if (!parse_fn(s, &fn, args, &nargs)) {
		int i;

		for (i = 0; i < ARRAY_SIZE(actions); i++) {
			if (!strcmp(actions[i].name, fn)) {
				int j;

				d->op = actions[i].op;

				for (j = 0; j < MAX_DESCRIPTOR_ARGS; j++) {
					if (!actions[i].args[j])
						break;
				}

				if ((int)nargs != j) {
					err("%s requires %d %s", actions[i].name, j, j == 1 ? "argument" : "arguments");
					return -1;
				}

				while (j--) {
					int type = actions[i].args[j];
					union descriptor_arg *arg = &d->args[j];
					char *argstr = args[j];
					struct descriptor desc;

					switch (type) {
					case ARG_LAYER:
						if (!strcmp(argstr, "main")) {
							err("the main layer cannot be toggled");
							return -1;
						}

						arg->idx = config_get_layer_index(config, argstr);
						if (arg->idx == -1 || config->layers[arg->idx].type == LT_LAYOUT) {
							err("%s is not a valid layer", argstr);
							return -1;
						}

						break;
					case ARG_LAYOUT:
						arg->idx = config_get_layer_index(config, argstr);
						if (arg->idx == -1 || config->layers[arg->idx].type != LT_LAYOUT) {
							err("%s is not a valid layout", argstr);
							return -1;
						}

						break;
					case ARG_DESCRIPTOR:
						if (parse_descriptor(argstr, &desc, config))
							return -1;

						if (config->nr_descriptors >= ARRAY_SIZE(config->descriptors)) {
							err("maximum descriptors exceeded");
							return -1;
						}

						config->descriptors[config->nr_descriptors] = desc;
						arg->idx = config->nr_descriptors++;
						break;
					case ARG_TIMEOUT:
						arg->timeout = atoi(argstr);
						break;
					case ARG_MACRO:
						if (config->nr_macros >= ARRAY_SIZE(config->macros)) {
							err("max macros (%d), exceeded", ARRAY_SIZE(config->macros));
							return -1;
						}

						if (parse_macro_expression(argstr, &config->macros[config->nr_macros])) {
							return -1;
						}

						arg->idx = config->nr_macros;
						config->nr_macros++;

						break;
					default:
						assert(0);
						break;
					}
				}

				return 0;
			}
		}
	}

	err("invalid key or action");
	return -1;
}

static void parse_global_section(struct config *config, struct ini_section *section)
{
	size_t i;

	for (i = 0; i < section->nr_entries;i++) {
		struct ini_entry *ent = &section->entries[i];

		if (!strcmp(ent->key, "macro_timeout"))
			config->macro_timeout = atoi(ent->val);
		else if (!strcmp(ent->key, "macro_sequence_timeout"))
			config->macro_sequence_timeout = atoi(ent->val);
		else if (!strcmp(ent->key, "default_layout"))
			snprintf(config->default_layout, sizeof config->default_layout,
				 "%s", ent->val);
		else if (!strcmp(ent->key, "macro_repeat_timeout"))
			config->macro_repeat_timeout = atoi(ent->val);
		else if (!strcmp(ent->key, "layer_indicator"))
			config->layer_indicator = atoi(ent->val);
		else
			warn("line %zd: %s is not a valid global option", ent->lnum, ent->key);
	}
}

static void parse_id_section(struct config *config, struct ini_section *section)
{
	size_t i;
	for (i = 0; i < section->nr_entries; i++) {
		uint16_t product, vendor;

		struct ini_entry *ent = &section->entries[i];
		const char *s = ent->key;

		if (!strcmp(s, "*")) {
			config->wildcard = 1;
		} else {
			if (sscanf(s, "-%hx:%hx", &vendor, &product) == 2) {
				assert(config->nr_excluded_ids < ARRAY_SIZE(config->excluded_ids));
				config->excluded_ids[config->nr_excluded_ids++] = (vendor << 16 | product);
			} else if (sscanf(s, "%hx:%hx", &vendor, &product) == 2) {
				assert(config->nr_ids < ARRAY_SIZE(config->ids));
				config->ids[config->nr_ids++] = (vendor << 16 | product);
			} 
			else {
				warn("%s is not a valid device id", s);
			}
		}
	}
}

static void parse_alias_section(struct config *config, struct ini_section *section)
{
	size_t i;

	for (i = 0; i < section->nr_entries; i++) {
		uint8_t code;
		struct ini_entry *ent = &section->entries[i];
		const char *name = ent->val;

		if ((code = lookup_keycode(ent->key))) {
			ssize_t len = strlen(name);

			if (len >= (ssize_t)sizeof(config->aliases[0])) {
				warn("%s exceeds the maximum alias length (%ld)", name, sizeof(config->aliases[0])-1);
			} else {
				uint8_t alias_code;

				if ((alias_code = lookup_keycode(name))) {
					struct descriptor *d = &config->layers[0].keymap[code];

					d->op = OP_KEYSEQUENCE;
					d->args[0].code = alias_code;
					d->args[1].mods = 0;
				}

				strcpy(config->aliases[code], name);
			}
		} else {
			warn("failed to define alias %s, %s is not a valid keycode", ent->key, name);
		}
	}
}

int config_parse(struct config *config, const char *path)
{
	size_t i;

	char *content;
	struct ini *ini;
	struct ini_section *section;

	config_init(config);

	if (!(content = read_file(path)))
		return -1;

	if (!(ini = ini_parse_string(content, NULL)))
		return -1;

	snprintf(config->path, sizeof(config->path), "%s", path);

	/* First pass: create all layers based on section headers.  */
	for (i = 0; i < ini->nr_sections; i++) {
		section = &ini->sections[i];

		if (!strcmp(section->name, "ids")) {
			parse_id_section(config, section);
		} else if (!strcmp(section->name, "aliases")) {
			parse_alias_section(config, section);
		} else if (!strcmp(section->name, "global")) {
			parse_global_section(config, section);
		} else {
			if (config_add_layer(config, section->name) < 0)
				warn("%s", errstr);
		}
	}

	/* Populate each layer. */
	for (i = 0; i < ini->nr_sections; i++) {
		size_t j;
		char *layername;
		section = &ini->sections[i];

		if (!strcmp(section->name, "ids") ||
		    !strcmp(section->name, "aliases") ||
		    !strcmp(section->name, "global"))
			continue;

		layername = strtok(section->name, ":");

		for (j = 0; j < section->nr_entries;j++) {
			char entry[MAX_EXP_LEN];
			struct ini_entry *ent = &section->entries[j];

			if (!ent->val) {
				warn("invalid binding on line %zd", ent->lnum);
				continue;
			}

			snprintf(entry, sizeof entry, "%s.%s = %s", layername, ent->key, ent->val);

			if (config_add_entry(config, entry) < 0)
				warn("line %zd: %s", ent->lnum, errstr);
		}
	}

	return 0;
}

int config_check_match(struct config *config, uint32_t id)
{
	size_t i;

	for (i = 0; i < config->nr_ids; i++) {
		if (config->ids[i] == id)
			return 2;
	}

	for (i = 0; i < config->nr_excluded_ids; i++)
		if (config->excluded_ids[i] == id) {
			return 0;
		}

	return config->wildcard ? 1 : 0;
}

int config_get_layer_index(const struct config *config, const char *name)
{
	size_t i;

	if (!name)
		return -1;

	for (i = 0; i < config->nr_layers; i++)
		if (!strcmp(config->layers[i].name, name))
			return i;

	return -1;
}

/* 
 * Adds a binding of the form [<layer>.]<key> = <descriptor expression>
 * to the given config.
 */
int config_add_entry(struct config *config, const char *exp)
{
	char *keyname, *descstr, *dot, *paren, *s;
	char *layername = "main";
	struct descriptor d;
	struct layer *layer;
	int idx;

	static char buf[MAX_EXP_LEN];

	if (strlen(exp) >= MAX_EXP_LEN) {
		err("%s exceeds maximum expression length (%d)", exp, MAX_EXP_LEN);
		return -1;
	}

	strcpy(buf, exp);
	s = buf;

	dot = strchr(s, '.');
	paren = strchr(s, '(');

	if (dot && (!paren || dot < paren)) {
		layername = s;
		*dot = 0;
		s = dot+1;
	}

	parse_kvp(s, &keyname, &descstr);
	idx = config_get_layer_index(config, layername);

	if (idx == -1) {
		err("%s is not a valid layer", layername);
		return -1;
	}

	layer = &config->layers[idx];

	if (parse_descriptor(descstr, &d, config) < 0)
		return -1;

	return set_layer_entry(config, layer, keyname, &d);
}

