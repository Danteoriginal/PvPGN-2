#include "stdafx.h";
/*
 * Copyright (C) 1998,1999,2000,2001  Ross Combs (rocombs@cs.nmsu.edu)
 * Copyright (C) 2000,2001  Marco Ziech (mmz@gmx.net)
 * Copyright (C) 2002,2003,2004 Mihai RUSU (dizzy@roedu.net)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#define ACCOUNT_INTERNAL_ACCESS
#include "common/setup_before.h"
#include <stdio.h>
#ifdef HAVE_STDDEF_H
# include <stddef.h>
#else
# ifndef NULL
#  define NULL ((void *)0)
# endif
#endif
#ifdef STDC_HEADERS
# include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
# include <string.h>
#else
# ifdef HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif
#include "compat/strchr.h"
#include "compat/strdup.h"
#include "compat/strcasecmp.h"
#include "compat/strncasecmp.h"
#include <ctype.h>
#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif
#include "compat/char_bit.h"
#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include <errno.h>
#include "compat/strerror.h"
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#include "compat/pdir.h"
#include "common/list.h"
#include "common/elist.h"
#include "common/eventlog.h"
#include "prefs.h"
#include "common/util.h"
#include "common/field_sizes.h"
#include "common/bnethash.h"
#include "common/introtate.h"
#include "account.h"
#include "account_wrap.h"
#include "common/hashtable.h"
#include "connection.h"
#include "watch.h"
#include "friends.h"
#include "team.h"
#include "common/tag.h"
#include "ladder.h"
#include "clan.h"
#include "server.h"
#include "attrgroup.h"
#include "attrlayer.h"
#include "storage.h"
#include "common/flags.h"
#include "common/xalloc.h"
#ifdef HAVE_ASSERT_H
# include <assert.h>
#endif
#include "common/setup_after.h"

account_list_type account_list;

unsigned int maxuserid=0;

/* This is to force the creation of all accounts when we initially load the accountlist. */
static int force_account_add=0;

static unsigned int account_hash(char const * username);
static t_account * account_load(t_attrgroup *);
static int account_load_friends(t_account * account);
static int account_unload_friends(t_account * account);
static void account_destroy(t_account * account);
static t_account * accountlist_add_account(t_account * account);

static unsigned int account_hash(char const *username)
{
    register unsigned int h;
    register unsigned int len = strlen(username);

    for (h = 5381; len > 0; --len, ++username) {
        h += h << 5;
        if (isupper((int) *username) == 0)
            h ^= *username;
        else
            h ^= tolower((int) *username);
    }
    return h;
}


static t_account * account_create(char const * username, char const * passhash1)
{
    t_account * account;

    if (username && !passhash1) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL passhash1");
        return NULL;
    }

    if (username && account_check_name(username)) {
        eventlog(eventlog_level_error,__FUNCTION__,"got invalid chars in username");
        return NULL;
    }

//    account = (t_account*)xmalloc(sizeof(t_account));
    account = new t_account;

//    account->name     = NULL;
	account->name=string();
    account->clanmember = NULL;
    account->attrgroup   = NULL;
    account->friends  = NULL;
    account->teams    = NULL;
    account->conn = NULL;
    FLAG_ZERO(&account->flags);

//    account->namehash = 0; /* hash it later before inserting */
    account->uid      = 0; /* hash it later before inserting */

    if (username) { /* actually making a new account */
        /* first check if such a username already owns an account.
         * we search in the memory hash mainly for non-indexed storage types.
         * indexed storage types check themselves if the username exists already 
         * in the storage (see storage_sql.c) */
        if (accountlist_find_account(username)) {
            eventlog(eventlog_level_debug,__FUNCTION__,"user \"%s\" already has an account",username);
            goto err;
        }

        account->attrgroup =  attrgroup_create_newuser(username);
        if(!account->attrgroup) {
            eventlog(eventlog_level_error,__FUNCTION__,"failed to add user");
            goto err;
        }

        account->name = /*xstrdup*/(username);
		account->nameLower=username;
		to_lower(account->nameLower);

        if (account_set_strattr(account,"BNET\\acct\\username",username)<0) {
            eventlog(eventlog_level_error,__FUNCTION__,"could not set username");
            goto err;
        }

        if (account_set_numattr(account,"BNET\\acct\\userid",maxuserid+1)<0) {
            eventlog(eventlog_level_error,__FUNCTION__,"could not set userid");
            goto err;
        }

        if (account_set_strattr(account,"BNET\\acct\\passhash1",passhash1)<0) {
            eventlog(eventlog_level_error,__FUNCTION__,"could not set passhash1");
            goto err;
        }

        if (account_set_numattr(account,"BNET\\acct\\ctime",(unsigned int)now)) {
            eventlog(eventlog_level_error,__FUNCTION__,"could not set ctime");
            goto err;
        }

        if (account_set_numattr(account,"BNET\\acct\\firstlogin_time",(unsigned int)time(NULL))<0)
        {
            eventlog(eventlog_level_error,__FUNCTION__,"could not set acct_firstlogin_time");
            goto err;
        }
    }

    return account;

err:
    account_destroy(account);
    return NULL;
}

static void account_destroy(t_account * account)
{
    assert(account);

    friendlist_close(account->friends);
    teams_destroy(account->teams);
    if (account->attrgroup)
        attrgroup_destroy(account->attrgroup);
//    if (account->name)
//        xfree(account->name);

    delete (account);
}


extern unsigned int account_get_uid(t_account const * account)
{
    if (!account) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return 0;
    }

    return account->uid;
}


extern int account_match(t_account * account, char const * username)
{
    unsigned int userid=0;
    unsigned int namehash;
    char const * tname;

    if (!account)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return -1;
    }
    if (!username)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL username");
        return -1;
    }

///    if (username[0]=='#')
///        if (str_to_uint(&username[1],&userid)<0)
///            userid = 0;

    if (userid)
    {
        if (account->uid==userid)
            return 1;
    }
    else
    {
		string target(username);
		to_lower(target);

		if(account->nameLower==target)
			return 1;
    }

    return 0;
}


extern int account_save(t_account * account, unsigned flags)
{
    assert(account);

    return attrgroup_save(account->attrgroup, flags);
}


extern int account_flush(t_account * account, unsigned flags)
{
    int res;

    assert(account);

    res = attrgroup_flush(account->attrgroup, flags);
    if (res<0) return res;

    account_unload_friends(account);

    return res;
}


extern char const * account_get_strattr_real(t_account * account, char const * key, char const * fn, unsigned int ln)
{
    if (!account) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account (from %s:%u)",fn,ln);
        return NULL;
    }

    if (!key) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL key (from %s:%u)",fn,ln);
        return NULL;
    }

    return attrgroup_get_attr(account->attrgroup, key);
}

extern int account_set_strattr(t_account * account, char const * key, char const * val)
{
    if (!account) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return -1;
    }

    if (!key) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL key");
        return -1;
    }

    return attrgroup_set_attr(account->attrgroup, key, val);
}

static t_account * account_load(t_attrgroup *attrgroup)
{
    t_account * account;

    assert(attrgroup);

    if (!(account = account_create(NULL,NULL))) {
        eventlog(eventlog_level_error,__FUNCTION__,"could not create account");
        return NULL;
    }

    account->attrgroup = attrgroup;

    return account;
}

static t_account * account_load_new(char const * name, unsigned uid)
{
    t_account *account;
    t_attrgroup *attrgroup;

    if (name && account_check_name(name)) return NULL;

    force_account_add = 1; /* disable the protection */
    attrgroup = attrgroup_create_nameuid(name, uid);
    if (!attrgroup) {
		force_account_add = 0;
		return NULL;
    }
    if (!(account = account_load(attrgroup))) {
        eventlog(eventlog_level_error, __FUNCTION__,"could not load account");
        attrgroup_destroy(attrgroup);
        force_account_add = 0;
        return NULL;
    }

    if (!accountlist_add_account(account)) {
        eventlog(eventlog_level_error, __FUNCTION__,"could not add account to list");
        account_destroy(account);
        force_account_add = 0;
        return NULL;
    }

    force_account_add = 0;

    return account;
}

static int _cb_read_accounts(t_attrgroup *attrgroup, void *data)
{
    unsigned int *count = (unsigned int *)data;
    t_account *account;

    if (!(account = account_load(attrgroup))) {
        eventlog(eventlog_level_error, __FUNCTION__,"could not load account");
        attrgroup_destroy(attrgroup);
        return -1;
    }

    if (!accountlist_add_account(account)) {
        eventlog(eventlog_level_error, __FUNCTION__,"could not add account to list");
        account_destroy(account);
        return -1;
    }

    /* might as well free up the memory since we probably won't need it */
    account_flush(account,FS_FORCE); /* force unload */

    (*count)++;

    return 0;
}

extern int accountlist_load_all(int flag)
{
    unsigned int count;
    int starttime = time(NULL);
    static int loaded = 0; /* all accounts already loaded ? */
    int res;

    if (loaded) return 0;

    count = 0;
    res = 0;

    force_account_add = 1; /* disable the protection */
    switch(attrgroup_read_accounts(flag, _cb_read_accounts, &count))
    {
    case -1:
        eventlog(eventlog_level_error, __FUNCTION__,"got error reading users");
        res = -1;
        break;
    case 0:
        loaded = 1;
        eventlog(eventlog_level_info, __FUNCTION__, "loaded %u user accounts in %ld seconds",count,time(NULL) - starttime);
        break;
    default:
        break;
    }
    force_account_add = 0; /* enable the protection */

    return res;
}

extern int accountlist_create(void)
{
    eventlog(eventlog_level_info, __FUNCTION__, "started creating accountlist");

    /* load accounts without force, indexed storage types wont be loading */
    accountlist_load_all(ST_NONE);
    maxuserid = storage->read_maxuserid();

    return 0;
}


extern int accountlist_destroy(void)
{
    t_entry *   curr;
    t_account * account;

    BEGIN(i,account_list)
    {
          account_destroy(&**i);
    }

	account_list.clear();

    return 0;
}


extern const account_list_type& accountlist(void)
{
    return account_list;
}

extern const account_list_type& accountlist_uid(void)
{
    return account_list;
}


extern unsigned int accountlist_get_length(void)
{
    return account_list.size();
}


extern int accountlist_save(unsigned flags)
{
    return attrlayer_save(flags);
}

extern int accountlist_flush(unsigned flags)
{
    return attrlayer_flush(flags);
}

extern t_account * accountlist_find_account(char const * username)
{
    t_entry *    curr;
    t_account *  account;

    if (!username)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL username");
        return NULL;
    }

/*    if (username[0]=='#') {
        if (str_to_uint(&username[1],&userid)<0)
            userid = 0;
    } else if (!(prefs_get_savebyname()))
        if (str_to_uint(username,&userid)<0)
            userid = 0;*/

    /* all accounts in list must be hashed already, no need to check */

/*    if (userid) {
        account=accountlist_find_account_by_uid(userid);
        if (account) return account;
    }*/

	unsigned int namehash;
	char const * tname;

	string strUsername=username;
	boost::algorithm::to_lower(strUsername);
	BOOST_AUTO(find_res,account_list.get<tag_name>().find(strUsername));
	BOOST_AUTO(find_err,account_list.get<tag_name>().end());
	if(find_res!=find_err)
	{
		return &**find_res;
	}

    return account_load_new(username,0);
}


extern t_account * accountlist_find_account_by_uid(unsigned int uid)
{
    t_entry *    curr;
    t_account *  account;

    if (uid) {
		BOOST_AUTO(find_res,account_list.get<tag_uid>().find(uid));
		BOOST_AUTO(find_err,account_list.get<tag_uid>().end());
		if(find_res!=find_err)
		{
			return &**find_res;
		}
    }
    return account_load_new(NULL,uid);
}


extern int accountlist_allow_add(void)
{
    if (force_account_add)
        return 1; /* the permission was forced */

    if (prefs_get_max_accounts()==0)
        return 1; /* allow infinite accounts */

    if (prefs_get_max_accounts()<=account_list.size())
        return 0; /* maximum account limit reached */

    return 1; /* otherwise let them proceed */
}

static t_account * accountlist_add_account(t_account * account)
{
    unsigned int uid;
    char const * username;

    if (!account) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    username = account_get_name(account);
    uid = account_get_numattr(account,"BNET\\acct\\userid");

    if (!username || strlen(username)<1) {
        eventlog(eventlog_level_error,__FUNCTION__,"got bad account (empty username)");
        return NULL;
    }
    if (uid<1) {
        eventlog(eventlog_level_error,__FUNCTION__,"got bad account (bad uid), fix it!");
        uid = maxuserid + 1;
    }

    /* check whether the account limit was reached */
    if (!accountlist_allow_add()) {
        eventlog(eventlog_level_warn,__FUNCTION__,"account limit reached (current is %u, storing %u)",prefs_get_max_accounts(),account_list.size());
        return NULL;
    }

    /* delayed hash, do it before inserting account into the list */
    account->uid = uid;

	{
		BOOST_AUTO(find_res,account_list.get<tag_uid>().find(uid));
		BOOST_AUTO(find_err,account_list.get<tag_uid>().end());
		if(find_res!=find_err)
		{
			eventlog(eventlog_level_debug,__FUNCTION__,"BUG: user \"%s\":"UID_FORMAT" already has an account (\"%s\":"UID_FORMAT")",username,uid,account_get_name(&**find_res),(*find_res)->uid);
			return NULL;
		}
	}
	{
		string strUsername=username;
		boost::algorithm::to_lower(strUsername);
		BOOST_AUTO(find_res,account_list.get<tag_name>().find(strUsername));
		BOOST_AUTO(find_err,account_list.get<tag_name>().end());
		if(find_res!=find_err)
		{
			eventlog(eventlog_level_debug,__FUNCTION__,"BUG: user \"%s\":"UID_FORMAT" already has an account (\"%s\":"UID_FORMAT")",username,uid,account_get_name(*find_res),(*find_res)->uid);
			return NULL;
		}
	}
/*
	printf(" - Before Add %d\n",account_list.size());
	int i=0;
	BEGIN(it,account_list)
	{
		printf("%d : %d, %s : %s\n",++i,(*it)->uid,(*it)->nameLower.c_str(),(*it)->name.c_str());
	}
*/
	account_list.insert((account));
	/*
	printf("CUR %d : %d, %s\n",++i,(account)->uid,(account)->nameLower.c_str(),(account)->name.c_str());

	printf(" - After Add %d\n",account_list.size());
	i=0;
	BEGIN(it,account_list)
	{
		printf("%d : %d, %s : %s\n",++i,(*it)->uid,(*it)->nameLower.c_str(),(*it)->name.c_str());
	}
	*/

    if (uid>maxuserid)
        maxuserid = uid;

    return account;
}

extern t_account * accountlist_create_account(const char *username, const char *passhash1)
{
    t_account *res;

    assert(username != NULL);
    assert(passhash1 != NULL);

    res = account_create(username,passhash1);
    if (!res) return NULL; /* eventlog reported ealier */

    if (!accountlist_add_account(res)) {
        account_destroy(res);
        return NULL; /* eventlog reported earlier */
    }

    account_save(res,FS_FORCE); /* sync new account to storage */

    return res;
}

extern int account_check_name(char const * name)
{
    unsigned int  i;
    char ch;

    if (!name) {
        eventlog(eventlog_level_error, __FUNCTION__,"got NULL name");
        return -1;
    }

    for (i=0; i<strlen(name); i++)
    {
        /* These are the Battle.net rules but they are too strict.
         * We want to allow any characters that wouldn't cause
         * problems so this should test for what is _not_ allowed
         * instead of what is.
         */
        ch = name[i];
        /* hardcoded safety checks */
        if (ch == '/' || ch == '\\') return -1;
        if (isalnum((int)ch)) continue;
        if (strchr(prefs_get_account_allowed_symbols(),ch)) continue;
        return -1;
    }
    if (i<USER_NAME_MIN || i>=USER_NAME_MAX)
        return -1;
    return 0;
}

extern char const * account_get_name_real(t_account * account, char const * fn, unsigned int ln)
{
    char const * temp;

    if (!account)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account (from %s:%u)",fn,ln);
        return NULL; /* FIXME: places assume this can't fail */
    }

    if (account->name.length()!=0) /* we have a cached username so return it */
        return account->name.c_str();

    /* we dont have a cached username so lets get it from attributes */
    if (!(temp = account_get_strattr(account,"BNET\\acct\\username")))
        eventlog(eventlog_level_error,__FUNCTION__,"account has no username");
    else
	{
        account->name = /*xstrdup*/(temp);
		account->nameLower=temp;
		to_lower(account->nameLower);
	}
    return account->name.c_str();
}


extern int account_check_mutual( t_account * account, int myuserid)
{
    if (account == NULL) {
        eventlog(eventlog_level_error, __FUNCTION__, "got NULL account");
        return -1;
    }

    if(!myuserid) {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL userid");
        return -1;
    }

    if(account->friends!=NULL)
    {
        t_friend * fr;
        if((fr=friendlist_find_uid(account->friends, myuserid))!=NULL)
        {
            friend_set_mutual(fr, 1);
            return 0;
        }
    }
    else
    {
        int i;
        int n = account_get_friendcount(account);
        int friend_;
        for(i=0; i<n; i++)
        {
            friend_ = account_get_friend(account,i);
            if(!friend_)  {
                eventlog(eventlog_level_error,__FUNCTION__,"got NULL friend_");
                continue;
            }

            if(myuserid==friend_)
                return 0;
        }
    }

    // If friend isnt in list return -1 to tell func NO
    return -1;
}

extern t_list * account_get_friends(t_account * account)
{
    if (!account)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    if(!FLAG_ISSET(account->flags,ACCOUNT_FLAG_FLOADED))
        if(account_load_friends(account)<0)
        {
            eventlog(eventlog_level_error,__FUNCTION__,"could not load friend list");
            return NULL;
        }

    return account->friends;
}

static int account_load_friends(t_account * account)
{
    int i;
    int n;
    int friend_;
    t_account * acc;
    t_friend * fr;

    int newlist=0;
    if (!account)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return -1;
    }

    if(FLAG_ISSET(account->flags,ACCOUNT_FLAG_FLOADED))
        return 0;

    if(account->friends==NULL)
    {
        account->friends=list_create();
        newlist=1;
    }

    n = account_get_friendcount(account);
    for(i=0; i<n; i++)
    {
        friend_ = account_get_friend(account,i);
        if(!friend_)  {
            account_remove_friend(account, i);
            continue;
        }
        fr=NULL;
        if(newlist || (fr=friendlist_find_uid(account->friends, friend_))==NULL)
        {
            if((acc = accountlist_find_account_by_uid(friend_))==NULL)
            {
                if(account_remove_friend(account, i) == 0)
                {
                    i--;
                    n--;
                }
                continue;
            }
            if(account_check_mutual(acc, account_get_uid(account))==0)
                friendlist_add_account(account->friends, acc, 1);
            else
                friendlist_add_account(account->friends, acc, 0);
        }
        else {
            if((acc=friend_get_account(fr))==NULL)
            {
                account_remove_friend(account, i);
                continue;
            }
            if(account_check_mutual(acc, account_get_uid(account))==0)
                friend_set_mutual(fr, 1);
            else
                friend_set_mutual(fr, 0);
        }
    }
    if(!newlist)
        friendlist_purge(account->friends);
    FLAG_SET(&account->flags,ACCOUNT_FLAG_FLOADED);
    return 0;
}

static int account_unload_friends(t_account * account)
{
    if(friendlist_unload(account->friends)<0)
        return -1;
    FLAG_CLEAR(&account->flags,ACCOUNT_FLAG_FLOADED);
    return 0;
}

extern int account_set_clanmember(t_account * account, t_clanmember * clanmember)
{
    if(account==NULL)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return -1;
    }

    account->clanmember = clanmember;
    return 0;
}

extern t_clanmember * account_get_clanmember(t_account * account)
{
    t_clanmember * member;

    if(account==NULL)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    if ((member = account->clanmember)&&(clanmember_get_clan(member))&&(clan_get_created(clanmember_get_clan(member)) > 0))
        return member;
    else
        return NULL;
}

extern t_clanmember * account_get_clanmember_forced(t_account * account)
{
    if(account==NULL)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    return  account->clanmember;
}

extern t_clan * account_get_clan(t_account * account)
{
    if(account==NULL)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    if(account->clanmember && (clanmember_get_clan(account->clanmember) != NULL) && (clan_get_created(clanmember_get_clan(account->clanmember)) > 0))
        return clanmember_get_clan(account->clanmember);
    else
        return NULL;
}

extern t_clan * account_get_creating_clan(t_account * account)
{
    if(account==NULL)
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    if(account->clanmember && (clanmember_get_clan(account->clanmember) != NULL) && (clan_get_created(clanmember_get_clan(account->clanmember)) <= 0))
        return clanmember_get_clan(account->clanmember);
    else
        return NULL;
}

int account_set_conn(t_account * account, t_connection * conn)
{
    if (!(account))
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return -1;
    }

    account->conn = conn;

    return 0;
}

t_connection * account_get_conn(t_account * account)
{
    if (!(account))
    {
        eventlog(eventlog_level_error,__FUNCTION__,"got NULL account");
        return NULL;
    }

    return account->conn;
}

void account_add_team(t_account * account,t_team * team)
{
    assert(account);
    assert(team);

    if (!(account->teams))
        account->teams = list_create();

    list_append_data(account->teams,team);
}

t_team * account_find_team_by_accounts(t_account * account, t_account **accounts, t_clienttag clienttag)
{
    if ((account->teams))
        return _list_find_team_by_accounts(accounts,clienttag,account->teams);
    else
        return NULL;
}

t_team * account_find_team_by_teamid(t_account * account, unsigned int teamid)
{
    if ((account->teams))
        return _list_find_team_by_teamid(teamid,account->teams);
    else
        return NULL;
}

t_list * account_get_teams(t_account * account)
{
    assert(account);

    return account->teams;
}
