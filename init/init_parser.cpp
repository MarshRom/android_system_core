/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include "action.h"
#include "init_parser.h"
#include "log.h"
#include "parser.h"
#include "service.h"
#include "util.h"

#include <cutils/iosched_policy.h>
#include <cutils/list.h>
#include <android-base/stringprintf.h>


static list_declare(service_list);
static list_declare(action_list);
static list_declare(action_queue);

struct import {
    struct listnode list;
    const char *filename;
};

static void *parse_service(struct parse_state *state, int nargs, char **args, bool redefine);
static void parse_line_service(struct parse_state *state, int nargs, char **args);

static void *parse_action(struct parse_state *state, int nargs, char **args);
static void parse_line_action(struct parse_state *state, int nargs, char **args);

#define SECTION 0x01
#define COMMAND 0x02
#define OPTION  0x04

#include "keywords.h"

#define KEYWORD(symbol, flags, nargs, func) \
    [ K_##symbol ] = { #symbol, func, nargs + 1, flags, },

static struct {
    const char *name;
    int (*func)(int nargs, char **args);
    unsigned char nargs;
    unsigned char flags;
} keyword_info[KEYWORD_COUNT] = {
    [ K_UNKNOWN ] = { "unknown", 0, 0, 0 },
#include "keywords.h"
};
#undef KEYWORD

#define kw_is(kw, type) (keyword_info[kw].flags & (type))
#define kw_name(kw) (keyword_info[kw].name)
#define kw_func(kw) (keyword_info[kw].func)
#define kw_nargs(kw) (keyword_info[kw].nargs)

void dump_parser_state() {
    if (false) {
        struct listnode* node;
        list_for_each(node, &service_list) {
            service* svc = node_to_item(node, struct service, slist);
            INFO("service %s\n", svc->name);
            INFO("  class '%s'\n", svc->classname);
            INFO("  exec");
            for (int n = 0; n < svc->nargs; n++) {
                INFO(" '%s'", svc->args[n]);
            }
            INFO("\n");
            for (socketinfo* si = svc->sockets; si; si = si->next) {
                INFO("  socket %s %s 0%o\n", si->name, si->type, si->perm);
            }
        }

        list_for_each(node, &action_list) {
            action* act = node_to_item(node, struct action, alist);
            INFO("on ");
            char name_str[256] = "";
            build_triggers_string(name_str, sizeof(name_str), act);
            INFO("%s", name_str);
            INFO("\n");

            struct listnode* node2;
            list_for_each(node2, &act->commands) {
                command* cmd = node_to_item(node2, struct command, clist);
                INFO("  %p", cmd->func);
                for (int n = 0; n < cmd->nargs; n++) {
                    INFO(" %s", cmd->args[n]);
                }
                INFO("\n");
            }
            INFO("\n");
        }
    }
}

static int lookup_keyword(const char *s)
{
    switch (*s++) {
    case 'b':
        if (!strcmp(s, "ootchart_init")) return K_bootchart_init;
        break;
    case 'c':
        if (!strcmp(s, "opy")) return K_copy;
        if (!strcmp(s, "lass")) return K_class;
        if (!strcmp(s, "lass_start")) return K_class_start;
        if (!strcmp(s, "lass_stop")) return K_class_stop;
        if (!strcmp(s, "lass_reset")) return K_class_reset;
        if (!strcmp(s, "onsole")) return K_console;
        if (!strcmp(s, "hown")) return K_chown;
        if (!strcmp(s, "hmod")) return K_chmod;
        if (!strcmp(s, "ritical")) return K_critical;
        break;
    case 'd':
        if (!strcmp(s, "isabled")) return K_disabled;
        if (!strcmp(s, "omainname")) return K_domainname;
        break;
    case 'e':
        if (!strcmp(s, "nable")) return K_enable;
        if (!strcmp(s, "xec")) return K_exec;
        if (!strcmp(s, "xport")) return K_export;
        break;
    case 'g':
        if (!strcmp(s, "roup")) return K_group;
        break;
    case 'h':
        if (!strcmp(s, "ostname")) return K_hostname;
        break;
    case 'i':
        if (!strcmp(s, "oprio")) return K_ioprio;
        if (!strcmp(s, "fup")) return K_ifup;
        if (!strcmp(s, "nsmod")) return K_insmod;
        if (!strcmp(s, "mport")) return K_import;
        if (!strcmp(s, "nstallkey")) return K_installkey;
        break;
    case 'k':
        if (!strcmp(s, "eycodes")) return K_keycodes;
        break;
    case 'l':
        if (!strcmp(s, "oglevel")) return K_loglevel;
        if (!strcmp(s, "oad_persist_props")) return K_load_persist_props;
        if (!strcmp(s, "oad_system_props")) return K_load_system_props;
        break;
    case 'm':
        if (!strcmp(s, "kdir")) return K_mkdir;
        if (!strcmp(s, "ount_all")) return K_mount_all;
        if (!strcmp(s, "ount")) return K_mount;
        break;
    case 'o':
        if (!strcmp(s, "n")) return K_on;
        if (!strcmp(s, "neshot")) return K_oneshot;
        if (!strcmp(s, "nrestart")) return K_onrestart;
        break;
    case 'p':
        if (!strcmp(s, "owerctl")) return K_powerctl;
        break;
    case 'r':
        if (!strcmp(s, "estart")) return K_restart;
        if (!strcmp(s, "estorecon")) return K_restorecon;
        if (!strcmp(s, "estorecon_recursive")) return K_restorecon_recursive;
        if (!strcmp(s, "mdir")) return K_rmdir;
        if (!strcmp(s, "m")) return K_rm;
        break;
    case 's':
        if (!strcmp(s, "eclabel")) return K_seclabel;
        if (!strcmp(s, "ervice")) return K_service;
        if (!strcmp(s, "ervice_redefine")) return K_service_redefine;
        if (!strcmp(s, "etenv")) return K_setenv;
        if (!strcmp(s, "etprop")) return K_setprop;
        if (!strcmp(s, "etrlimit")) return K_setrlimit;
        if (!strcmp(s, "etusercryptopolicies")) return K_setusercryptopolicies;
        if (!strcmp(s, "ocket")) return K_socket;
        if (!strcmp(s, "tart")) return K_start;
        if (!strcmp(s, "top")) return K_stop;
        if (!strcmp(s, "wapon_all")) return K_swapon_all;
        if (!strcmp(s, "ymlink")) return K_symlink;
        if (!strcmp(s, "ysclktz")) return K_sysclktz;
        break;
    case 't':
        if (!strcmp(s, "rigger")) return K_trigger;
        break;
    case 'u':
        if (!strcmp(s, "ser")) return K_user;
        break;
    case 'v':
        if (!strcmp(s, "erity_load_state")) return K_verity_load_state;
        if (!strcmp(s, "erity_update_state")) return K_verity_update_state;
        break;
    case 'w':
        if (!strcmp(s, "rite")) return K_write;
        if (!strcmp(s, "ritepid")) return K_writepid;
        if (!strcmp(s, "ait")) return K_wait;
        break;
    }
    return K_UNKNOWN;
}

static void parse_line_no_op(struct parse_state*, int, char**) {
}

static int push_chars(char **dst, int *len, const char *chars, int cnt)
{
    if (cnt > *len)
        return -1;

    memcpy(*dst, chars, cnt);
    *dst += cnt;
    *len -= cnt;

    return 0;
}

int expand_props(char *dst, const char *src, int dst_size)
{
    char *dst_ptr = dst;
    const char *src_ptr = src;
    int ret = 0;
    int left = dst_size - 1;

    if (!src || !dst || dst_size == 0)
        return -1;

    /* - variables can either be $x.y or ${x.y}, in case they are only part
     *   of the string.
     * - will accept $$ as a literal $.
     * - no nested property expansion, i.e. ${foo.${bar}} is not supported,
     *   bad things will happen
     */
    while (*src_ptr && left > 0) {
        char *c;
        char prop[PROP_NAME_MAX + 1];
        char prop_val[PROP_VALUE_MAX];
        int prop_len = 0;
        int prop_val_len;

        c = strchr(src_ptr, '$');
        if (!c) {
            while (left-- > 0 && *src_ptr)
                *(dst_ptr++) = *(src_ptr++);
            break;
        }

        memset(prop, 0, sizeof(prop));

        ret = push_chars(&dst_ptr, &left, src_ptr, c - src_ptr);
        if (ret < 0)
            goto err_nospace;
        c++;

        if (*c == '$') {
            *(dst_ptr++) = *(c++);
            src_ptr = c;
            left--;
            continue;
        } else if (*c == '\0') {
            break;
        }

        if (*c == '{') {
            c++;
            while (*c && *c != '}' && prop_len < PROP_NAME_MAX)
                prop[prop_len++] = *(c++);
            if (*c != '}') {
                /* failed to find closing brace, abort. */
                if (prop_len == PROP_NAME_MAX)
                    ERROR("prop name too long during expansion of '%s'\n",
                          src);
                else if (*c == '\0')
                    ERROR("unexpected end of string in '%s', looking for }\n",
                          src);
                goto err;
            }
            prop[prop_len] = '\0';
            c++;
        } else if (*c) {
            while (*c && prop_len < PROP_NAME_MAX)
                prop[prop_len++] = *(c++);
            if (prop_len == PROP_NAME_MAX && *c != '\0') {
                ERROR("prop name too long in '%s'\n", src);
                goto err;
            }
            prop[prop_len] = '\0';
            ERROR("using deprecated syntax for specifying property '%s', use ${name} instead\n",
                  prop);
        }

        if (prop_len == 0) {
            ERROR("invalid zero-length prop name in '%s'\n", src);
            goto err;
        }

        prop_val_len = property_get(prop, prop_val);
        if (!prop_val_len) {
            ERROR("property '%s' doesn't exist while expanding '%s'\n",
                  prop, src);
            goto err;
        }

        ret = push_chars(&dst_ptr, &left, prop_val, prop_val_len);
        if (ret < 0)
            goto err_nospace;
        src_ptr = c;
        continue;
    }

    *dst_ptr = '\0';
    return 0;

Parser::Parser() {
}

Parser& Parser::GetInstance() {
    static Parser instance;
    return instance;
}

static void parse_new_section(struct parse_state *state, int kw,
                       int nargs, char **args)
{
    printf("[ %s %s ]\n", args[0],
           nargs > 1 ? args[1] : "");
    switch(kw) {
    case K_service:
    case K_service_redefine:
        state->context = parse_service(state, nargs, args, (kw == K_service_redefine));
        if (state->context) {
            state->parse_line = parse_line_service;
            return;
        }
        break;
    case K_on:
        state->context = parse_action(state, nargs, args);
        if (state->context) {
            state->parse_line = parse_line_action;
            return;
        }
        break;
    case K_import:
        parse_import(state, nargs, args);
        break;
    }
    state->parse_line = parse_line_no_op;
}

void Parser::AddSectionParser(const std::string& name,
                              std::unique_ptr<SectionParser> parser) {
    section_parsers_[name] = std::move(parser);
}

void Parser::ParseData(const std::string& filename, const std::string& data) {
    //TODO: Use a parser with const input and remove this copy
    std::vector<char> data_copy(data.begin(), data.end());
    data_copy.push_back('\0');

    parse_state state;
    state.filename = filename.c_str();
    state.line = 0;
    state.ptr = &data_copy[0];
    state.nexttoken = 0;

    SectionParser* section_parser = nullptr;
    std::vector<std::string> args;

    for (;;) {
        switch (next_token(&state)) {
        case T_EOF:
            if (section_parser) {
                section_parser->EndSection();
            }
            return;
        case T_NEWLINE:
            state.line++;
            if (args.empty()) {
                break;
            }
            if (section_parsers_.count(args[0])) {
                if (section_parser) {
                    section_parser->EndSection();
                }
                section_parser = section_parsers_[args[0]].get();
                std::string ret_err;
                if (!section_parser->ParseSection(args, &ret_err)) {
                    parse_error(&state, "%s\n", ret_err.c_str());
                    section_parser = nullptr;
                }
            } else if (section_parser) {
                std::string ret_err;
                if (!section_parser->ParseLineSection(args, state.filename,
                                                      state.line, &ret_err)) {
                    parse_error(&state, "%s\n", ret_err.c_str());
                }
            }
            args.clear();
            break;
        case T_TEXT:
            args.emplace_back(state.text);
            break;
        }
    }
}

bool Parser::ParseConfigFile(const std::string& path) {
    INFO("Parsing file %s...\n", path.c_str());
    Timer t;
    std::string data;
    if (!read_file(path.c_str(), &data)) {
        return false;
    }

    data.push_back('\n'); // TODO: fix parse_config.
    ParseData(path, data);
    for (const auto& sp : section_parsers_) {
        sp.second->EndFile(path);
    }

    // Turning this on and letting the INFO logging be discarded adds 0.2s to
    // Nexus 9 boot time, so it's disabled by default.
    if (false) DumpState();

    NOTICE("(Parsing %s took %.2fs.)\n", path.c_str(), t.duration());
    return true;
}

bool Parser::ParseConfigDir(const std::string& path) {
    INFO("Parsing directory %s...\n", path.c_str());
    std::unique_ptr<DIR, int(*)(DIR*)> config_dir(opendir(path.c_str()), closedir);
    if (!config_dir) {
        ERROR("Could not import directory '%s'\n", path.c_str());
        return false;
    }
    dirent* current_file;
    while ((current_file = readdir(config_dir.get()))) {
        std::string current_path =
            android::base::StringPrintf("%s/%s", path.c_str(), current_file->d_name);
        // Ignore directories and only process regular files.
        if (current_file->d_type == DT_REG) {
            if (!ParseConfigFile(current_path)) {
                ERROR("could not import file '%s'\n", current_path.c_str());
            }
        }
    }
    return true;
}

bool Parser::ParseConfig(const std::string& path) {
    if (is_dir(path.c_str())) {
        return ParseConfigDir(path);
    }
    return ParseConfigFile(path);
}

<<<<<<< HEAD
void queue_all_property_triggers()
{
    queue_property_triggers(NULL, NULL);
}

void queue_builtin_action(int (*func)(int nargs, char **args), const char *name)
{
    action* act = (action*) calloc(1, sizeof(*act));
    trigger* cur_trigger = (trigger*) calloc(1, sizeof(*cur_trigger));
    cur_trigger->name = name;
    list_init(&act->triggers);
    list_add_tail(&act->triggers, &cur_trigger->nlist);
    list_init(&act->commands);
    list_init(&act->qlist);

    command* cmd = (command*) calloc(1, sizeof(*cmd));
    cmd->func = func;
    cmd->args[0] = const_cast<char*>(name);
    cmd->nargs = 1;
    list_add_tail(&act->commands, &cmd->clist);

    list_add_tail(&action_list, &act->alist);
    action_add_queue_tail(act);
}

void action_add_queue_tail(struct action *act)
{
    if (list_empty(&act->qlist)) {
        list_add_tail(&action_queue, &act->qlist);
    }
}

struct action *action_remove_queue_head(void)
{
    if (list_empty(&action_queue)) {
        return 0;
    } else {
        struct listnode *node = list_head(&action_queue);
        struct action *act = node_to_item(node, struct action, qlist);
        list_remove(node);
        list_init(node);
        return act;
    }
}

int action_queue_empty()
{
    return list_empty(&action_queue);
}

service* make_exec_oneshot_service(int nargs, char** args) {
    // Parse the arguments: exec [SECLABEL [UID [GID]*] --] COMMAND ARGS...
    // SECLABEL can be a - to denote default
    int command_arg = 1;
    for (int i = 1; i < nargs; ++i) {
        if (strcmp(args[i], "--") == 0) {
            command_arg = i + 1;
            break;
        }
    }
    if (command_arg > 4 + NR_SVC_SUPP_GIDS) {
        ERROR("exec called with too many supplementary group ids\n");
        return NULL;
    }

    int argc = nargs - command_arg;
    char** argv = (args + command_arg);
    if (argc < 1) {
        ERROR("exec called without command\n");
        return NULL;
    }

    service* svc = (service*) calloc(1, sizeof(*svc) + sizeof(char*) * argc);
    if (svc == NULL) {
        ERROR("Couldn't allocate service for exec of '%s': %s", argv[0], strerror(errno));
        return NULL;
    }

    if ((command_arg > 2) && strcmp(args[1], "-")) {
        svc->seclabel = args[1];
    }
    if (command_arg > 3) {
        svc->uid = decode_uid(args[2]);
    }
    if (command_arg > 4) {
        svc->gid = decode_uid(args[3]);
        svc->nr_supp_gids = command_arg - 1 /* -- */ - 4 /* exec SECLABEL UID GID */;
        for (size_t i = 0; i < svc->nr_supp_gids; ++i) {
            svc->supp_gids[i] = decode_uid(args[4 + i]);
        }
    }

    static int exec_count; // Every service needs a unique name.
    char* name = NULL;
    asprintf(&name, "exec %d (%s)", exec_count++, argv[0]);
    if (name == NULL) {
        ERROR("Couldn't allocate name for exec service '%s'\n", argv[0]);
        free(svc);
        return NULL;
    }
    svc->name = name;
    svc->classname = "default";
    svc->flags = SVC_EXEC | SVC_ONESHOT;
    svc->nargs = argc;
    memcpy(svc->args, argv, sizeof(char*) * svc->nargs);
    svc->args[argc] = NULL;
    list_add_tail(&service_list, &svc->slist);
    return svc;
}

static void *parse_service(struct parse_state *state, int nargs, char **args, bool redefine)
{
    if (nargs < 3) {
        parse_error(state, "services must have a name and a program\n");
        return 0;
    }
    if (!valid_name(args[1])) {
        parse_error(state, "invalid service name '%s'\n", args[1]);
        return 0;
    }

    service* svc = (service*) service_find_by_name(args[1]);
    if (svc && !redefine) {
        parse_error(state, "ignored duplicate definition of service '%s'\n", args[1]);
        return 0;
    }

    nargs -= 2;

    if (!svc) {
        svc = (service*) calloc(1, sizeof(*svc) + sizeof(char*) * nargs);
        redefine = false;
    }

    if (!svc) {
        parse_error(state, "out of memory\n");
        return 0;
    }
    svc->name = strdup(args[1]);
    svc->classname = "default";
    memcpy(svc->args, args + 2, sizeof(char*) * nargs);
    trigger* cur_trigger = (trigger*) calloc(1, sizeof(*cur_trigger));
    svc->args[nargs] = 0;
    svc->nargs = nargs;
    list_init(&svc->onrestart.triggers);
    cur_trigger->name = "onrestart";
    list_add_tail(&svc->onrestart.triggers, &cur_trigger->nlist);
    list_init(&svc->onrestart.commands);
    if (!redefine)
        list_add_tail(&service_list, &svc->slist);
    return svc;
}

static void parse_line_service(struct parse_state *state, int nargs, char **args)
{
    struct service *svc = (service*) state->context;
    struct command *cmd;
    int i, kw, kw_nargs;

    if (nargs == 0) {
        return;
    }

    svc->ioprio_class = IoSchedClass_NONE;

    kw = lookup_keyword(args[0]);
    switch (kw) {
    case K_class:
        if (nargs != 2) {
            parse_error(state, "class option requires a classname\n");
        } else {
            svc->classname = args[1];
        }
        break;
    case K_console:
        svc->flags |= SVC_CONSOLE;
        break;
    case K_disabled:
        svc->flags |= SVC_DISABLED;
        svc->flags |= SVC_RC_DISABLED;
        break;
    case K_ioprio:
        if (nargs != 3) {
            parse_error(state, "ioprio optin usage: ioprio <rt|be|idle> <ioprio 0-7>\n");
        } else {
            svc->ioprio_pri = strtoul(args[2], 0, 8);

            if (svc->ioprio_pri < 0 || svc->ioprio_pri > 7) {
                parse_error(state, "priority value must be range 0 - 7\n");
                break;
            }

            if (!strcmp(args[1], "rt")) {
                svc->ioprio_class = IoSchedClass_RT;
            } else if (!strcmp(args[1], "be")) {
                svc->ioprio_class = IoSchedClass_BE;
            } else if (!strcmp(args[1], "idle")) {
                svc->ioprio_class = IoSchedClass_IDLE;
            } else {
                parse_error(state, "ioprio option usage: ioprio <rt|be|idle> <0-7>\n");
            }
        }
        break;
    case K_group:
        if (nargs < 2) {
            parse_error(state, "group option requires a group id\n");
        } else if (nargs > NR_SVC_SUPP_GIDS + 2) {
            parse_error(state, "group option accepts at most %d supp. groups\n",
                        NR_SVC_SUPP_GIDS);
        } else {
            int n;
            svc->gid = decode_uid(args[1]);
            for (n = 2; n < nargs; n++) {
                svc->supp_gids[n-2] = decode_uid(args[n]);
            }
            svc->nr_supp_gids = n - 2;
        }
        break;
    case K_keycodes:
        if (nargs < 2) {
            parse_error(state, "keycodes option requires atleast one keycode\n");
        } else {
            svc->keycodes = (int*) malloc((nargs - 1) * sizeof(svc->keycodes[0]));
            if (!svc->keycodes) {
                parse_error(state, "could not allocate keycodes\n");
            } else {
                svc->nkeycodes = nargs - 1;
                for (i = 1; i < nargs; i++) {
                    svc->keycodes[i - 1] = atoi(args[i]);
                }
            }
        }
        break;
    case K_oneshot:
        svc->flags |= SVC_ONESHOT;
        break;
    case K_onrestart:
        nargs--;
        args++;
        kw = lookup_keyword(args[0]);
        if (!kw_is(kw, COMMAND)) {
            parse_error(state, "invalid command '%s'\n", args[0]);
            break;
        }
        kw_nargs = kw_nargs(kw);
        if (nargs < kw_nargs) {
            parse_error(state, "%s requires %d %s\n", args[0], kw_nargs - 1,
                kw_nargs > 2 ? "arguments" : "argument");
            break;
        }

        cmd = (command*) malloc(sizeof(*cmd) + sizeof(char*) * nargs);
        cmd->func = kw_func(kw);
        cmd->nargs = nargs;
        memcpy(cmd->args, args, sizeof(char*) * nargs);
        list_add_tail(&svc->onrestart.commands, &cmd->clist);
        break;
    case K_critical:
        svc->flags |= SVC_CRITICAL;
        break;
    case K_setenv: { /* name value */
        if (nargs < 3) {
            parse_error(state, "setenv option requires name and value arguments\n");
            break;
        }
        svcenvinfo* ei = (svcenvinfo*) calloc(1, sizeof(*ei));
        if (!ei) {
            parse_error(state, "out of memory\n");
            break;
        }
        ei->name = args[1];
        ei->value = args[2];
        ei->next = svc->envvars;
        svc->envvars = ei;
        break;
    }
    case K_socket: {/* name type perm [ uid gid context ] */
        if (nargs < 4) {
            parse_error(state, "socket option requires name, type, perm arguments\n");
            break;
        }
        if (strcmp(args[2],"dgram") && strcmp(args[2],"stream")
                && strcmp(args[2],"seqpacket")) {
            parse_error(state, "socket type must be 'dgram', 'stream' or 'seqpacket'\n");
            break;
        }
        socketinfo* si = (socketinfo*) calloc(1, sizeof(*si));
        if (!si) {
            parse_error(state, "out of memory\n");
            break;
        }
        si->name = args[1];
        si->type = args[2];
        si->perm = strtoul(args[3], 0, 8);
        if (nargs > 4)
            si->uid = decode_uid(args[4]);
        if (nargs > 5)
            si->gid = decode_uid(args[5]);
        if (nargs > 6)
            si->socketcon = args[6];
        si->next = svc->sockets;
        svc->sockets = si;
        break;
    }
    case K_user:
        if (nargs != 2) {
            parse_error(state, "user option requires a user id\n");
        } else {
            svc->uid = decode_uid(args[1]);
        }
        break;
    case K_seclabel:
        if (nargs != 2) {
            parse_error(state, "seclabel option requires a label string\n");
        } else {
            svc->seclabel = args[1];
        }
        break;
    case K_writepid:
        if (nargs < 2) {
            parse_error(state, "writepid option requires at least one filename\n");
            break;
        }
        svc->writepid_files_ = new std::vector<std::string>;
        for (int i = 1; i < nargs; ++i) {
            svc->writepid_files_->push_back(args[i]);
        }
        break;

    default:
        parse_error(state, "invalid option '%s'\n", args[0]);
    }
}

static void *parse_action(struct parse_state *state, int nargs, char **args)
{
    struct trigger *cur_trigger;
    int i;
    if (nargs < 2) {
        parse_error(state, "actions must have a trigger\n");
        return 0;
    }

    action* act = (action*) calloc(1, sizeof(*act));
    list_init(&act->triggers);

    for (i = 1; i < nargs; i++) {
        if (!(i % 2)) {
            if (strcmp(args[i], "&&")) {
                struct listnode *node;
                struct listnode *node2;
                parse_error(state, "& is the only symbol allowed to concatenate actions\n");
                list_for_each_safe(node, node2, &act->triggers) {
                    struct trigger *trigger = node_to_item(node, struct trigger, nlist);
                    free(trigger);
                }
                free(act);
                return 0;
            } else
                continue;
        }
        cur_trigger = (trigger*) calloc(1, sizeof(*cur_trigger));
        cur_trigger->name = args[i];
        list_add_tail(&act->triggers, &cur_trigger->nlist);
    }

    list_init(&act->commands);
    list_init(&act->qlist);
    list_add_tail(&action_list, &act->alist);
        /* XXX add to hash */
    return act;
}

static void parse_line_action(struct parse_state* state, int nargs, char **args)
{
    struct action *act = (action*) state->context;
    int kw, n;

    if (nargs == 0) {
        return;
    }

    kw = lookup_keyword(args[0]);
    if (!kw_is(kw, COMMAND)) {
        parse_error(state, "invalid command '%s'\n", args[0]);
        return;
    }

    n = kw_nargs(kw);
    if (nargs < n) {
        parse_error(state, "%s requires %d %s\n", args[0], n - 1,
            n > 2 ? "arguments" : "argument");
        return;
    }
    command* cmd = (command*) malloc(sizeof(*cmd) + sizeof(char*) * nargs);
    cmd->func = kw_func(kw);
    cmd->line = state->line;
    cmd->filename = state->filename;
    cmd->nargs = nargs;
    memcpy(cmd->args, args, sizeof(char*) * nargs);
    list_add_tail(&act->commands, &cmd->clist);
}

void Parser::DumpState() const {
    ServiceManager::GetInstance().DumpState();
    ActionManager::GetInstance().DumpState();
}
