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

#include <Messenger.h>
#include <network.h>

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
#include <arpa/inet.h>

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

#define TOXPRPL_MAX_STATUS        4
#define TOXPRPL_STATUS_ONLINE       0
#define TOXPRPL_STATUS_AWAY         1
#define TOXPRPL_STATUS_BUSY         2
#define TOXPRPL_STATUS_OFFLINE      3

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
static void toxprpl_query_buddy_info(gpointer data, gpointer user_data);

static unsigned char *toxprpl_tox_hex_string_to_id(const char *hex_string);

// stay independent from the lib
static int toxprpl_get_status_index(Messenger *m, int fnum, USERSTATUS status)
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
                if (m_friendstatus(m, fnum) == FRIEND_ONLINE)
                {
                    return TOXPRPL_STATUS_ONLINE;
                }
            }
    }
    return TOXPRPL_STATUS_OFFLINE;
}

static USERSTATUS toxprpl_get_tox_status_from_id(const char *status_id)
{
    int i;
    for (i = 0; i < TOXPRPL_MAX_STATUS; i++)
    {
        if (strcmp(toxprpl_statuses[i].id, status_id) == 0)
        {
            return toxprpl_statuses[i].tox_status;
        }
    }
    return USERSTATUS_INVALID;
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
    string_id[CLIENT_ID_SIZE * 2] = '\0';
    return string_id;
}


/* tox specific stuff */
static void on_connectionstatus(Messenger *m, int fnum, uint8_t status)
{
    int tox_status = TOXPRPL_STATUS_OFFLINE;
    if (status == 1)
    {
        tox_status = TOXPRPL_STATUS_ONLINE;
    }

    purple_debug_info("toxprpl", "Friend status change: %d\n", status);
    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, fnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend #%d\n",
                          fnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    PurpleAccount *account = purple_connection_get_account(g_tox_gc);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[tox_status].id, NULL);
    g_free(buddy_key);
}

static void on_request(uint8_t* public_key, uint8_t* data,
                       uint16_t length)
{
    gchar *dialog_message;

    if (g_tox_gc == NULL)
    {
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(public_key);
    purple_debug_info("toxprpl", "Buddy request from %s: %s\n",
                      buddy_key, data);

    PurpleAccount *account = purple_connection_get_account(g_tox_gc);
    PurpleBuddy *buddy = purple_find_buddy(account, buddy_key);
    if (buddy != NULL)
    {
        purple_debug_info("toxprpl", "Buddy %s already in buddy list!\n",
                          buddy_key);
        g_free(buddy_key);
        return;
    }

    dialog_message = g_strdup_printf("The user %s has sent you a friend "
                                    "request, do you want to add him?",
                                    buddy_key);

    gchar *request_msg = NULL;
    if (length > 0)
    {
        request_msg = g_strndup((const gchar *)data, length);
    }

    purple_request_yes_no(g_tox_gc, "New friend request", dialog_message,
                          request_msg,
                          PURPLE_DEFAULT_ACTION_NONE,
                          purple_connection_get_account(g_tox_gc), NULL,
                          NULL,
                          buddy_key, // buddy key will be freed elsewhere
                          G_CALLBACK(toxprpl_add_to_buddylist),
                          G_CALLBACK(toxprpl_do_not_add_to_buddylist));
    g_free(dialog_message);
    g_free(request_msg);
}

static void on_incoming_message(Messenger *m, int friendnum, uint8_t* string,
                                uint16_t length)
{
    purple_debug_info("toxprpl", "Message received!\n");
    if (g_tox_gc == NULL)
    {
        return;
    }

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    serv_got_im(g_tox_gc, buddy_key, (const char *)string, PURPLE_MESSAGE_RECV, time(NULL));
    g_free(buddy_key);
}

static void on_nick_change(Messenger *m, int friendnum, uint8_t* data,
                           uint16_t length)
{
    purple_debug_info("toxprpl", "Nick change!\n");

    if (g_tox_gc == NULL)
    {
        return;
    }

    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
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
    purple_blist_alias_buddy(buddy, (const char *)data);
}

static void on_status_change(Messenger *m, int friendnum, USERSTATUS userstatus)
{
    purple_debug_info("toxprpl", "Status change: %d\n", userstatus);
    uint8_t client_id[CLIENT_ID_SIZE];
    if (getclient_id(m, friendnum, client_id) < 0)
    {
        purple_debug_info("toxprpl", "Could not get id of friend %d\n",
                          friendnum);
        return;
    }

    gchar *buddy_key = toxprpl_tox_bin_id_to_string(client_id);
    PurpleAccount *account = purple_connection_get_account(g_tox_gc);
    purple_debug_info("toxprpl", "Setting user status for user %s to %s\n",
        buddy_key, toxprpl_statuses[
                        toxprpl_get_status_index(m, friendnum, userstatus)].id);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[toxprpl_get_status_index(m, friendnum, userstatus)].id,
        NULL);
    g_free(buddy_key);
}

static gboolean tox_messenger_loop(gpointer data)
{
    PurpleConnection *gc = (PurpleConnection *)data;
    doMessenger(purple_connection_get_protocol_data(gc));
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
        g_slist_foreach(buddy_list, toxprpl_query_buddy_info, gc);
        g_slist_free(buddy_list);

        purple_account_set_username(account, id);

        Messenger *m = purple_connection_get_protocol_data(gc);
        uint8_t our_name[MAX_NAME_LENGTH];
        uint16_t name_len = getself_name(m, our_name);
        // bug in the library?
        if (name_len == 0)
        {
            our_name[0] = '\0';
        }

        const char *nick = purple_account_get_string(account, "nickname", NULL);
        if (nick == NULL)
        {
            if (strlen((const char *)our_name) > 0)
            {
                purple_connection_set_display_name(gc, (const char *)our_name);
                purple_account_set_string(account, "nickname",
                                                      (const char *)our_name);
            }
        }
        else
        {
            purple_connection_set_display_name(gc, nick);
            if (strcmp(nick, (const char *)our_name) != 0)
            {
                setname(m, (uint8_t *)nick, strlen(nick) + 1);
            }
        }
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

static void toxprpl_set_status(PurpleAccount *account, PurpleStatus *status)
{
    const char* status_id = purple_status_get_id(status);
    const char *message = purple_status_get_attr_string(status, "message");

    PurpleConnection *gc = purple_account_get_connection(account);
    Messenger *m = purple_connection_get_protocol_data(gc);

    purple_debug_info("toxprpl", "setting status %s\n", status_id);

    USERSTATUS tox_status = toxprpl_get_tox_status_from_id(status_id);
    if (tox_status == USERSTATUS_INVALID)
    {
        purple_debug_info("toxprpl", "status %s is invalid\n", status_id);
        return;
    }

    m_set_userstatus(m, tox_status);
    if ((message != NULL) && (strlen(message) > 0))
    {
        m_set_statusmessage(m, (uint8_t *)message, strlen(message) + 1);
    }
    // FOKEL
}
// query buddy status
static void toxprpl_query_buddy_info(gpointer data, gpointer user_data)
{
    purple_debug_info("toxprpl", "toxprpl_query_buddy_info\n");
    PurpleBuddy *buddy = (PurpleBuddy *)data;
    PurpleConnection *gc = (PurpleConnection *)user_data;
    Messenger *m = purple_connection_get_protocol_data(gc);

    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data == NULL)
    {
        unsigned char *bin_key = toxprpl_tox_hex_string_to_id(buddy->name);
        int fnum = getfriend_id(m, bin_key);
        buddy_data = g_new0(toxprpl_buddy_data, 1);
        buddy_data->tox_friendlist_number = fnum;
        purple_buddy_set_protocol_data(buddy, buddy_data);
        g_free(bin_key);
    }

    PurpleAccount *account = purple_connection_get_account(gc);
    purple_debug_info("toxprpl", "Setting user status for user %s to %s\n",
        buddy->name, toxprpl_statuses[toxprpl_get_status_index(m,
            buddy_data->tox_friendlist_number,
            m_get_userstatus(m, buddy_data->tox_friendlist_number))].id);
    purple_prpl_got_user_status(account, buddy->name,
        toxprpl_statuses[toxprpl_get_status_index(m,
            buddy_data->tox_friendlist_number,
            m_get_userstatus(m, buddy_data->tox_friendlist_number))].id,
        NULL);

    uint8_t alias[MAX_NAME_LENGTH];
    if (getname(m, buddy_data->tox_friendlist_number, alias) == 0)
    {
        purple_blist_alias_buddy(buddy, (const char*)alias);
    }
}

static void report_status_change(PurpleConnection *from, PurpleConnection *to,
        gpointer userdata)
{
    purple_debug_info("toxprpl", "notifying %s that %s changed status\n",
            to->account->username, from->account->username);
    discover_status(to, from, NULL);
}

static const char *toxprpl_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
    return "tox";
}

static GList *toxprpl_status_types(PurpleAccount *acct)
{
    GList *types = NULL;
    PurpleStatusType *type;
    int i;

    purple_debug_info("toxprpl", "setting up status types\n");

    for (i = 0; i < TOXPRPL_MAX_STATUS; i++)
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
    if (len != 64)
    {
        purple_debug_info("toxprpl", "invalid string key %s\n", hex_string);
        return NULL;
    }
    unsigned char *bin = g_malloc(len);
    const char *p = hex_string;
    for (i = 0; i < len / 2; i++)
    {
        sscanf(p, "%2hhx", &bin[i]);
        p = p + 2;
    }
    return bin;
}

static void toxprpl_login(PurpleAccount *acct)
{
    IP_Port dht;

    purple_debug_info("toxprpl", "logging in...\n");

    Messenger *m = initMessenger();
    if (m == NULL)
    {
        purple_debug_info("toxprpl", "Fatal error, could not allocate memory "
                                     "for messenger!\n");
        return;

    }

    m_callback_friendmessage(m, on_incoming_message);
    m_callback_namechange(m, on_nick_change);
    m_callback_userstatus(m, on_status_change);
    m_callback_friendrequest(m, on_request);
    m_callback_connectionstatus(m, on_connectionstatus);

    purple_debug_info("toxprpl", "initialized tox callbacks\n");

    PurpleConnection *gc = purple_account_get_connection(acct);
    purple_connection_set_protocol_data(gc, m);

    g_tox_gc = gc;

    purple_debug_info("toxprpl", "logging in %s\n", acct->username);

    const char *msg64 = purple_account_get_string(acct, "messenger", NULL);
    if (msg64 != NULL)
    {
        purple_debug_info("toxprpl", "found preference data\n");
        gsize out_len;
        guchar *msg_data = g_base64_decode(msg64, &out_len);
        if (msg_data && (out_len > 0))
        {
            Messenger_load(m, (uint8_t *)msg_data, (uint32_t)out_len);
            g_free(msg_data);
        }
    }
    else
    {
        purple_debug_info("toxprpl", "preferences not found\n");
        //purple_account_add_string("/plugins/prpl/tox/messenger", "");
    }

    purple_connection_update_progress(gc, _("Connecting"),
            0,   /* which connection step this is */
            2);  /* total number of steps */

    const char* ip = purple_account_get_string(acct, "dht_server",
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
    g_free(bin_str);
    purple_debug_info("toxprpl", "Will connect to %s:%d (%s)\n" ,
                      ip, ntohs(dht.port), key);
    g_tox_messenger_timer = purple_timeout_add(100, tox_messenger_loop, gc);
    purple_debug_info("toxprpl", "added messenger timer as %d\n",
                      g_tox_messenger_timer);
    g_tox_connection_timer = purple_timeout_add_seconds(2, tox_connection_check,
                                                        gc);
}

static void toxprpl_close(PurpleConnection *gc)
{
    /* notify other toxprpl accounts */
    purple_debug_info("toxprpl", "Closing!\n");
    foreach_toxprpl_gc(report_status_change, gc, NULL);

    PurpleAccount *account = purple_connection_get_account(gc);
    Messenger *m = purple_connection_get_protocol_data(gc);

    uint32_t msg_size = Messenger_size(m);
    guchar *msg_data = g_malloc0(msg_size);
    Messenger_save(m, (uint8_t *)msg_data);

    gchar *msg64 = g_base64_encode(msg_data, msg_size);
    purple_account_set_string(account, "messenger", msg64);
    g_free(msg64);
    g_free(msg_data);

    purple_debug_info("toxprpl", "shutting down\n");
    purple_timeout_remove(g_tox_messenger_timer);
    purple_timeout_remove(g_tox_connection_timer);

    cleanupMessenger(m);
}

static int toxprpl_send_im(PurpleConnection *gc, const char *who,
        const char *message, PurpleMessageFlags flags)
{
    const char *from_username = gc->account->username;

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
    Messenger *m = purple_connection_get_protocol_data(gc);
    m_sendmessage(m, buddy_data->tox_friendlist_number, (uint8_t *)message,
                  strlen(message)+1);
    return 1;
}

static int toxprpl_tox_addfriend(Messenger *m, const char *buddy_key,
                                 gboolean sendrequest)
{
    unsigned char *bin_key = toxprpl_tox_hex_string_to_id(buddy_key);
    int ret;

    if (sendrequest == TRUE)
    {
        ret = m_addfriend(m, bin_key, (uint8_t *)DEFAULT_REQUEST_MESSAGE,
                          (uint16_t)strlen(DEFAULT_REQUEST_MESSAGE) + 1);
    }
    else
    {
        ret = m_addfriend_norequest(m, bin_key);
    }

    g_free(bin_key);
    const char *msg;
    switch (ret)
    {
        case FAERR_TOOLONG:
            msg = "Message too long";
            break;
        case FAERR_NOMESSAGE:
            msg = "Missing request message";
            break;
        case FAERR_OWNKEY:
            msg = "You're trying to add yourself as a friend";
            break;
        case FAERR_ALREADYSENT:
            msg = "Friend request already sent";
            break;
        case FAERR_UNKNOWN:
            msg = "Error adding friend";
            break;
        default:
            purple_debug_info("toxprpl", "Friend %s added as %d\n", buddy_key,
                              ret);
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

    Messenger *m = purple_connection_get_protocol_data(g_tox_gc);

    int ret = toxprpl_tox_addfriend(m, buddy_key, FALSE);
    if (ret < 0)
    {
        g_free(buddy_key);
        // error dialogs handled in toxprpl_tox_addfriend()
        return;
    }

    PurpleAccount *account = purple_connection_get_account(g_tox_gc);

    uint8_t alias[MAX_NAME_LENGTH];

    PurpleBuddy *buddy;
    if ((getname(m, ret, alias) == 0) && (strlen((const char *)alias) > 0))
    {
        purple_debug_info("toxprpl", "Got friend alias %s\n", alias);
        buddy = purple_buddy_new(account, buddy_key, (const char*)alias);
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
    USERSTATUS userstatus = m_get_userstatus(m, ret);
    purple_debug_info("toxprpl", "Friend %s has status %d\n",
            buddy_key, userstatus);
    purple_prpl_got_user_status(account, buddy_key,
        toxprpl_statuses[toxprpl_get_status_index(m, ret, userstatus)].id,
        NULL);

    g_free(buddy_key);
}

static void toxprpl_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
        PurpleGroup *group, const char *msg)
{
    purple_debug_info("toxprpl", "adding %s to buddy list\n", buddy->name);

    Messenger *m = purple_connection_get_protocol_data(gc);
    int ret = toxprpl_tox_addfriend(m, buddy->name, TRUE);
    if ((ret < 0) && (ret != FAERR_ALREADYSENT))
    {
        purple_blist_remove_buddy(buddy);
    }
    // buddy data will be added by the query_buddy_info function
    toxprpl_query_buddy_info((gpointer)buddy, (gpointer)gc);
}

static void toxprpl_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
        PurpleGroup *group)
{
    purple_debug_info("toxprpl", "removing buddy %s\n", buddy->name);
    Messenger *m = purple_connection_get_protocol_data(gc);
    toxprpl_buddy_data *buddy_data = purple_buddy_get_protocol_data(buddy);
    if (buddy_data != NULL)
    {
        purple_debug_info("toxprpl", "removing tox friend #%d\n", buddy_data->tox_friendlist_number);
        m_delfriend(m, buddy_data->tox_friendlist_number);
    }
}

static void toxprpl_set_nick_action(PurpleConnection *gc,
                                    PurpleRequestFields *fields)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    Messenger *m = purple_connection_get_protocol_data(gc);
    const char *nickname = purple_request_fields_get_string(fields,
                                                            "text_nickname");
    if (nickname != NULL)
    {
        purple_connection_set_display_name(gc, nickname);
        setname(m, (uint8_t *)nickname, strlen(nickname) + 1);
        purple_account_set_string(account, "nickname", nickname);
    }
}

static void toxprpl_action_set_nick_dialog(PurplePluginAction *action)
{
    PurpleConnection *gc = (PurpleConnection*)action->context;
    PurpleAccount *account = purple_connection_get_account(gc);

    PurpleRequestFields *fields;
    PurpleRequestFieldGroup *group;
    PurpleRequestField *field;

    fields = purple_request_fields_new();
    group = purple_request_field_group_new(NULL);
    purple_request_fields_add_group(fields, group);

    field = purple_request_field_string_new("text_nickname",
                    _("Nickname"),
                    purple_account_get_string(account, "nickname", ""), FALSE);

    purple_request_field_group_add_field(group, field);
    purple_request_fields(gc, _("Set your nickname"), NULL, NULL, fields,
            _("_Set"), G_CALLBACK(toxprpl_set_nick_action),
            _("_Cancel"), NULL,
            account, account->username, NULL, gc);
}

static GList *toxprpl_account_actions(PurplePlugin *plugin, gpointer context)
{
    purple_debug_info("toxprpl", "setting up account actions\n");

    GList *actions = NULL;
    PurplePluginAction *action;

    action = purple_plugin_action_new(_("Set nickname..."),
             toxprpl_action_set_nick_dialog);
    actions = g_list_append(actions, action);
    return actions;
}

static void toxprpl_free_buddy(PurpleBuddy *buddy)
{
    if (buddy->proto_data)
    {
        toxprpl_buddy_data *buddy_data = buddy->proto_data;
        g_free(buddy_data);
    }
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
    NO_BUDDY_ICONS,
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
    toxprpl_set_status,                 /* set_status */
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
    NULL,                                      /* can_receive_file */
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

    PurpleAccountOption *option = purple_account_option_string_new(
        _("Nickname"), "nickname", "");
    prpl_info.protocol_options = g_list_append(NULL, option);

    option = purple_account_option_string_new(
        _("Server"), "dht_server", DEFAULT_SERVER_IP);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);

    option = purple_account_option_int_new(_("Port"), "dht_server_port",
            DEFAULT_SERVER_PORT);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);

    option = purple_account_option_string_new(_("Server key"),
        "dht_server_key", DEFAULT_SERVER_KEY);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
                                               option);
    g_tox_protocol = plugin;
    purple_debug_info("toxprpl", "initialization complete\n");
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
    NULL,                                               /* destroy */
    NULL,                                               /* ui_info */
    &prpl_info,                                         /* extra_info */
    NULL,                                               /* prefs_info */
    toxprpl_account_actions,                            /* actions */
    NULL,                                               /* padding... */
    NULL,
    NULL,
    NULL,
};

PURPLE_INIT_PLUGIN(tox, toxprpl_init, info);
