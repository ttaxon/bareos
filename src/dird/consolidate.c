/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2016-2016 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * BAREOS Director -- consolidate.c -- responsible for doing consolidation jobs
 *
 * based on admin.c
 * Philipp Storz, May 2016
 *
 * Basic tasks done here:
 *   run a virtual full job for all jobs that are configured to be always incremental
 */

#include "bareos.h"
#include "dird.h"

static const int dbglvl = 100;

bool do_consolidate_init(JCR *jcr)
{
   free_rstorage(jcr);
   if (!allow_duplicate_job(jcr)) {
      return false;
   }
   return true;
}

static inline void start_new_consolidation_job(JCR *jcr, char *jobname)
{
   JobId_t jobid;
   UAContext *ua;
   POOL_MEM cmd(PM_MESSAGE);

   ua = new_ua_context(jcr);
   ua->batch = true;
   Mmsg(ua->cmd, "run job=\"%s\" jobid=%s level=VirtualFull %s", jobname, jcr->vf_jobids, jcr->accurate ? "accurate=yes" : "accurate=no");

   Dmsg1(dbglvl, "=============== consolidate cmd=%s\n", ua->cmd);
   parse_ua_args(ua);                 /* parse command */

   jobid = do_run_cmd(ua, ua->cmd);
   if (jobid == 0) {
      Jmsg(jcr, M_ERROR, 0, _("Could not start %s job.\n"), jcr->get_OperationName());
   } else {
      Jmsg(jcr, M_INFO, 0, _("%s JobId %d started.\n"), jcr->get_OperationName(), (int)jobid);
   }

   free_ua_context(ua);
}

/*
 * Returns: false on failure
 *          true  on success
 */
bool do_consolidate(JCR *jcr)
{
   JOBRES *job;
   JOBRES *tmpjob;
   bool retval = true;
   time_t now = time(NULL);

   tmpjob = jcr->res.job; /* Memorize job */

   jcr->jr.JobId = jcr->JobId;

   jcr->fname = (char *)get_pool_memory(PM_FNAME);

   /*
    * Print Job Start message
    */
   Jmsg(jcr, M_INFO, 0, _("Start Consolidate JobId %d, Job=%s\n"), jcr->JobId, jcr->Job);

   jcr->setJobStatus(JS_Running);

   foreach_res(job, R_JOB) {
      if (job->AlwaysIncremental) {
         Jmsg(jcr, M_INFO, 0, _("Looking at always incremental job %s\n"), job->name());

         /*
          * Fake always incremental job as job of current jcr.
          */
         jcr->res.job = job;
         init_jcr_job_record(jcr);
         jcr->jr.JobLevel = L_INCREMENTAL;

         if (!get_or_create_fileset_record(jcr)) {
            Jmsg(jcr, M_FATAL, 0, _("JobId=%d no FileSet\n"), (int)jcr->JobId);
            retval = false;
            goto bail_out;
         }

         db_list_ctx jobids_ctx;
         int32_t incrementals_total;

         /*
          * first determine the number of total incrementals
          */
         db_accurate_get_jobids(jcr, jcr->db, &jcr->jr, &jobids_ctx);
         incrementals_total = jobids_ctx.count - 1;
         Dmsg1(10, "unlimited jobids list:  %s.\n", jobids_ctx.list);

         /*
          * If we are doing always incremental, we need to limit the search to
          * only include incrementals that are older than (now - AlwaysIncrementalJobRetention)
          */
         if (job->AlwaysIncrementalJobRetention) {
            char sdt[50];
            time_t starttime = now - job->AlwaysIncrementalJobRetention;

            bstrftimes(sdt, sizeof(sdt), starttime);
            jcr->jr.StartTime = starttime;
            Jmsg(jcr, M_INFO, 0, _("%s: considering jobs older than %s for consolidation.\n"), job->name(), sdt);
         }

         db_accurate_get_jobids(jcr, jcr->db, &jcr->jr, &jobids_ctx);
         Dmsg1(10, "consolidate candidates:  %s.\n", jobids_ctx.list);

         /*
          * Consolidation of zero or one job does not make sense, we leave it like it is
          */
         if (jobids_ctx.count < 2) {
            Jmsg(jcr, M_INFO, 0, _("%s: less than two jobs to consolidate found, doing nothing.\n"), job->name());
            continue;
         }
         int32_t max_incrementals_to_consolidate;

         /*
          * Calculate limit for query. We specify how many incrementals should be left.
          * the limit is total number of incrementals - number required - 1
          */
         max_incrementals_to_consolidate = incrementals_total - job->AlwaysIncrementalKeepNumber;

         Dmsg2(10, "Incrementals found/required. (%d/%d).\n", incrementals_total, job->AlwaysIncrementalKeepNumber);
         if ((max_incrementals_to_consolidate + 1 ) > 1) {
            jcr->jr.limit = max_incrementals_to_consolidate + 1;
            Dmsg3(10, "total: %d, to_consolidate: %d, limit: %d.\n", incrementals_total, max_incrementals_to_consolidate, jcr->jr.limit);
            jobids_ctx.reset();
            db_accurate_get_jobids(jcr, jcr->db, &jcr->jr, &jobids_ctx);
            Dmsg1(10, "consolidate ids after limit: %s.\n", jobids_ctx.list);
         } else {
            Jmsg(jcr, M_INFO, 0, _("%s: less incrementals than required, not consolidating\n"), job->name());
            continue;
         }

         /*
          * Set the virtualfull jobids to be consolidated
          */
         if (!jcr->vf_jobids) {
            jcr->vf_jobids = get_pool_memory(PM_MESSAGE);
         }
         pm_strcpy(jcr->vf_jobids, jobids_ctx.list);

         Jmsg(jcr, M_INFO, 0, _("%s: Start new consolidation\n"), job->name());
         start_new_consolidation_job(jcr, job->name());
      }
   }

bail_out:
   /*
    * Restore original job back to jcr.
    */
   jcr->res.job = tmpjob;
   jcr->setJobStatus(JS_Terminated);
   consolidate_cleanup(jcr, JS_Terminated);

   return retval;
}

/*
 * Release resources allocated during backup.
 */
void consolidate_cleanup(JCR *jcr, int TermCode)
{
   char sdt[50], edt[50], schedt[50];
   char term_code[100];
   const char *term_msg;
   int msg_type;

   Dmsg0(dbglvl, "Enter backup_cleanup()\n");

   update_job_end(jcr, TermCode);

   if (!db_get_job_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_WARNING, 0, _("Error getting Job record for Job report: ERR=%s"),
         db_strerror(jcr->db));
      jcr->setJobStatus(JS_ErrorTerminated);
   }

   msg_type = M_INFO;                 /* by default INFO message */
   switch (jcr->JobStatus) {
   case JS_Terminated:
      term_msg = _("Consolidate OK");
      break;
   case JS_FatalError:
   case JS_ErrorTerminated:
      term_msg = _("*** Consolidate Error ***");
      msg_type = M_ERROR;          /* Generate error message */
      break;
   case JS_Canceled:
      term_msg = _("Consolidate Canceled");
      break;
   default:
      term_msg = term_code;
      sprintf(term_code, _("Inappropriate term code: %c\n"), jcr->JobStatus);
      break;
   }
   bstrftimes(schedt, sizeof(schedt), jcr->jr.SchedTime);
   bstrftimes(sdt, sizeof(sdt), jcr->jr.StartTime);
   bstrftimes(edt, sizeof(edt), jcr->jr.EndTime);

   Jmsg(jcr, msg_type, 0, _("BAREOS " VERSION " (" LSMDATE "): %s\n"
        "  JobId:                  %d\n"
        "  Job:                    %s\n"
        "  Scheduled time:         %s\n"
        "  Start time:             %s\n"
        "  End time:               %s\n"
        "  Termination:            %s\n\n"),
        edt,
        jcr->jr.JobId,
        jcr->jr.Job,
        schedt,
        sdt,
        edt,
        term_msg);

   Dmsg0(dbglvl, "Leave consolidate_cleanup()\n");
}
