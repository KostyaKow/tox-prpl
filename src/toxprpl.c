/*
 *  Copyright (c) 2013 Sergey 'Jin' Bostandzhyan <jin at mediatomb dot cc>
 *
 *  tox-prlp - libpurple protocol plugin or Tox (see http://tox.im)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This plugin is based on the Nullprpl mockup from Pidgin / Finch / libpurple
 *  which is disributed under GPL v2 or later.  See http://pidgin.im/
 */

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <glib.h>

#include <tox/Messenger.h>
#include <tox/network.h>

#define PURPLE_PLUGINS

#ifdef HAVE_CONFIG_H
#include "autoconfig.h"
#endif

#include <account.h>
#include <accountopt.h>
#include <blist.h>
#include <cmds.h>
#include <conversation.h>
#include <connection.h>
#include <debug.h>
#include <notify.h>
#include <privacy.h>
#include <prpl.h>
#include <roomlist.h>
#include <request.h>
#include <status.h>
#include <util.h>
#include <version.h>

#define _(msg) msg // might add gettext later

// TODO: these two things below show be added to a public header of the library
#define CLIENT_ID_SIZE crypto_box_PUBLICKEYBYTES
extern uint8_t self_public_key[crypto_box_PUBLICKEYBYTES];

#define TOXPRPL_ID "prpl-jin_eld-tox"
#define DEFAULT_SERVER_KEY "5CD7EB176C19A2FD840406CD56177BB8E75587BB366F7BB3004B19E3EDC04143"
#define DEFAULT_SERVER_PORT 33445
#define DEFAULT_SERVER_IP   "192.184.81.118"

// todo: allow user to specify a contact request message
#define DEFAULT_REQUEST_MESSAGE _("Please allow me to add you as a friend!")

static PurplePlugin *g_tox_protocol = NULL;
// tox does not allow to pass user data to callbacks, it also does not allow
// to run multiple instances of the library, so this whole thing is pretty
// unstable at this point
static PurpleConnection *g_tox_gc = NULL;
static int g_connected = 0;
static guint g_tox_messenger_timer = -1;
static guint g_tox_connection_timer = -1;
static int g_logged_in = 0;

typedef struct
{
    PurpleStatusPrimitive primitive;
    uint8_t tox_status;
    gchar *id;
    gchar *title;
} toxprpl_status;

typedef void (*GcFunc)(PurpleConnection *from,
        PurpleConnection *to,
        gpointer userdata);

typedef struct
{
    GcFunc fn;
    PurpleConnection *from;
    gpointer userdata;
} GcFuncData;

typedef struct
{
    int tox_friendlist_number;
} toxprpl_buddy_data;

#define TOXPRPL_MAX_STATUSES    4
#define TOXPRPL_STATUS_ONLINE     0
#define TOXPRPL_STATUS_AWAY       1
#define TOXPRPL_STATUS_BUSY       2
#define TOXPRPL_STATUS_OFFLINE    3

static toxprpl_status toxprpl_statuses[] =
{
    {
        PURPLE_STATUS_AVAILABLE, TOXPRPL_STATUS_ONLINE,
        "tox_online", _("Online")
    },
    {
        PURPLE_STATUS_AWAY, TOXPRPL_STATUS_ONLINE,
        "tox_away", _("Away")
    },
    {
        PURPLE_STATUS_UNAVAILABLE, TOXPRPL_STATUS_BUSY,
        "tox_busy", _("Busy")
    },
    {
        PURPLE_STATUS_OFFLINE, TOXPRPL_STATUS_OFFLINE,
        "tox_offline", _("Offline")
    }
};

/*
 * stores offline messages that haven't been delivered yet. maps username
 * (char *) to GList * of GOfflineMessages. initialized in toxprpl_init.
 */
GHashTable* goffline_messages = NULL;

typedef struct
{
    char *from;
    char *message;
    time_t mtime;
    PurpleMessageFlags flags;
} GOfflineMessage;

static void toxprpl_add_to_buddylist(char *buddy_key);
static void toxprpl_do_not_add_to_buddylist(char *buddy_key);
static void foreach_toxprpl_gc(GcFunc fn, PurpleConnection *from,
                               gpointer userdata);
static void discover_status(PurpleConnection *from, PurpleConnection *to,
        gpointer userdata);
static void toxprpl_query_buddy_status(gpointer data, gpointer user_data);

static unsigned char *toxprpl_tox_hex_string_to_id(const char *hex_string);

// stay independent from the lib
static int toxprpl_get_status_index(int fnum, USERSTATUS status)
{
    switch (status)
    {
        case USERSTATUS_AWAY:
            return TOXPRPL_STATUS_AWAY;
        case USERSTATUS_BUSY:
            return TOXPRPL_STATUS_BUSY;
        case USERSTATUS_NONE:
        case USERSTATUS_INVALID:
        default:
            if (fnum != -1)
            {
                if (m_friendstatus(fnum) == FRIEND_ONLINE)
                {
                    return TOXPRPL_STATUS_ONLINE;
                }
            }
    }
    return TOXPRPL_STATUS_OFFLINE;
}

/* tox helpers */
static gchar *toxprpl_tox_bin_id_to_string(uint8_t *bin_id)
{
    int i;
    gchar *string_id = g_malloc(CLIENT_ID_SIZE * 2 + 1);
    gchar *p = string_id;
    for (i = 0; i < CLIENT_ID_SIZE; i++)
    {
        sprintf(p, "%02x", bin_id[i] & 0xff);
        p = p + 2;
    }
    p[CLIENT_ID_SIZE * 2] = '\0';
    return string_id;
}


/* tox specific stuff */
static void on_friendstatus(int fnum, uint8_t status)
{
    if (status == FRIEND_ONLINE)
    {
        purple_debug_info("toxprpl", "Friend status change: %d\n", status);
        uint8_t client_id[CLIENT_ID_SIZE];
        if (getclient_id(fnum, client_id) < 0)
        {
            purple_debug_info("toxprpl", "Could not get id of friend #%d\n",
                              fnum);
            return;
        }

        gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
        PurpleAccount *account = purple_connection_get_account(g_tox_gc);
        purple_prpl_got_user_status(account, buddy_key,
            toxprpl_statuses[TOXPRPL_STATUS_ONLINE].id, NULL);
        g_free(buddy_key);
    }
}

static void on_request(uint8_t* public_key, uint8_t* data, uint16_t length)
{
    int i;
    gchar *dialog_message;

    if (g_tox_gc == NULL)
    {
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(public_key);
    if (buddy_key == NULL)
    {
        purple_notify_error(g_tox_gc, _("Error"),
                            _("Could not parse public key of a buddy request "),
                            NULL);
        return;
    }
    purple_debug_info("toxprpl", "Buddy request from %s: %s\n",
                      buddy_key, data);

    PurpleAccount *account = purple_connection_get_account(g_tox_gc);
    PurpleBuddy *buddy = purple_find_buddy(account, buddy_key);
    if (buddy != NULL)
    {
        purple_debug_info("toxprpl", "Buddy %s already in buddy list!\n");
        g_free(buddy_key);
        return;
    }

    dialog_message = g_strdup_printf("The user %s sendy you a friend request, "
                                    "do you want to add him?", buddy_key);

    gchar *request_msg = NULL;
    if (length > 0)
    {
        request_msg = g_strndup(data, length);
    }

    purple_request_yes_no(g_tox_gc, "New friend request", dialog_message,
                          request_msg,
                          PURPLE_DEFAULT_ACTION_NONE,
                          purple_connection_get_account(g_tox_gc), NULL,
                          NULL,
                          buddy_key, // data, will be freed elsewhere
                          G_CALLBACK(toxprpl_add_to_buddylist),
                          G_CALLBACK(toxprpl_do_not_add_to_buddylist));
    g_free(dialog_message);
    g_free(request_msg);
}

static void on_incoming_message(int friendnum, uint8_t* string, uint16_t length)
{
    purple_debug_info("toxprpl", "Message received!\n");
    if (g_tox_gc == NULL)
    {
        return;
    }

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    serv_got_im(g_tox_gc, buddy_key, string, PURPLE_MESSAGE_RECV, time(NULL));
    g_free(buddy_key);
}

static void on_nick_change(int friendnum, uint8_t* data, uint16_t length)
{
    purple_debug_info("toxprpl", "Nick change!\n");

    if (g_tox_gc == NULL)
    {
        return;
    }

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    PurpleAccount *account = purple_connection_get_account(g_tox_gc);
    PurpleBuddy *buddy = purple_find_buddy(account, buddy_key);
    if (buddy == NULL)
    {
        purple_debug_info("toxprpl", "Ignoring nick change because buddy %s was not found\n", buddy_key);
        g_free(buddy_key);
        return;
    }

    g_free(buddy_key);
    purple_blist_alias_buddy(buddy, data);
}

static void on_status_change(int friendnum, USERSTATUS userstatus)
{
    int i;
    purple_debug_info("toxprpl", "Status change: %d\n", userstatus);
    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    PurpleAccount *account = purple_connection_get_account(g_tox_gc);
    purple_debug_info("toxprpl", "Setting user status for user %s to %s\n",
        buddy_key, toxprpl_statuses[toxprpl_get_status_index(friendnum, userstatus)].id);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[toxprpl_get_status_index(friendnum, userstatus)].id,
        NULL);
    g_free(buddy_key);
}

static gboolean tox_messenger_loop(gpointer data)
{
    doMessenger();
    return TRUE;
}

static gboolean tox_connection_check(gpointer gc)
{
    if ((g_connected == 0) && DHT_isconnected())
    {
        g_connected = 1;
        purple_connection_update_progress(gc, _("Connected"),
                1,   /* which connection step this is */
                2);  /* total number of steps */
        purple_connection_set_state(gc, PURPLE_CONNECTED);
        purple_debug_info("toxprpl", "DHT connected!\n");

        char id[32*2 + 1] = {0};
        size_t i;

        for(i=0; i<32; i++)
        {
            char xx[3];
            snprintf(xx, sizeof(xx), "%02x",  self_public_key[i] & 0xff);
            strcat(id, xx);
        }
        purple_debug_info("toxprpl", "My ID: %s\n", id);

        // query status of all buddies
        PurpleAccount *account = purple_connection_get_account(gc);
        GSList *buddy_list = purple_find_buddies(account, NULL);
        g_slist_foreach(buddy_list, toxprpl_query_buddy_status, gc);
        g_slist_free(buddy_list);
    }
    else if ((g_connected == 1) && !DHT_isconnected())
    {
        g_connected = 0;
        purple_debug_info("toxprpl", "DHT not connected!\n");
        purple_connection_update_progress(gc, _("Connecting"),
                0,   /* which connection step this is */
                2);  /* total number of steps */
    }
    return TRUE;
}
/*
 * helpers
 */
static PurpleConnection *get_toxprpl_gc(const char *username)
{
    PurpleAccount *acct = purple_accounts_find(username, TOXPRPL_ID);
    if (acct && purple_account_is_connected(acct))
        return acct->gc;
    else
        return NULL;
}

static void call_if_toxprpl(gpointer data, gpointer userdata)
{
    PurpleConnection *gc = (PurpleConnection *)(data);
    GcFuncData *gcfdata = (GcFuncData *)userdata;

    if (!strcmp(gc->account->protocol_id, TOXPRPL_ID))
        gcfdata->fn(gcfdata->from, gc, gcfdata->userdata);
}

static void foreach_toxprpl_gc(GcFunc fn, PurpleConnection *from,
        gpointer userdata)
{
    GcFuncData gcfdata = { fn, from, userdata };
    g_list_foreach(purple_connections_get_all(), call_if_toxprpl,
            &gcfdata);
}


typedef void(*ChatFunc)(PurpleConvChat *from, PurpleConvChat *to,
        int id, const char *room, gpointer userdata);

typedef struct
{
    ChatFunc fn;
    PurpleConvChat *from_chat;
    gpointer userdata;
} ChatFuncData;

static void call_chat_func(gpointer data, gpointer userdata)
{
    PurpleConnection *to = (PurpleConnection *)data;
    ChatFuncData *cfdata = (ChatFuncData *)userdata;

    int id = cfdata->from_chat->id;
    PurpleConversation *conv = purple_find_chat(to, id);
    if (conv)
    {
        PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
        cfdata->fn(cfdata->from_chat, chat, id, conv->name, cfdata->userdata);
    }
}

static void foreach_gc_in_chat(ChatFunc fn, PurpleConnection *from,
        int id, gpointer userdata)
{
    PurpleConversation *conv = purple_find_chat(from, id);
    ChatFuncData cfdata = { fn,
        purple_conversation_get_chat_data(conv),
        userdata };

    g_list_foreach(purple_connections_get_all(), call_chat_func,
            &cfdata);
}

// TODO: implement the tox parts
static void discover_status(PurpleConnection *from, PurpleConnection *to,
        gpointer userdata) {
    const char *from_username = from->account->username;
    const char *to_username = to->account->username;

    purple_debug_info("toxprpl", "discover status from %s to %s\n",
            from_username, to_username);
    if (purple_find_buddy(from->account, to_username))
    {
        PurpleStatus *status = purple_account_get_active_status(to->account);
        const char *status_id = purple_status_get_id(status);
        const char *message = purple_status_get_attr_string(status, "message");

        purple_debug_info("toxprpl", "discover status: status id %s\n",
                status_id);
        if (!strcmp(status_id, toxprpl_statuses[TOXPRPL_STATUS_ONLINE].id) ||
                !strcmp(status_id, toxprpl_statuses[TOXPRPL_STATUS_AWAY].id) ||
                !strcmp(status_id, toxprpl_statuses[TOXPRPL_STATUS_BUSY].id) ||
                !strcmp(status_id, toxprpl_statuses[TOXPRPL_STATUS_OFFLINE].id))
        {
            purple_debug_info("toxprpl", "%s sees that %s is %s: %s\n",
                    from_username, to_username, status_id, message);
            purple_prpl_got_user_status(from->account, to_username, status_id,
                    (message) ? "message" : NULL, message, NULL);
        }
        else
        {
            purple_debug_error("toxprpl",
                    "%s's buddy %s has an unknown status: %s, %s",
                    from_username, to_username, status_id, message);
        }
    }
}

// query buddy status
static void toxprpl_query_buddy_status(gpointer data, gpointer user_data)
{
    purple_debug_info("toxprpl", "toxprpl_query_buddy_status\n");
    PurpleBuddy *buddy = (PurpleBuddy *)data;
    PurpleConnection *gc = (PurpleConnection *)user_data;
    unsigned char *bin_key = toxprpl_tox_hex_string_to_id(buddy->name);
    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data == NULL)
    {
        int fnum = getfriend_id(bin_key);
        buddy_data = g_new0(toxprpl_buddy_data, 1);
        buddy_data->tox_friendlist_number = fnum;
        purple_buddy_set_protocol_data(buddy, buddy_data);
    }

    PurpleAccount *account = purple_connection_get_account(gc);
    purple_debug_info("toxprpl", "Setting user status for user %s to %s\n",
        buddy->name, toxprpl_statuses[toxprpl_get_status_index(
            buddy_data->tox_friendlist_number,
            m_get_userstatus(buddy_data->tox_friendlist_number))].id);
    purple_prpl_got_user_status(account, buddy->name,
        toxprpl_statuses[toxprpl_get_status_index(
            buddy_data->tox_friendlist_number,
            m_get_userstatus(buddy_data->tox_friendlist_number))].id,
        NULL);
    g_free(bin_key);
}

static void report_status_change(PurpleConnection *from, PurpleConnection *to,
        gpointer userdata)
{
    purple_debug_info("toxprpl", "notifying %s that %s changed status\n",
            to->account->username, from->account->username);
    discover_status(to, from, NULL);
}


/*
 * UI callbacks
 */
static void toxprpl_input_user_info(PurplePluginAction *action)
{
    PurpleConnection *gc = (PurpleConnection *)action->context;
    PurpleAccount *acct = purple_connection_get_account(gc);
    purple_debug_info("toxprpl", "showing 'Set User Info' dialog for %s\n",
            acct->username);

    purple_account_request_change_user_info(acct);
}

/* this is set to the actions member of the PurplePluginInfo struct at the
 * bottom.
 */
static GList *toxprpl_actions(PurplePlugin *plugin, gpointer context)
{
    PurplePluginAction *action = purple_plugin_action_new(
            _("Set User Info..."), toxprpl_input_user_info);
    return g_list_append(NULL, action);
}


static const char *toxprpl_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
    return "null";
}

static GList *toxprpl_status_types(PurpleAccount *acct)
{
    GList *types = NULL;
    PurpleStatusType *type;
    int i;

    purple_debug_info("toxprpl", "setting up status types\n");

    for (i = 0; i < TOXPRPL_MAX_STATUSES; i++)
    {
        type = purple_status_type_new_with_attrs(toxprpl_statuses[i].primitive,
            toxprpl_statuses[i].id, toxprpl_statuses[i].title, TRUE, TRUE,
            FALSE,
            "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
            NULL);
        types = g_list_append(types, type);
    }

    return types;
}

static unsigned char *toxprpl_tox_hex_string_to_id(const char *hex_string)
{
    int i;
    size_t len = strlen(hex_string);
    unsigned char *bin = malloc(len);
    if (bin == NULL)
    {
        return NULL;
    }
    const char *p = hex_string;
    for (i = 0; i < len; i++)
    {
        sscanf(p, "%2hhx", &bin[i]);
        p = p + 2;
    }
    return bin;
}

static void toxprpl_login(PurpleAccount *acct)
{
    IP_Port dht;

    purple_debug_info("toxprpl", "logging in %d\n", g_logged_in);
    if (g_logged_in)
    {
        return;
    }

    g_logged_in = 1;

    PurpleConnection *gc = purple_account_get_connection(acct);
    g_tox_gc = gc;

    purple_debug_info("toxprpl", "logging in %s\n", acct->username);

    purple_connection_update_progress(gc, _("Connecting"),
            0,   /* which connection step this is */
            2);  /* total number of steps */

    const char* ip = purple_account_get_string(acct, "dht_server_ip",
                                               DEFAULT_SERVER_IP);
    dht.port = htons(
            purple_account_get_int(acct, "dht_server_port",
                                   DEFAULT_SERVER_PORT));
    const char *key = purple_account_get_string(acct, "dht_server_key",
                                          DEFAULT_SERVER_KEY);
    uint32_t resolved = resolve_addr(ip);
    dht.ip.i = resolved;
    unsigned char *bin_str = toxprpl_tox_hex_string_to_id(key);
    DHT_bootstrap(dht, bin_str);
    free(bin_str);
    purple_debug_info("toxprpl", "Will connect to %s:%d (%s)\n" ,
                      ip, DEFAULT_SERVER_PORT, key);
    g_tox_messenger_timer = purple_timeout_add(100, tox_messenger_loop, NULL);
    purple_debug_info("toxprpl", "added messenger timer as %d\n", g_tox_messenger_timer);
    g_tox_connection_timer = purple_timeout_add_seconds(2, tox_connection_check,
                                                        gc);
}

static void toxprpl_close(PurpleConnection *gc)
{
    /* notify other toxprpl accounts */
    purple_debug_info("toxprpl", "Closing!\n");
    foreach_toxprpl_gc(report_status_change, gc, NULL);
}

static int toxprpl_send_im(PurpleConnection *gc, const char *who,
        const char *message, PurpleMessageFlags flags)
{
    const char *from_username = gc->account->username;
    PurpleMessageFlags receive_flags = ((flags & ~PURPLE_MESSAGE_SEND)
            | PURPLE_MESSAGE_RECV);
    PurpleAccount *to_acct = purple_accounts_find(who, TOXPRPL_ID);
    PurpleConnection *to;

    purple_debug_info("toxprpl", "sending message from %s to %s: %s\n",
            from_username, who, message);


    PurpleAccount *account = purple_connection_get_account(gc);
    PurpleBuddy *buddy = purple_find_buddy(account, who);
    if (buddy == NULL)
    {
        purple_debug_info("toxprpl", "Can't send message because buddy %s was not found\n", who);
        return 0;
    }
    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data == NULL)
    {
         purple_debug_info("toxprpl", "Can't send message because tox friend number is unknown\n");
        return 0;
    }

    m_sendmessage(buddy_data->tox_friendlist_number, (uint8_t *)message,
                  strlen(message)+1);
    return 1;
}

static int toxprpl_tox_addfriend(const char *buddy_key)
{
    unsigned char *bin_key = toxprpl_tox_hex_string_to_id(buddy_key);
    if (!bin_key)
    {
        purple_debug_info("toxprpl", "Can not allocate memory for key\n");
        return -1;
    }
    int ret = m_addfriend(bin_key, DEFAULT_REQUEST_MESSAGE,
                                   strlen(DEFAULT_REQUEST_MESSAGE) + 1);
    free(bin_key);
    const char *msg;
    switch (ret)
    {
        case -1:
            msg = "Message too long";
            break;
        case -2:
            msg = "Missing request message";
            break;
        case -3:
            msg = "You're trying to add yourself as a friend";
            break;
        case -4:
            msg = "Friend request already sent";
            break;
        case -5:
            msg = "Error adding friend";
            break;
        default:
            purple_debug_info("toxprpl", "Friend %s added\n", buddy_key);
            break;
    }

    if (ret < 0)
    {
        purple_notify_error(g_tox_gc, _("Error"), msg, NULL);
    }
    return ret;
}

static void toxprpl_do_not_add_to_buddylist(char *buddy_key)
{
    g_free(buddy_key);
}

static void toxprpl_add_to_buddylist(char *buddy_key)
{
    if (g_tox_gc == NULL)
    {
        purple_debug_info("toxprpl", "Can't add buddy %s invalid connection\n",
                          buddy_key);
        return;
    }

    int ret = toxprpl_tox_addfriend(buddy_key);
    if (ret < 0)
    {
        g_free(buddy_key);
        // error dialogs handled in toxprpl_tox_addfriend()
        return;
    }

    PurpleAccount *account = purple_connection_get_account(g_tox_gc);

    uint8_t alias[MAX_NAME_LENGTH];

    PurpleBuddy *buddy;
    if ((getname(ret, alias) == 0) && (strlen(alias) > 0))
    {
        purple_debug_info("toxprpl", "Got friend alias %s\n", alias);
        buddy = purple_buddy_new(account, buddy_key, alias);
    }
    else
    {
        purple_debug_info("toxprpl", "Adding [%s]\n", buddy_key);
        buddy = purple_buddy_new(account, buddy_key, NULL);
    }

    toxprpl_buddy_data *buddy_data = g_new0(toxprpl_buddy_data, 1);
    buddy_data->tox_friendlist_number = ret;
    purple_buddy_set_protocol_data(buddy, buddy_data);
    purple_blist_add_buddy(buddy, NULL, NULL, NULL);
    USERSTATUS userstatus = m_get_userstatus(ret);
    purple_debug_info("toxprpl", "Friend %s has status %d\n",
            buddy_key, userstatus);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[toxprpl_get_status_index(ret, userstatus)].id, NULL);

    g_free(buddy_key);
}

static void toxprpl_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
        PurpleGroup *group, const char *msg)
{
    purple_debug_info("toxprpl", "adding %s to buddy list\n", buddy->name);

    PurpleAccount *account = purple_connection_get_account(gc);

    if (purple_find_buddy(account, buddy->name) != NULL) {
//        purple_blist_remove_buddy(buddy);
        return;
    }

    int ret = toxprpl_tox_addfriend(buddy->name);
    if (ret < 0)
    {
        purple_blist_remove_buddy(buddy);
    }
    toxprpl_buddy_data *buddy_data = g_new0(toxprpl_buddy_data, 1);
    buddy_data->tox_friendlist_number = ret;
    purple_buddy_set_protocol_data(buddy, buddy_data);
}

static void toxprpl_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
        PurpleGroup *group)
{
    purple_debug_info("toxprpl", "removing buddy %s\n", buddy->name);
    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data != NULL)
    {
        purple_debug_info("toxprpl", "removing tox friend #%d\n", buddy_data->tox_friendlist_number);
        m_delfriend(buddy_data->tox_friendlist_number);
    }
}

static void toxprpl_free_buddy(PurpleBuddy *buddy)
{
    if (buddy->proto_data) {
        toxprpl_buddy_data *buddy_data = buddy->proto_data;
        g_free(buddy_data);
    }
}

static gboolean toxprpl_can_receive_file(PurpleConnection *gc,
        const char *who)
{
    return FALSE;
}

static gboolean toxprpl_offline_message(const PurpleBuddy *buddy)
{
    return FALSE;
}


static PurplePluginProtocolInfo prpl_info =
{
    OPT_PROTO_NO_PASSWORD | OPT_PROTO_REGISTER_NOSCREENNAME,  /* options */
    NULL,               /* user_splits, initialized in toxprpl_init() */
    NULL,               /* protocol_options, initialized in toxprpl_init() */
    {   /* icon_spec, a PurpleBuddyIconSpec */
        "png,jpg,gif",                   /* format */
        0,                               /* min_width */
        0,                               /* min_height */
        128,                             /* max_width */
        128,                             /* max_height */
        10000,                           /* max_filesize */
        PURPLE_ICON_SCALE_DISPLAY,       /* scale_rules */
    },
    toxprpl_list_icon,                   /* list_icon */
    NULL,                                      /* list_emblem */
    NULL,                                      /* status_text */
    NULL,                                      /* tooltip_text */
    toxprpl_status_types,               /* status_types */
    NULL,                                      /* blist_node_menu */
    NULL,                                      /* chat_info */
    NULL,                                      /* chat_info_defaults */
    toxprpl_login,                      /* login */
    toxprpl_close,                      /* close */
    toxprpl_send_im,                    /* send_im */
    NULL,                                      /* set_info */
    NULL,                                      /* send_typing */
    NULL,                                      /* get_info */
    NULL,                                      /* set_status */
    NULL,                                      /* set_idle */
    NULL,                                      /* change_passwd */
    NULL,                                      /* add_buddy */
    NULL,                                      /* add_buddies */
    toxprpl_remove_buddy,               /* remove_buddy */
    NULL,                                      /* remove_buddies */
    NULL,                                      /* add_permit */
    NULL,                                      /* add_deny */
    NULL,                                      /* rem_permit */
    NULL,                                      /* rem_deny */
    NULL,                                      /* set_permit_deny */
    NULL,                                      /* join_chat */
    NULL,                                      /* reject_chat */
    NULL,                                      /* get_chat_name */
    NULL,                                      /* chat_invite */
    NULL,                                      /* chat_leave */
    NULL,                                      /* chat_whisper */
    NULL,                                      /* chat_send */
    NULL,                                      /* keepalive */
    NULL,                                      /* register_user */
    NULL,                                      /* get_cb_info */
    NULL,                                      /* get_cb_away */
    NULL,                                      /* alias_buddy */
    NULL,                                      /* group_buddy */
    NULL,                                      /* rename_group */
    toxprpl_free_buddy,                  /* buddy_free */
    NULL,                                      /* convo_closed */
    NULL,                                      /* normalize */
    NULL,                                      /* set_buddy_icon */
    NULL,                                      /* remove_group */
    NULL,                                      /* get_cb_real_name */
    NULL,                                      /* set_chat_topic */
    NULL,                                      /* find_blist_chat */
    NULL,                                      /* roomlist_get_list */
    NULL,                                      /* roomlist_cancel */
    NULL,                                      /* roomlist_expand_category */
    toxprpl_can_receive_file,            /* can_receive_file */
    NULL,                                /* send_file */
    NULL,                                /* new_xfer */
    toxprpl_offline_message,             /* offline_message */
    NULL,                                /* whiteboard_prpl_ops */
    NULL,                                /* send_raw */
    NULL,                                /* roomlist_room_serialize */
    NULL,                                /* unregister_user */
    NULL,                                /* send_attention */
    NULL,                                /* get_attention_types */
    sizeof(PurplePluginProtocolInfo),    /* struct_size */
    NULL,                                /* get_account_text_table */
    NULL,                                /* initiate_media */
    NULL,                                /* get_media_caps */
    NULL,                                /* get_moods */
    NULL,                                /* set_public_alias */
    NULL,                                /* get_public_alias */
    toxprpl_add_buddy,                   /* add_buddy_with_invite */
    NULL                                 /* add_buddies_with_invite */
};

static void toxprpl_init(PurplePlugin *plugin)
{
    purple_debug_info("toxprpl", "starting up\n");

    initMessenger();
    //m_callback_friendrequest(on_friend_request);
    m_callback_friendmessage(on_incoming_message);
    m_callback_namechange(on_nick_change);
    m_callback_userstatus(on_status_change);
    m_callback_friendrequest(on_request);
    m_callback_friendstatus(on_friendstatus);

    purple_debug_info("toxprpl", "initialized tox callbacks\n");

    PurpleAccountOption *option = purple_account_option_string_new(
        _("Server"), "dht_server", DEFAULT_SERVER_IP);
    prpl_info.protocol_options = g_list_append(NULL, option);

    option = purple_account_option_int_new(_("Port"), "dht_server_port",
            DEFAULT_SERVER_PORT);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);

    option = purple_account_option_string_new(_("Server key"),
        "dht_server_key", DEFAULT_SERVER_KEY);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);
    purple_prefs_add_none("/plugins");
    purple_prefs_add_none("/plugins/prpl");
    purple_prefs_add_none("/plugins/prpl/tox");
    const char *msg64 = purple_prefs_get_string("/plugins/prpl/tox/messenger");
    if (msg64 != NULL)
    {
        purple_debug_info("toxprpl", "found preference data\n");
        gsize out_len;
        guchar *msg_data = g_base64_decode(msg64, &out_len);
        if (msg_data && (out_len > 0))
        {
            Messenger_load((uint8_t *)msg_data, (uint32_t)out_len);
            g_free(msg_data);
        }
    }
    else
    {
        purple_debug_info("toxprpl", "preferences not found, adding\n", msg64);
        purple_prefs_add_string("/plugins/prpl/tox/messenger", "");
    }


    g_tox_protocol = plugin;
    purple_debug_info("toxprpl", "initialization complete\n");
}

static void toxprpl_destroy(PurplePlugin *plugin)
{
    if (!g_logged_in)
    {
        return;
    }
    purple_debug_info("toxprpl", "shutting down\n");
    purple_timeout_remove(g_tox_messenger_timer);
    purple_timeout_remove(g_tox_connection_timer);

    uint32_t msg_size = Messenger_size();
    guchar *msg_data = g_malloc0(msg_size);
    Messenger_save((uint8_t *)msg_data);

    gchar *msg64 = g_base64_encode(msg_data, msg_size);
    purple_prefs_set_string("/plugins/prpl/tox/messenger", msg64);
    g_free(msg64);
    g_free(msg_data);
    g_logged_in = 0;
}


static PurplePluginInfo info =
{
    PURPLE_PLUGIN_MAGIC,                                /* magic */
    PURPLE_MAJOR_VERSION,                               /* major_version */
    PURPLE_MINOR_VERSION,                               /* minor_version */
    PURPLE_PLUGIN_PROTOCOL,                             /* type */
    NULL,                                               /* ui_requirement */
    0,                                                  /* flags */
    NULL,                                               /* dependencies */
    PURPLE_PRIORITY_DEFAULT,                            /* priority */
    TOXPRPL_ID,                                         /* id */
    "Tox",                                              /* name */
    VERSION,                                            /* version */
    "Tox Protocol Plugin",                              /* summary */
    "Tox Protocol Plugin http://tox.im/",              /* description */
    "Sergey 'Jin' Bostandzhyan",                        /* author */
    PACKAGE_URL,                                        /* homepage */
    NULL,                                               /* load */
    NULL,                                               /* unload */
    toxprpl_destroy,                                    /* destroy */
    NULL,                                               /* ui_info */
    &prpl_info,                                         /* extra_info */
    NULL,                                               /* prefs_info */
    toxprpl_actions,                                    /* actions */
    NULL,                                               /* padding... */
    NULL,
    NULL,
    NULL,
};

PURPLE_INIT_PLUGIN(tox, toxprpl_init, info);
