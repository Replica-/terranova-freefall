/* config.c -- TN config-variable library (Terra Nova v1.09, 0x2E1DF0-0x2E3110; no source path in
 * the binary, names ours).  Decompiled from __FF.EXE.
 *
 * Variables live in one hash table (config_table, DSTRUCT hash, 0x8B-byte ConfigVar elements)
 * keyed by name (first 0x32 chars, case-insensitive; hash = first 4 chars upper-cased).  They
 * come from config files ("name value ;comment" lines, config_read_file), the command line
 * (config_parse_cmdline: "name=value", "+name", "-x value" switches) and the game
 * (config_set_value).  Each has a priority (a file's values get the read filter's result,
 * the command line -1, the game config_default_pri = 3) and the file it came from.
 * config_write_file rewrites a file in place through a temporary "_" + name[1:] copy.
 *
 *   0x2E1DF0 config_switch_tmpl (2-byte literal "X" in the code segment, not a function)
 *   0x2E1DF4 config_str_create      0x2E1E44 config_str_addref     0x2E1E48 config_str_release
 *   0x2E1E58 config_read_line       0x2E1E9C config_hash           0x2E1ED4 config_equ
 *   0x2E1EE4 config_set_string_pri  0x2E1FF8 config_parse_line     0x2E2084 config_init
 *   0x2E20C8 config_set_priority    0x2E2118 config_get_priority   0x2E2168 config_get_origin
 *   0x2E21E0 config_set_from_file   0x2E22FC config_read_file      0x2E2428 config_write_line
 *   0x2E254C config_write_iter      0x2E258C config_split_path     0x2E2604 config_origin_is
 *   0x2E2634 config_write_file      0x2E297C config_set_value      0x2E2A80 config_set_single_value
 *   0x2E2AE4 config_get_value       0x2E2C88 config_get_single_value
 *   0x2E2CB4 CfgLookup (config_get_raw)                            0x2E2D0C config_set_cmdline_seps
 *   0x2E2D14 config_parse_cmdline   0x2E2E40 config_unset          0x2E2EAC config_iter_nop
 *   0x2E2EB0 config_shutdown        0x2E2EF4 config_set_switch_table
 *   0x2E2EFC config_check_switch    0x2E2FB0 config_set_key_table  0x2E2FB8 config_check_key
 *   0x2E30BC config_is_defined_last 0x2E30D0 config_spew
 *
 * Original quirks kept:
 *  - hash_lookup always returns 0 (OK), so config_set_priority / config_get_priority /
 *    config_get_origin / config_write_file use the result pointer even when the variable is
 *    missing: they write or read NULL + 0x83 / 0x87 (low memory under DOS/4GW).
 *  - Values are strcpy'd into the 0x50-byte value field: longer values run over pri/origin
 *    (and over the caller's stack frame for a new variable).  Unused bytes of a new element
 *    (value after its NUL) are uninitialised stack copied into the table.
 *  - config_set_string_pri keeps the higher priority but overwrites the value even when the
 *    new priority is lower; config_set_from_file refuses (6) instead.
 *  - config_set_value / config_set_single_value with an unknown type (or count <= 0) store an
 *    uninitialised buffer.
 *  - config_get_value: a ',' is never skipped, so "1,2" yields "1", then empty items until the
 *    caller's count is used up; *count <= 0 returns TRUE with nothing read.
 *  - config_parse_cmdline: "-x" as the last argument sets x from NULL (reads address 0);
 *    strpbrk returning NULL (no separator) is dereferenced (address 0 holds 0 in practice).
 *  - config_read_file dereferences the origin string even if its allocation failed.
 *  - config_write_file: the temporary name is dir + "_" + file name without its first char;
 *    the name buffers are 0x50 (dir) and 0x10 (file) bytes; the hash copy it restores keeps
 *    origin pointers that config_unset released while rewriting.  A failed hash_copy is
 *    returned; refusing because the library is not initialised leaks the copy.
 *  - config_shutdown iterates with a callback that does nothing (origins are never freed).
 *  - config_check_switch / config_check_key: CfgLookup leaves the buffer untouched when the
 *    variable is missing (uninitialised compare).
 */
#include "t1_b.h"

#pragma GCC optimize("no-isolate-erroneous-paths-dereference", "no-delete-null-pointer-checks")

#ifndef TN_LINK_ORIGINAL_DATA
char config_switch_tmpl[2] = "X";
Hashtable config_table;
long config_init_count;
static char config_default_seps[] = "=:";
char *config_cmdline_seps = config_default_seps;
uchar config_write_ok;
ulong config_default_pri = 3;
ConfigSwitch *config_switch_table;
uchar config_switch_default;
ConfigKey *config_key_table;
char *config_last_var;
#endif

#define ISSPACE(c) (_IsTable[(uchar)((c) + 1)] & 2)
/* hide a pointer's value from the optimiser (NULL may reach a dereference on purpose) */
#define LAUNDER(p) __asm__("" : "+r"(p))

/* Watcom's inline strcpy / strcat */
static char *str_copy(char *d, const char *s)
{
    char *r = d;
    LAUNDER(d);
    LAUNDER(s);
    while ((*d++ = *s++) != 0)
        ;
    return r;
}
static void str_cat(char *d, const char *s)
{
    str_copy(d + strlen(d), s);
}

/* A new element: the value may run past the element (see above), keep it in our frame. */
typedef union {
    ConfigVar v;
    char pad[0x100];
} ConfigTmp;

/* ------------------------------------------------------------------ ref-counted strings */

ConfigStr *config_str_create(char *s)
{
    ConfigStr *r = Malloc(strlen(s) + 5);
    if (r != NULL) {
        str_copy(r->str, s);
        r->refs = 1;
    }
    return r;
}

void config_str_addref(ConfigStr *r)
{
    r->refs++;
}

void config_str_release(ConfigStr *r)
{
    LAUNDER(r);
    if (--r->refs == 0)
        Free(r);
}

/* ------------------------------------------------------------------ file lines */

/* Reads one line (up to '\n', which is dropped; '\r' is kept) into buf. */
char *config_read_line(int fd, char *buf, int size)
{
    char *end = buf + size - 1;
    char *p = buf;
    if (p < end) {
        while (eof(fd) == 0) {
            read(fd, p, 1);
            if (*p == '\n')
                break;
            p++;
            if (p >= end)
                break;
        }
    }
    *p = 0;
    return buf;
}

int config_hash(char *name)
{
    int h = 0;
    uint n;
    if (name == NULL)
        return 0;
    for (n = 0; *name;) {
        h = toupper(*name++) + (h << 8);
        if (++n >= 4)
            break;
    }
    return h;
}

int config_equ(char *a, char *b)
{
    return strnicmp(a, b, 0x32);
}

/* Sets var to val; the variable keeps the higher of its old and the new priority. */
int config_set_string_pri(char *var, char *val, ulong pri)
{
    ConfigTmp tmp;
    ConfigVar *v;
    if (var == NULL)
        return CONFIG_ERR_ARG;
    strncpy(tmp.v.name, var, CONFIG_NAME_LEN);
    tmp.v.name[CONFIG_NAME_LEN - 1] = 0;
    hash_lookup(&config_table, &tmp, (void **)&v);
    if (v == NULL) {
        str_copy(tmp.v.value, val);
        tmp.v.origin = NULL;
        tmp.v.pri = pri;
        return hash_set(&config_table, &tmp);
    }
    if (pri < v->pri)
        pri = v->pri;
    v->pri = pri;
    if (v->origin != NULL)
        config_str_release(v->origin);
    v->origin = NULL;
    str_copy(v->value, val);
    return 0;
}

/* Splits "name value ;comment" in place.  Leading blanks are skipped, the name ends at a blank,
 * the value is the rest (blanks after the name skipped).  NULL name for an empty line. */
void config_parse_line(char *line, char **var, char **val, char **comment)
{
    char *p, *start;
    *comment = NULL;
    *val = NULL;
    *var = NULL;
    p = line;
    if (*line) {
        for (;;) {
            if (*p == ';') {
                *p = 0;
                *comment = p + 1;
                break;
            }
            if (p[1] == 0)
                break;
            p++;
        }
    }
    p = line;
    while (ISSPACE(*p))
        p++;
    if (*p == 0)
        return;
    start = p;
    while (!ISSPACE(*p) && *p != 0)
        p++;
    if (*p != 0) {
        *p = 0;
        do
            p++;
        while (ISSPACE(*p));
    }
    *var = start;
    *val = p;
}

int config_init(void)
{
    long n = config_init_count++;
    if (n > 0)
        return CONFIG_ERR_NOEFFECT;
    return hash_init(&config_table, sizeof(ConfigVar), 10, (Hashfunc)WATCOM_FN(config_hash),
                     (Equfunc)WATCOM_FN(config_equ));
}

int config_set_priority(char *var, ulong pri)
{
    ConfigVar tmp, *v;
    int r;
    strncpy(tmp.name, var, 0x32);
    tmp.name[0x31] = 0;
    r = hash_lookup(&config_table, &tmp, (void **)&v);
    if (r == 0)
        v->pri = pri;               /* NULL + 0x83 when missing */
    return r;
}

ulong config_get_priority(char *var)
{
    ConfigVar tmp, *v;
    strncpy(tmp.name, var, 0x32);
    tmp.name[0x31] = 0;
    if (hash_lookup(&config_table, &tmp, (void **)&v) != 0)
        return 0;
    return v->pri;                  /* NULL + 0x83 when missing */
}

/* Copies the name of the file var came from into buf ("" if none). */
int config_get_origin(char *var, char *buf, int size)
{
    ConfigVar tmp, *v;
    int r;
    strncpy(tmp.name, var, 0x32);
    tmp.name[0x31] = 0;
    r = hash_lookup(&config_table, &tmp, (void **)&v);
    if (r != 0)
        return CONFIG_ERR_NOEFFECT;
    if (v->origin == NULL) {        /* NULL + 0x87 when missing */
        buf[0] = 0;
    } else {
        strncpy(buf, v->origin->str, size);
        buf[size - 1] = 0;
    }
    return r;
}

/* Sets var from a file line: refused (6) if the variable has a higher priority. */
int config_set_from_file(char *var, char *val, ulong pri, ConfigStr *origin)
{
    ConfigTmp tmp;
    ConfigVar *v;
    if (var == NULL)
        return CONFIG_ERR_ARG;
    strncpy(tmp.v.name, var, CONFIG_NAME_LEN);
    tmp.v.name[CONFIG_NAME_LEN - 1] = 0;
    hash_lookup(&config_table, &tmp, (void **)&v);
    LAUNDER(origin);
    if (v == NULL) {
        str_copy(tmp.v.value, val);
        tmp.v.pri = pri;
        tmp.v.origin = origin;
        origin->refs++;
        return hash_set(&config_table, &tmp);
    }
    if (pri < v->pri)
        return CONFIG_ERR_NOEFFECT;
    v->pri = pri;
    if (v->origin != NULL)
        config_str_release(v->origin);
    v->origin = origin;
    origin->refs++;
    str_copy(v->value, val);
    return 0;
}

/* Reads a config file.  filter(name) (Watcom callback, may be NULL) returns the priority to
 * give the variable, 0 to skip it; without a filter the priority is 1. */
int config_read_file(char *fname, ConfigReadFilter filter)
{
    char buf[0x100];
    char *comment, *val, *var;
    ConfigStr *origin;
    ulong pri;
    int fd;
    if (config_init_count <= 0)
        return CONFIG_ERR_ARG;
    fd = open(fname, O_RDONLY);
    if (fd == -1)
        return CONFIG_ERR_FILE;
    origin = config_str_create(fname);
    LAUNDER(origin);
    while (eof(fd) == 0) {
        config_read_line(fd, buf, sizeof(buf));
        config_parse_line(buf, &var, &val, &comment);
        if (filter != NULL)
            pri = filter(var);
        else
            pri = 1;
        if (pri != 0 && var != NULL)
            config_set_from_file(var, val, pri, origin);
    }
    close(fd);
    config_str_release(origin);
    return 0;
}

/* Writes "var val[;comment]\n"; a short write clears config_write_ok. */
void config_write_line(int fd, char *var, char *val, char *comment)
{
    char buf[0x100];
    int len;
    str_copy(buf, var);
    buf[strlen(var)] = ' ';
    str_copy(buf + strlen(var) + 1, val);
    if (comment != NULL) {
        len = strlen(buf);
        buf[len] = ';';
        str_copy(buf + len + 1, comment);
    }
    str_cat(buf, "\n");
    if (write(fd, buf, strlen(buf)) != strlen(buf))
        config_write_ok = 0;
}

/* hash_iter callback of config_write_file: writes the variables the file didn't have. */
bool config_write_iter(ConfigVar *v, ConfigWriteData *d)
{
    if (v->pri == 0 || v->pri == (ulong)-1)
        return FALSE;
    if (d->filter != NULL && !(bool)d->filter(d->fname, v))
        return FALSE;
    config_write_line(d->fd, v->name, v->value, NULL);
    return FALSE;
}

/* "dir/file" -> dir = "dir/", file = "file" (after the last '/', '\\' or ':'). */
void config_split_path(char *path, char *dir, char *file)
{
    char *p = path, *q = path;
    char c;
    if (*p) {
        for (;;) {
            c = *p;
            if (c == '/' || c == '\\' || c == ':')
                q = p + 1;
            if (p[1] == 0)
                break;
            p++;
        }
    }
    c = *q;
    *q = 0;
    str_copy(dir, path);
    *q = c;
    str_copy(file, q);
}

bool config_origin_is(char *fname, char *var)
{
    char buf[0x90];
    config_get_origin(var, buf, 0x8F);
    return strcmpi(buf, fname) == 0;
}

/* Rewrites fname: lines naming a saved variable (priority != 0) get its current value, other
 * lines are copied, then the remaining saved variables (priority not 0 or -1, accepted by
 * filter(fname, var) if given) are appended. */
int config_write_file(char *fname, ConfigWriteFilter filter)
{
    char orig[0x102], line[0x100], tmpname[0x90], dir[0x50], file[0x10];
    ConfigTmp key;
    Hashtable copy;
    ConfigWriteData d;
    ConfigVar *v;
    char *comment, *val, *var;
    int r, fdw, fdr;
    r = hash_copy(&copy, &config_table);
    if ((short)r != 0)
        return r;
    if (config_init_count <= 0)
        return CONFIG_ERR_ARG;
    config_split_path(fname, dir, file);
    str_copy(tmpname, dir);
    str_cat(tmpname, "_");
    str_cat(tmpname, file + 1);
    fdw = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, S_IWRITE);
    if (fdw == -1)
        return CONFIG_ERR_FILE;
    config_write_ok = 1;
    fdr = open(fname, O_RDONLY);
    if (fdr != -1) {
        while (eof(fdr) == 0) {
            config_read_line(fdr, line, sizeof(line));
            strncpy(orig, line, 0x100);
            config_parse_line(line, &var, &val, &comment);
            if (var != NULL) {
                strncpy(key.v.name, var, 0x32);
                key.v.name[0x31] = 0;
            } else {
                key.v.name[0] = 0;
            }
            r = hash_lookup(&config_table, &key, (void **)&v);
            if (var != NULL && r == 0 && v->pri != 0) {   /* NULL + 0x83 when missing */
                config_write_line(fdw, var, v->value, comment);
                config_unset(var);
            } else {
                str_cat(orig, "\n");
                write(fdw, orig, strlen(orig));
            }
        }
        close(fdr);
    }
    d.fd = fdw;
    d.fname = fname;
    d.filter = filter;
    hash_iter(&config_table, (HashIterFunc)WATCOM_FN(config_write_iter), &d);
    close(fdw);
    hash_destroy(&config_table);
    hash_copy(&config_table, &copy);
    hash_destroy(&copy);
    if (config_write_ok) {
        remove(fname);
        rename(tmpname, fname);
    } else {
        remove(tmpname);
    }
    if (config_write_ok)
        return 0;
    return CONFIG_ERR_FILE;
}

/* ------------------------------------------------------------------ values */

/* Sets var to count strings or ints joined by blanks, at config_default_pri. */
int config_set_value(char *var, int type, long *vals, int count)
{
    char buf[0x100];
    char *p = buf;
    int i;
    for (i = 0; i < count; i++) {
        if ((uint)type == CONFIG_STRING_TYPE)
            str_copy(p, (char *)vals[i]);
        else if (type == CONFIG_INT_TYPE)
            ltoa(vals[i], p, 10);
        if (i < count - 1) {
            p += strlen(p);
            *p++ = ' ';
        }
    }
    return config_set_string_pri(var, buf, config_default_pri);
}

int config_set_single_value(char *var, int type, long val)
{
    char buf[0x100];
    if ((uint)type == CONFIG_STRING_TYPE)
        str_copy(buf, (char *)val);
    else if (type == CONFIG_INT_TYPE)
        ltoa(val, buf, 10);
    return config_set_string_pri(var, buf, config_default_pri);
}

/* Reads up to *count items (blank or comma separated) of var into vals: malloc'd strings or
 * atoi values.  *count = items read.  FALSE if var is not defined. */
bool config_get_value(char *var, int type, long *vals, int *count)
{
    ConfigVar key, *v;
    int max;
    char *p, *q;
    char save;
    max = *count > 0 ? *count : -1;
    *count = 0;
    strncpy(key.name, var, CONFIG_NAME_LEN);
    hash_lookup(&config_table, &key, (void **)&v);
    if (v == NULL)
        return FALSE;
    p = v->value;
    while (ISSPACE(*p))
        p++;
    for (;;) {
        if (*p == 0 || max <= 0)
            return TRUE;
        q = p;
        while (!ISSPACE(*q) && *q != ',' && *q != 0)
            q++;
        save = *q;
        *q = 0;
        if ((uint)type == CONFIG_STRING_TYPE) {
            vals[*count] = (long)Malloc(strlen(p) + 1);
            str_copy((char *)vals[*count], p);
        } else if (type == CONFIG_INT_TYPE) {
            vals[*count] = atoi(p);
        }
        *q = save;
        (*count)++;
        if (max > 0)
            max--;
        p = q;
        while (ISSPACE(*p))
            p++;
    }
}

bool config_get_single_value(char *var, int type, long *val)
{
    int count = 1;
    if (config_get_value(var, type, val, &count) && count > 0)
        return TRUE;
    return FALSE;
}

/* config_get_raw: copies var's value text into buf (strncpy: not terminated if it fills buf). */
bool CfgLookup(char *var, char *buf, int size)
{
    ConfigVar key, *v;
    strncpy(key.name, var, CONFIG_NAME_LEN);
    hash_lookup(&config_table, &key, (void **)&v);
    if (v == NULL)
        return FALSE;
    strncpy(buf, v->value, size);
    return TRUE;
}

void config_set_cmdline_seps(char *seps)
{
    config_cmdline_seps = seps;
}

/* argv[1..]: with a filter, "-abc" / "/abc" switches: the first letter c with filter(c) TRUE
 * sets variable "c" to the rest of the argument or to the next argument.  "+name" sets name to
 * "".  "name=value" (separators config_cmdline_seps) sets name, "name=" unsets it.  All at
 * priority -1. */
int config_parse_cmdline(int argc, char **argv, ConfigSwitchFilter filter)
{
    int i;
    char *arg, *p, *q, *e, *val;
    char sw[2];
    char save;
    for (i = 1; i < argc; i++) {
        arg = argv[i];
        if (filter != NULL && (arg[0] == '/' || arg[0] == '-')) {
            p = arg + 1;
            if (*p == 0)
                continue;
            for (;;) {
                if ((bool)filter(*p)) {
                    *(short *)sw = *(short *)config_switch_tmpl;
                    sw[0] = *p++;
                    val = NULL;
                    if (*p != 0)
                        val = p;
                    else if (++i < argc)
                        val = argv[i];
                    config_set_string_pri(sw, val, (ulong)-1);
                    break;
                }
                if (p[1] == 0)
                    break;
                p++;
            }
        } else if (arg[0] == '+') {
            config_set_string_pri(arg + 1, "", (ulong)-1);
        } else {
            q = strpbrk(arg, config_cmdline_seps);
            LAUNDER(q);
            q = TN_LOWMEM(q);           /* NULL: read address 0 (DOS low memory, 0) */
            if (*q == 0)
                continue;
            e = arg;
            while (e < q && !ISSPACE(*e))
                e++;
            q++;
            save = *e;
            *e = 0;
            if (*q != 0)
                config_set_string_pri(arg, q, (ulong)-1);
            else
                config_unset(arg);
            *e = save;
        }
    }
    return 0;
}

int config_unset(char *var)
{
    ConfigVar key, *v;
    strncpy(key.name, var, CONFIG_NAME_LEN);
    hash_lookup(&config_table, &key, (void **)&v);
    if (v == NULL)
        return -1;
    if (v->origin != NULL)
        config_str_release(v->origin);
    return hash_delete(&config_table, &key);
}

bool config_iter_nop(void *elem, void *data)
{
    return FALSE;
}

int config_shutdown(void)
{
    if (--config_init_count > 0)
        return CONFIG_ERR_NOEFFECT;
    config_init_count = 0;
    hash_iter(&config_table, (HashIterFunc)WATCOM_FN(config_iter_nop), NULL);
    return hash_destroy(&config_table);
}

/* ------------------------------------------------------------------ switches and keys */

void config_set_switch_table(ConfigSwitch *t)
{
    config_switch_table = t;
}

/* "@SWITCH:x" sets x to 0 and gives 0; otherwise the switch table's value for name, else 0
 * if name's value is "@SWITCH:", else config_switch_default. */
uchar config_check_switch(int unused, char *name)
{
    char buf[0x100];
    ConfigSwitch *s = config_switch_table;
    if (strncmp("@SWITCH:", name, strlen("@SWITCH:")) == 0) {
        config_set_single_value(name + strlen("@SWITCH:"), CONFIG_INT_TYPE, 0);
        return 0;
    }
    for (; s != NULL && s->name != NULL; s++)
        if (strcmp(s->name, name) == 0)
            return s->value;
    CfgLookup(name, buf, sizeof(buf));
    if (strcmp(buf, "@SWITCH:") == 0)
        return 0;
    return config_switch_default;
}

void config_set_key_table(ConfigKey *t)
{
    config_key_table = t;
}

/* Runs the key table entries for key; TRUE when one has mode 2. */
bool config_check_key(char key)
{
    char buf[0x100];
    ConfigKey *k = config_key_table;
    bool found;
    if (k == NULL)
        return FALSE;
    for (; k->key != 0; k++) {
        if (k->key != key)
            continue;
        found = CfgLookup(k->var, NULL, 0);
        switch (k->mode) {
        case 0:
            if (!found) {
                config_set_single_value(k->var, CONFIG_STRING_TYPE, (long)"");
                config_set_priority(k->var, 0);
            }
            break;
        case 1:
            if (found) {
                memcpy(buf, "@SWITCH:", 9);
                str_cat(buf, k->var);
                config_set_single_value(buf, CONFIG_INT_TYPE, 0);
            }
            config_unset(k->var);
            break;
        case 2:
            return TRUE;
        }
    }
    return FALSE;
}

bool config_is_defined_last(char *var)
{
    config_last_var = var;
    return CfgLookup(var, NULL, 0);
}

/* Formats into a local buffer and drops it (debug output compiled out). */
int config_spew(char *fmt, ...)
{
    char buf[0x400];
    char *va[1];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    va[0] = ap;
    return lg_vsprintf(buf, fmt, va, 0);
}
