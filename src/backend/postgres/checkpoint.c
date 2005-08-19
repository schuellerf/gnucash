/********************************************************************\
 * checkpoint.c : computes account balance checkpoints              *
 * Copyright (C) 2001 Linas Vepstas <linas@linas.org>               *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
\********************************************************************/

/* 
 * FILE:
 * checkpoint.c
 *
 * FUNCTION:
 * Account balance checkpointing.
 * Not used in single-user mode; vital for multi-user mode.
 *
 * HISTORY:
 * Copyright (c) 2000, 2001 Linas Vepstas
 * 
 */

#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>  
#include <sys/types.h>  

#include <libpq-fe.h>  

#include "Account.h"
#include "AccountP.h"
#include "qofbackend.h"
#include "qofbackend-p.h"
#include "Group.h"
#include "gnc-commodity.h"
#include "gnc-engine-util.h"
#include "guid.h"
#include "qofid.h"

#include "builder.h"
#include "checkpoint.h"
#include "escape.h"

#include "putil.h"

static short module = MOD_BACKEND; 

/* ============================================================= */
/* include autogenerated code */

#include "check-autogen.h"
#include "check-autogen.c"

/* ============================================================= */
/* recompute *all* checkpoints for the account */

static void
pgendAccountRecomputeAllCheckpoints (PGBackend *be, const GUID *acct_guid)
{
   Timespec this_ts, next_ts;
   GMemChunk *chunk;
   GList *node, *checkpoints = NULL;
   PGresult *result;
   Checkpoint *bp;
   char *p;
   int i;
   int nck;
   Account *acc;
   const char *commodity_name, *guid_string;

   if (!be) return;
   ENTER("be=%p", be);

   guid_string = guid_to_string (acct_guid);
   acc = pgendAccountLookup (be, acct_guid);
   commodity_name =
     gnc_commodity_get_unique_name (xaccAccountGetCommodity(acc));

   chunk = g_mem_chunk_create (Checkpoint, 300, G_ALLOC_ONLY);

   /* prevent others from inserting any splits while we recompute 
    * the checkpoints. (hack alert -verify that this is the correct
    * lock) */
   p = "BEGIN WORK;\n"
       "LOCK TABLE gncCheckpoint IN ACCESS EXCLUSIVE MODE;\n"
       "LOCK TABLE gncSplit IN SHARE MODE;\n";
   SEND_QUERY (be,p, );
   FINISH_QUERY(be->connection);

   /* Blow all the old checkpoints for this account out of the water.
    * This should help ensure against accidental corruption.
    */
   p = be->buff; *p = 0;
   p = stpcpy (p, "DELETE FROM gncCheckpoint WHERE accountGuid='");
   p = guid_to_string_buff (acct_guid, p);
   p = stpcpy (p, "';");
   SEND_QUERY (be,be->buff, );
   FINISH_QUERY(be->connection);

   /* malloc a new checkpoint, set it to the dawn of unix time ... */
   bp = g_chunk_new0 (Checkpoint, chunk);
   checkpoints = g_list_prepend (checkpoints, bp);
   this_ts = gnc_iso8601_to_timespec_gmt (CK_EARLIEST_DATE);
   bp->date_start = this_ts;
   bp->account_guid = acct_guid;
   bp->commodity = commodity_name;

   /* loop over entries, creating a set of evenly-spaced checkpoints */
   nck = MIN_CHECKPOINT_COUNT;
   while (1)
   {
      p = be->buff; *p = 0;
      p = stpcpy (p, "SELECT gncTransaction.date_posted"
                     "    FROM gncTransaction, gncSplit"
                     "    WHERE"
                     "        gncSplit.transguid = gncTransaction.transguid AND"
                     "        gncSplit.accountguid='");
      p = stpcpy (p, guid_string);
      p = stpcpy (p, "'"
                     "    ORDER BY gncTransaction.date_posted ASC"
                     "    LIMIT 2 OFFSET ");
      p += sprintf (p, "%d", nck);
      p = stpcpy (p, ";");
      SEND_QUERY (be,be->buff, );

      i=0; 
      do {
         GET_RESULTS (be->connection, result);
         {
            int jrows;
            int ncols = PQnfields (result);
            jrows = PQntuples (result);
            PINFO ("query result %d has %d rows and %d cols",
               i, jrows, ncols);

            if (0 == jrows) {
                FINISH_QUERY(be->connection);
                goto done; 
            }

            if (0 == i) this_ts = gnc_iso8601_to_timespec_gmt (DB_GET_VAL("date_posted",0));
            if (2 == jrows) {
               next_ts = gnc_iso8601_to_timespec_gmt (DB_GET_VAL("date_posted",1));
            } else if (1 == i) {
               next_ts = gnc_iso8601_to_timespec_gmt (DB_GET_VAL("date_posted",0));
            } 
            PQclear (result);
            i++;
         }
      } while (result);

      /* lets see if its time to start a new checkpoint */
      /* look for splits that occur at least ten seconds apart */
      this_ts.tv_sec += 10;
      if (timespec_cmp (&this_ts, &next_ts) < 0)
      {
         /* Set checkpoint five seconds back. This is safe,
          * because we looked for a 10 second gap above */
         this_ts.tv_sec -= 5;
         bp->date_end = this_ts;

         /* and build a new checkpoint */
         bp = g_chunk_new0 (Checkpoint, chunk);
         checkpoints = g_list_prepend (checkpoints, bp);
         bp->date_start = this_ts;
         bp->account_guid = acct_guid;
         bp->commodity = commodity_name;
         nck += MIN_CHECKPOINT_COUNT;
      }
      else 
      {
         /* step one at a time until we find at least a ten-second gap */
         nck += 1;
      }
   }

done:

   /* set the timestamp on the final checkpoint into the distant future */
   this_ts = gnc_iso8601_to_timespec_gmt (CK_LAST_DATE);
   bp->date_end = this_ts;

   /* now store the checkpoints */
   for (node = checkpoints; node; node = node->next)
   {
      bp = (Checkpoint *) node->data;
      pgendStoreOneCheckpointOnly (be, bp, SQL_INSERT);
   }

   g_list_free (checkpoints);
   g_mem_chunk_destroy (chunk);

   /* finally, let the sql server do the heavy lifting of computing the 
    * subtotal balances */
   p = be->buff; *p = 0;
   p = stpcpy (p, "UPDATE gncCheckpoint SET "
          "   balance            = (gncsubtotalbalance        (accountGuid, date_start, date_end )),"
          "   cleared_balance    = (gncsubtotalclearedbalance (accountGuid, date_start, date_end )),"
          "   reconciled_balance = (gncsubtotalreconedbalance (accountGuid, date_start, date_end )) "
          " WHERE accountGuid='");
   p = stpcpy (p, guid_string);
   p = stpcpy (p, "';\n");
   p = stpcpy (p, "COMMIT WORK;\n"
                  "NOTIFY gncCheckpoint;\n");
   SEND_QUERY (be,be->buff, );
   FINISH_QUERY(be->connection);
}

/* ============================================================= */
/* recompute fresh balance checkpoints for every account */

void
pgendGroupRecomputeAllCheckpoints (PGBackend *be, AccountGroup *grp)
{
   GList *acclist, *node;

   acclist = xaccGroupGetSubAccounts(grp);
   for (node = acclist; node; node=node->next)
   {
      Account *acc = (Account *) node->data;
      pgendAccountRecomputeAllCheckpoints (be, xaccAccountGetGUID(acc));
   }
   g_list_free (acclist);
}

/* ============================================================= */
/* recompute *one* checkpoint for the account */

void
pgendAccountRecomputeOneCheckpoint (PGBackend *be, Account *acc, Timespec ts)
{
   char *p, dbuf[80];

   gnc_timespec_to_iso8601_buff (ts, dbuf);

   p = be->buff; *p = 0;
   p = stpcpy (p, "BEGIN WORK;\n"
                  "LOCK TABLE gncCheckpoint IN ACCESS EXCLUSIVE MODE;\n"
                  "LOCK TABLE gncSplit IN SHARE MODE;\n"
                  "UPDATE gncCheckpoint SET "
          "   balance            = (gncsubtotalbalance        (accountGuid, date_start, date_end )),"
          "   cleared_balance    = (gncsubtotalclearedbalance (accountGuid, date_start, date_end )),"
          "   reconciled_balance = (gncsubtotalreconedbalance (accountGuid, date_start, date_end )) "
          " WHERE accountGuid='");
   p = guid_to_string_buff (xaccAccountGetGUID(acc), p);
   p = stpcpy (p, "' AND date_start <= '");
   p = stpcpy (p, dbuf);
   p = stpcpy (p, "' AND date_end > '");
   p = stpcpy (p, dbuf);
   p = stpcpy (p, "';\n");

   p = stpcpy (p, "COMMIT WORK;\n"
                  "NOTIFY gncCheckpoint;\n");
   SEND_QUERY (be,be->buff, );
   FINISH_QUERY(be->connection);
}

/* ============================================================= */
/* recompute all checkpoints affected by this transaction */

void
pgendTransactionRecomputeCheckpoints (PGBackend *be, Transaction *trans)
{
   char *p;

   p = be->buff; *p = 0;
   p = stpcpy (p, "BEGIN WORK;\n"
                  "LOCK TABLE gncCheckpoint IN ACCESS EXCLUSIVE MODE;\n"
                  "LOCK TABLE gncTransaction IN SHARE MODE;\n"
                  "LOCK TABLE gncSplit IN SHARE MODE;\n"
                  "UPDATE gncCheckpoint SET "
   "  balance            = (gncsubtotalbalance        (gncSplit.accountGuid, date_start, date_end )),"
   "  cleared_balance    = (gncsubtotalclearedbalance (gncSplit.accountGuid, date_start, date_end )),"
   "  reconciled_balance = (gncsubtotalreconedbalance (gncSplit.accountGuid, date_start, date_end )) "
   " WHERE gncSplit.transGuid = '");
   p = guid_to_string_buff (xaccTransGetGUID(trans), p);
   p = stpcpy (p, "' AND gncTransaction.transGuid = gncSplit.transGuid "
                  "  AND gncCheckpoint.accountGuid = gncSplit.accountGuid "
                  "  AND date_start <= gncTransaction.date_posted "
                  "  AND date_end > gncTransaction.date_posted;\n"
                  "COMMIT WORK;\n"
                  "NOTIFY gncCheckpoint;\n");
   SEND_QUERY (be,be->buff, );
   FINISH_QUERY(be->connection);
}

/* ============================================================= */
/* get checkpoint value for the account 
 * We find the checkpoint which matches the account and commodity,
 * for the first date immediately preceeding the date.  
 * Then we fill in the balance fields for the returned query.
 */

static gpointer 
get_checkpoint_cb (PGBackend *be, PGresult *result, int j, gpointer data)
{
   Checkpoint *chk = (Checkpoint *) data;
   chk->balance = strtoll(DB_GET_VAL("baln", j), NULL, 0);
   chk->cleared_balance = strtoll(DB_GET_VAL("cleared_baln", j), NULL, 0);
   chk->reconciled_balance = strtoll(DB_GET_VAL("reconed_baln", j), NULL, 0);
   return data;
}

static gpointer 
get_checkpoint_date_cb (PGBackend *be, PGresult *result, int j, gpointer data)
{
   Checkpoint *chk = (Checkpoint *) data;
   chk->date_start = gnc_iso8601_to_timespec_gmt (DB_GET_VAL("date_start", j));
   return data;
}

static void
pgendAccountGetCheckpoint (PGBackend *be, Checkpoint *chk)
{
   sqlEscape *escape;
   char guid_str[80], end_str[80];
   char * p;

   if (!be || !chk) return;
   ENTER("be=%p", be);

   escape = sqlEscape_new ();

   guid_to_string_buff (chk->account_guid, guid_str);
   gnc_timespec_to_iso8601_buff (chk->date_end, end_str);

   /* sum up the total of all the checpoints before this date */
   p = be->buff; *p = 0;
   p = stpcpy (p, "SELECT sum(balance) AS baln, "
                  "       sum(cleared_balance) AS cleared_baln, "
                  "       sum(reconciled_balance) AS reconed_baln "
                  "    FROM gncCheckpoint "
                  "    WHERE accountGuid='");
   p = stpcpy (p, guid_str);
   p = stpcpy (p, "'   AND commodity='");
   p = stpcpy (p, sqlEscapeString (escape, chk->commodity));
   p = stpcpy (p, "'   AND date_end <'");
   p = stpcpy (p, end_str);
   p = stpcpy (p, "';");
   SEND_QUERY (be,be->buff, );

   sqlEscape_destroy (escape);
   escape = NULL;

   pgendGetResults (be, get_checkpoint_cb, chk);

   /* now get the ending date of the last checkpoint,
    * aka the starting date of the next checkpoint */
   p = be->buff; *p = 0;
   p = stpcpy (p, "SELECT date_start FROM gncCheckpoint "
                  "    WHERE accountGuid='");
   p = stpcpy (p, guid_str);
   p = stpcpy (p, "'   AND date_start < '");
   p = stpcpy (p, end_str);
   p = stpcpy (p, "'   ORDER BY date_start DESC LIMIT 1;");
   SEND_QUERY (be,be->buff, );

   /* provide default value, in case there are no checkpoints */
   chk->date_start = gnc_iso8601_to_timespec_gmt (CK_EARLIEST_DATE);
   pgendGetResults (be, get_checkpoint_date_cb, chk);

   LEAVE("be=%p", be);
}

/* ============================================================= */
/* get partial balance for an account */

static void
pgendAccountGetPartialBalance (PGBackend *be, Checkpoint *chk)
{
   char guid_str[80], start_str[80], end_str[80];
   char * p;

   if (!be || !chk) return;
   ENTER("be=%p", be);

   guid_to_string_buff (chk->account_guid, guid_str);
   gnc_timespec_to_iso8601_buff (chk->date_start, start_str);
   gnc_timespec_to_iso8601_buff (chk->date_end, end_str);
   
   /* create the query we need */
   p = be->buff; *p = 0;
   p = stpcpy (p, "SELECT gncSubtotalBalance ('");
   p = stpcpy (p, guid_str);
   p = stpcpy (p, "', '");
   p = stpcpy (p, start_str);
   p = stpcpy (p, "', '");
   p = stpcpy (p, end_str);
   p = stpcpy (p, "') AS baln, "
                  " gncSubtotalClearedBalance ('");
   p = stpcpy (p, guid_str);
   p = stpcpy (p, "', '");
   p = stpcpy (p, start_str);
   p = stpcpy (p, "', '");
   p = stpcpy (p, end_str);
   p = stpcpy (p, "') AS cleared_baln, "
                  " gncSubtotalReconedBalance ('");
   p = stpcpy (p, guid_str);
   p = stpcpy (p, "', '");
   p = stpcpy (p, start_str);
   p = stpcpy (p, "', '");
   p = stpcpy (p, end_str);
   p = stpcpy (p, "') AS reconed_baln;");

   SEND_QUERY (be,be->buff, );

   pgendGetResults (be, get_checkpoint_cb, chk);

   LEAVE("be=%p", be);
}

/* ============================================================= */
/* get checkpoint value for one accounts */

void
pgendAccountGetBalance (PGBackend *be, Account *acc, Timespec as_of_date)
{
   Checkpoint chk;
   const gnc_commodity *com;
   gint64 b, cl_b, rec_b, deno;
   gnc_numeric baln;
   gnc_numeric cleared_baln;
   gnc_numeric reconciled_baln;

   if (!be || !acc) return;
   ENTER("be=%p", be);

   /* setup what we will match for */
   chk.date_end = as_of_date;

   com = xaccAccountGetCommodity(acc);
   if (!com)
   {
     PERR ("account %s has no commodity",
           guid_to_string (xaccAccountGetGUID (acc)));
     return;
   }

   chk.commodity = gnc_commodity_get_unique_name(com);
   chk.account_guid = xaccAccountGetGUID (acc);
   chk.balance = 0;
   chk.cleared_balance = 0;
   chk.reconciled_balance = 0;

   /* get the checkpoint */
   pgendAccountGetCheckpoint (be, &chk);

   b = chk.balance;
   cl_b = chk.cleared_balance;
   rec_b = chk.reconciled_balance;
   deno = gnc_commodity_get_fraction (com);

   DEBUGCMD({
      char buf[80];
      gnc_timespec_to_iso8601_buff (chk.date_start, buf);
      PINFO("%s balance to %s baln=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " clr=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " rcn=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, 
        xaccAccountGetDescription (acc), buf,
        b, deno, cl_b, deno, rec_b, deno);
      })

   /* add up loose entries since the checkpoint */
   pgendAccountGetPartialBalance (be, &chk);

   b += chk.balance;
   cl_b += chk.cleared_balance;
   rec_b += chk.reconciled_balance;

   /* set the account balances */
   baln = gnc_numeric_create (b, deno);
   cleared_baln = gnc_numeric_create (cl_b, deno);
   reconciled_baln = gnc_numeric_create (rec_b, deno);

   xaccAccountSetStartingBalance (acc, baln, cleared_baln, reconciled_baln);

   DEBUGCMD ({
      char buf[80];
      gnc_timespec_to_iso8601_buff (as_of_date, buf);
      LEAVE("be=%p %s %s baln=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " clr=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " rcn=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, be, 
        xaccAccountGetDescription (acc), buf,
        b, deno, cl_b, deno, rec_b, deno);
      })
}

/* ============================================================= */
/* get checkpoint value for all accounts */

void
pgendGroupGetAllBalances (PGBackend *be, AccountGroup *grp, 
                          Timespec as_of_date)
{
   GList *acclist, *node;

   if (!be || !grp) return;
   ENTER("be=%p", be);

   /* loop over all accounts */
   acclist = xaccGroupGetSubAccounts (grp);
   for (node=acclist; node; node=node->next)
   {
      Account *acc = (Account *) node->data;
      pgendAccountGetBalance (be, acc, as_of_date);
   }

   g_list_free (acclist);
   LEAVE("be=%p", be);
}

/* ======================== END OF FILE ======================== */
