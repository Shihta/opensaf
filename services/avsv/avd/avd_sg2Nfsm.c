/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
 * under the GNU Lesser General Public License Version 2.1, February 1999.
 * The complete license can be accessed from the following location:
 * http://opensource.org/licenses/lgpl-license.php
 * See the Copying file included with the OpenSAF distribution for full
 * licensing terms.
 *
 * Author(s): Emerson Network Power
 *
 */

/*****************************************************************************
..............................................................................



..............................................................................

  DESCRIPTION: This file is part of the SG processing module. It contains
  the SG state machine, for processing the events related to SG for 2N
  redundancy model.


..............................................................................

  FUNCTIONS INCLUDED in this file:

  avd_sg_2n_act_susi - Finds SUSIs belonging to a SI which has active
                       and standby asssignments. 
  avd_sg_2n_su_chose_asgn - Choose and assign SIs to SUs.
  avd_sg_2n_si_func - Function to process the new complete SI in the SG.
  avd_sg_2n_suswitch_func - function called when a operator does a SU switch.
  avd_sg_2n_siswitch_func - function called when a operator does a SI switch.
  avd_sg_2n_su_fault_func - function is called when a SU failed and switchover
                            needs to be done.
  avd_sg_2n_su_insvc_func - function is called when a SU readiness state changes
                            to inservice from out of service
  avd_sg_2n_susi_sucss_func - processes successful SUSI assignment. 
  avd_sg_2n_susi_fail_func - processes failure of SUSI assignment.
  avd_sg_2n_realign_func - function called when SG operation is done or cluster
                           timer expires.
  avd_sg_2n_node_fail_func - function is called when the node has already failed and
                             the SIs have to be failed over.
  avd_sg_2n_su_admin_fail -  function is called when SU is LOCKED/SHUTDOWN.  
  avd_sg_2n_si_admin_down - function is called when SIs is LOCKED/SHUTDOWN.
  avd_sg_2n_sg_admin_down - function is called when SGs is LOCKED/SHUTDOWN.
******************************************************************************
*/

/*
 * Module Inclusion Control...
 */

#include "avd.h"



/*****************************************************************************
 * Function: avd_sg_2n_act_susi
 *
 * Purpose:  This function searches the entire SG to identify the
 * SUSIs belonging to a SI which has active and standby asssignments. 
 *
 * Input: cb - the AVD control block
 *        sg - The pointer to the service servive group.
 * stby_susi - pointer to the pointer of standby SUSI if present.
 *
 * Returns: AVD_SU_SI_REL * Pointer to active SUSI if present.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

static AVD_SU_SI_REL * avd_sg_2n_act_susi(AVD_CL_CB *cb,AVD_SG *sg, 
                                   AVD_SU_SI_REL **stby_susi)
{

   NCS_BOOL l_flag = TRUE;
   AVD_SU_SI_REL *susi;
   AVD_SU_SI_REL *a_susi = AVD_SU_SI_REL_NULL;
   AVD_SU_SI_REL *s_susi = AVD_SU_SI_REL_NULL;
   AVD_SI *l_si;

   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_act_susi");
   
   l_si = sg->list_of_si;   

   /* check for configured SIs and see if they have been already
    * assigned to SUs. Find the active and standby SUSI assignments. If any.
    */

   while ((l_si != AVD_SI_NULL) && (l_flag == TRUE))
   {

      /* check to see if this SI has both the assignments */
      if((susi = l_si->list_of_sisu) == AVD_SU_SI_REL_NULL)
      {
         l_si = l_si->sg_list_of_si_next;
         continue;
      }
      
      switch(susi->state)
      {
      case SA_AMF_HA_ACTIVE:
      case SA_AMF_HA_QUIESCING:
         /* Check if this SI has both active and standby assignments */
         a_susi = susi;
         if(susi->si_next != AVD_SU_SI_REL_NULL)
         {
            s_susi = susi->si_next;
            l_flag = FALSE;
         }
         break;
      case SA_AMF_HA_STANDBY:
         /* Check if this SI has both active and standby assignments */
         s_susi = susi;
         if(susi->si_next != AVD_SU_SI_REL_NULL)
         {
            a_susi = susi->si_next;
            l_flag = FALSE;
         }
         break;
      case SA_AMF_HA_QUIESCED:
         /* Check if this SI has both active and standby assignments */         
         if(susi->si_next != AVD_SU_SI_REL_NULL)
         {
            if(susi->si_next->state == SA_AMF_HA_ACTIVE)
            {
               s_susi = susi;
               a_susi = susi->si_next;
            }else
            {
               a_susi = susi;
               s_susi = susi->si_next;
            }
            
            l_flag = FALSE;
         }else
         {
            a_susi = susi;
         }
         break;
      default:
         break;
         
      } /* switch(susi->state) */

      l_si = l_si->sg_list_of_si_next;

   } /* while ((l_si != AVD_SI_NULL) && (l_flag == TRUE)) */

   *stby_susi = s_susi;
   
   return a_susi;

}

/*****************************************************************************
 * Function: avd_sg_2n_su_chose_asgn
 *
 * Purpose:  This function will identify the current active SU. 
 * If not available choose the first SU in the SG that is in-service.
 * Send D2N-INFO_SU_SI_ASSIGN message for the SU with role active for the SIs in
 * the SG list that dont have active assignment. If the active assignments meet
 * all the criteria identify the standby SU, If not available identify the next
 * in-service SU and send standby assignments for the SIs that have active
 * assignments. It returns the SU on which the assignment started, if no
 * assignments returns NULL.
 *
 * Input: cb - the AVD control block
 *        sg - The pointer to the service group.
 *        
 *
 * Returns: pointer to the SU that is undergoing assignment. Null if
 *          no assignments need to happen.
 *
 * NOTES: none.
 *
 * 
 **************************************************************************/

static AVD_SU *avd_sg_2n_su_chose_asgn(AVD_CL_CB *cb,AVD_SG *sg)
{
   AVD_SU_SI_REL *a_susi;
   AVD_SU_SI_REL *s_susi;
   AVD_SU *a_su,*s_su;
   AVD_SI *i_si;
   NCS_BOOL l_flag=TRUE;
   AVD_SU_SI_REL *tmp_susi;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_su_chose_asgn");
   
   a_susi = avd_sg_2n_act_susi(cb,sg,&s_susi);
   
   if (a_susi == AVD_SU_SI_REL_NULL)
   {
      /* No active assignment exists. Scan the ranked list of SUs in the SG
       * to identify a in-service SU 
       */
      a_su = sg->list_of_su;
      while((a_su != AVD_SU_NULL) && (l_flag == TRUE))
      {
         if(a_su->readiness_state == NCS_IN_SERVICE)
         {
            l_flag = FALSE;
            continue;
         }
         
         a_su = a_su->sg_list_su_next;
      }
      
      if (a_su == AVD_SU_NULL)
      {
         /* No in service SUs available in the SG. */
         return AVD_SU_NULL;
      }
   }else /* if (a_susi == AVD_SU_SI_REL_NULL) */
   {
      a_su = a_susi->su;
   }
   
   if (a_su->readiness_state != NCS_IN_SERVICE)
   {
      /* The current active SU is OOS so return */
      return AVD_SU_NULL;
   }

   /* check if any more active SIs can be assigned to this SU */
   if (a_su->si_max_active > a_su->si_curr_active)
   {
      l_flag = FALSE;
      /* choose and assign SIs in the SG that dont have active assignment */
      i_si = sg->list_of_si;
      while ((i_si != AVD_SI_NULL) && (a_su->si_max_active > a_su->si_curr_active))
      {
         /* Screen SI sponsors state and adjust the SI-SI dep state accordingly */
         avd_screen_sponsor_si_state(cb, i_si, FALSE);

         if ((i_si->admin_state == NCS_ADMIN_STATE_UNLOCK) &&
             (i_si->max_num_csi == i_si->num_csi) &&
             (i_si->si_dep_state == AVD_SI_NO_DEPENDENCY) &&
             (i_si->list_of_sisu == AVD_SU_SI_REL_NULL))
         {
            /* found a SI that needs active assignment. */
            if(avd_new_assgn_susi(cb,a_su,i_si,SA_AMF_HA_ACTIVE, FALSE, &tmp_susi) == NCSCC_RC_SUCCESS)
            {
               l_flag = TRUE;
            }else
            {
               /* log a fatal error */               
               m_AVD_LOG_INVALID_VAL_ERROR(((long)i_si));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(i_si->name_net.value,i_si->name_net.length);
            }
         }
         i_si = i_si->sg_list_of_si_next;
      } /* while ((i_si != AVD_SI_NULL) && (a_su->si_max_active > a_su->si_curr_active)) */
      
      /* if any assignments have been done return the SU */
      if (l_flag == TRUE)
      {
         return a_su;
      }
   } /* if(a_su->si_max_active > a_su->si_curr_active) */
      
   
   
   if (s_susi == AVD_SU_SI_REL_NULL)
   {
      /* No standby assignment exists. Scan the ranked list of SUs in the SG
       * to identify a in-service SU with no assignments. 
       */
      l_flag = TRUE;
      s_su = sg->list_of_su;
      while((s_su != AVD_SU_NULL) && (l_flag == TRUE))
      {
         if((s_su->readiness_state == NCS_IN_SERVICE) &&
            (s_su->list_of_susi == AVD_SU_SI_REL_NULL))
         {
            l_flag = FALSE;
            continue;
         }
         
         s_su = s_su->sg_list_su_next;
      }
      
      if (s_su == AVD_SU_NULL)
      {
         /* No in service SUs available in the SG, that can be made standby. */
         return AVD_SU_NULL;
      }
   }else /* if (s_susi == AVD_SU_SI_REL_NULL) */
   {
      s_su = s_susi->su;
   }
   
   if (s_su->readiness_state != NCS_IN_SERVICE)
   {
      /* The current standby SU is OOS so return */
      return AVD_SU_NULL;
   }

   /* check if any more standby SIs can be assigned to this SU */
   if (s_su->si_max_standby > s_su->si_curr_standby)
   {
      l_flag = FALSE;
      /* choose and assign SIs in the SG that have active assignment but dont
       * have standby assignment.
       */
      i_si = sg->list_of_si;
      while ((i_si != AVD_SI_NULL) && (s_su->si_max_standby > s_su->si_curr_standby))
      {
         if (i_si->list_of_sisu != AVD_SU_SI_REL_NULL)
         {
            /* found a SI that has active assignment. check if it has standby
             * assignment. If not assign standby to this SU. 
             */
            if(i_si->list_of_sisu->si_next == AVD_SU_SI_REL_NULL)
            {
               if(avd_new_assgn_susi(cb,s_su,i_si,SA_AMF_HA_STANDBY, FALSE, &tmp_susi) == NCSCC_RC_SUCCESS)
               {
                  l_flag = TRUE;
               }else
               {
                  /* log a fatal error */              
                  m_AVD_LOG_INVALID_VAL_ERROR(((long)i_si));
                  m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(i_si->name_net.value,i_si->name_net.length);
               }
            } /* if(i_si->list_of_sisu->si_next == AVD_SU_SI_REL_NULL) */
            
         } /* if (i_si->list_of_sisu != AVD_SU_SI_REL_NULL) */
         
         i_si = i_si->sg_list_of_si_next;
      } /* while ((i_si != AVD_SI_NULL) && (s_su->si_max_standby > s_su->si_curr_standby)) */
      
      /* if any assignments have been done return the SU */
      if (l_flag == TRUE)
      {
         return s_su;
      }
   } /* if(s_su->si_max_standby > s_su->si_curr_standby) */
   
   /* If we are here it means no new assignments have been done so just 
    * return NULL
    */
   
   return AVD_SU_NULL;
}


/*****************************************************************************
 * Function: avd_sg_2n_si_func
 *
 * Purpose:  This function is called when a new SI is added to a SG. The SG is
 * of type 2N redundancy model. This function will perform the functionality
 * described in the SG FSM design. 
 *
 * Input: cb - the AVD control block
 *        si - The pointer to the service instance.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function. If there are
 * any SIs being transitioned due to operator, this call will return
 * failure.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_si_func(AVD_CL_CB *cb,AVD_SI *si)
{
   AVD_SU *l_su;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_si_func");
   m_AVD_LOG_RCVD_VAL(((long)si));
   
   m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
   
   /* If the SG FSM state is not stable just return success. */
   if (si->sg_of_si->sg_fsm_state != AVD_SG_FSM_STABLE)
   {
      return NCSCC_RC_SUCCESS;
   }
   
   if((cb->init_state != AVD_APP_STATE) &&
      (si->sg_of_si->sg_ncs_spec == SA_FALSE))
   {
      m_AVD_LOG_INVALID_VAL_ERROR(si->sg_of_si->sg_ncs_spec);
      return NCSCC_RC_SUCCESS;
   }
   
   if ((l_su = avd_sg_2n_su_chose_asgn(cb,si->sg_of_si)) == AVD_SU_NULL)
   {
      /* all the assignments have already been done in the SG. */
      return NCSCC_RC_SUCCESS;
   }
   
   /* Add the SU to the list and change the FSM state */
   avd_sg_su_oper_list_add(cb,l_su,FALSE);
   
   m_AVD_SET_SG_FSM(cb,(si->sg_of_si),AVD_SG_FSM_SG_REALIGN);
   
   m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
 
   return NCSCC_RC_SUCCESS;
}


/*****************************************************************************
 * Function: avd_sg_2n_suswitch_func
 *
 * Purpose:  This function is called when a operator does a SU switch on
 * a SU that belongs to 2N redundancy model SG. 
 * This will trigger a role change action as described in the SG FSM design.
 *
 * Input: cb - the AVD control block
 *        su - The pointer to the SU that needs to be switched.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_suswitch_func(AVD_CL_CB *cb,AVD_SU *su)
{
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_suswitch_func");
   m_AVD_LOG_RCVD_VAL(((long)su));
   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);

   /* Switch operation is not allowed on NCS SGs, when the AvD is not in
    * application state, if su has no SI assignments and If the SG FSM state is
    * not stable.
    */
   if((cb->init_state != AVD_APP_STATE) ||
      (su->sg_of_su->sg_ncs_spec == SA_TRUE) ||
      (su->sg_of_su->sg_fsm_state != AVD_SG_FSM_STABLE) ||
      (su->list_of_susi == AVD_SU_SI_REL_NULL))
   {
      return NCSCC_RC_FAILURE;
   }
   
   /* switch operation is not valid on SUs that have only standby assignment
    * or if the SG has no standby SU.
    */

   if((su->sg_of_su->su_assigned_num != 2) ||
      (su->list_of_susi->state == SA_AMF_HA_STANDBY))
   {
      return NCSCC_RC_FAILURE;
   }
   
   /* Modify all the SU SIs to quiesced state.*/
   if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
   {
      m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
      m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
      return NCSCC_RC_FAILURE;
   }

   /* Add the SU to the operation list and change the SG state to SU_operation. */
   avd_sg_su_oper_list_add(cb,su,FALSE);
   m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   
   return NCSCC_RC_SUCCESS;
} 



/*****************************************************************************
 * Function: avd_sg_2n_siswitch_func
 *
 * Purpose:  This function is called when a operator does a SI switch on
 * a SI that belongs to 2N redundancy model SG. 
 * This will trigger a role change action as described in the SG FSM design.
 *
 * Input: cb - the AVD control block
 *        si - The pointer to the SI that needs to be switched.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_siswitch_func(AVD_CL_CB *cb,AVD_SI *si)
{
   AVD_SU *a_su;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_siswitch_func");
   m_AVD_LOG_RCVD_VAL(((long)si));
   
   m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
   
   /* Switch operation is not allowed on NCS SGs, when the AvD is not in
    * application state, if si has no SU assignments and If the SG FSM state is
    * not stable.
    */
   if((cb->init_state != AVD_APP_STATE) ||
      (si->sg_of_si->sg_ncs_spec == SA_TRUE) ||
      (si->sg_of_si->sg_fsm_state != AVD_SG_FSM_STABLE) ||
      (si->list_of_sisu == AVD_SU_SI_REL_NULL))
   {
      return NCSCC_RC_FAILURE;
   }
   
   /* switch operation is not valid on SIs that have only one assignment.
    */
   if(si->list_of_sisu->si_next == AVD_SU_SI_REL_NULL)
   {
      return NCSCC_RC_FAILURE;
   }
   
   if (si->list_of_sisu->state == SA_AMF_HA_ACTIVE)
   {
      a_su = si->list_of_sisu->su;
   }else
   {
      a_su = si->list_of_sisu->si_next->su;
   }
   
   /* change the state for all assignments to quiesced. */
   if(avd_sg_su_si_mod_snd(cb,a_su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
   {
      m_AVD_LOG_INVALID_VAL_ERROR(((long)a_su));
      m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_su->name_net.value,a_su->name_net.length);
      return NCSCC_RC_FAILURE;
   }

   /* Add the SI to the SG admin pointer and change the SG state to SI_operation. */
   m_AVD_SET_SG_ADMIN_SI(cb,si);
   m_AVD_SET_SG_FSM(cb,(si->sg_of_si),AVD_SG_FSM_SI_OPER);   
   
   m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
   
   return NCSCC_RC_SUCCESS;
}
 

 /*****************************************************************************
 * Function: avd_sg_2n_su_fault_su_oper
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_su_fault_func.
 * It is called if the SG is in AVD_SG_FSM_SU_OPER.
 *
 * Input: cb - the AVD control block
 *        su - The pointer to the service unit.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static uns32 avd_sg_2n_su_fault_su_oper(AVD_CL_CB *cb,AVD_SU *su)
{
   AVD_SU *a_su;
   NCS_BOOL flag;
   AVD_AVND *su_node_ptr = NULL;
   
   /* If the SU is same as the SU in the list and if the SI relationships to
    * the SU is quiesced or  quiescing. If this SU admin is shutdown change
    * to LOCK and send D2N-INFO_SU_SI_ASSIGN modify quiesced all. If this
    * SU switch state is TRUE change to FALSE.
    */
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_su_fault_su_oper");
   
   if(su->sg_of_su->su_oper_list.su == su)
   {
      if (su->list_of_susi->state == SA_AMF_HA_QUIESCED)
      {
         m_AVD_SET_SU_SWITCH(cb,su,AVSV_SI_TOGGLE_STABLE);
      }else if (su->list_of_susi->state == SA_AMF_HA_QUIESCING)
      {
         if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }

         m_AVD_GET_SU_NODE_PTR(cb,su,su_node_ptr);
 
         if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {               
            m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
         }else if (su_node_ptr->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {
            m_AVD_IS_NODE_LOCK((su_node_ptr),flag);
            if (flag == TRUE)
            {
               m_AVD_SET_AVND_SU_ADMIN(cb,(su_node_ptr),NCS_ADMIN_STATE_LOCK);
            }
         }
      } else
      {
         /* log a fatal error */
         m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
         m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
         m_AVD_LOG_INVALID_VAL_ERROR(su->list_of_susi->state);
         return NCSCC_RC_FAILURE;         
      }
   }else /* if(su->sg_of_su->su_oper_list.su == su) */
   {
      /* the SU is not the same as the SU in the list */         
      if(su->list_of_susi->state == SA_AMF_HA_STANDBY)
      {
         /* the SU has standby SI assignments Send D2N-INFO_SU_SI_ASSIGN 
          * remove all to this SU. Add the SU to the operation list. 
          * Change state to SG_realign.
          */

         /* change the state for all assignments to unassign and send delete mesage. */
         if(avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         /* add the SU to the operation list and change the SG FSM to SG realign. */
         avd_sg_su_oper_list_add(cb,su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         /* if the other SUs switch field is TRUE, it is in service, 
          * having quiesced assigning state Send D2N-INFO_SU_SI_ASSIGN modify 
          * active all to the other SU.
          */
         
         a_su = su->sg_of_su->su_oper_list.su;
         
         if ((a_su->su_switch == AVSV_SI_TOGGLE_SWITCH) &&
            (a_su->readiness_state == NCS_IN_SERVICE) &&
            (a_su->list_of_susi->state == SA_AMF_HA_QUIESCED))
         {
            avd_sg_su_si_mod_snd(cb,a_su,SA_AMF_HA_ACTIVE);
            m_AVD_SET_SU_SWITCH(cb,a_su,AVSV_SI_TOGGLE_STABLE);
         }
            
      }/* if(su->list_of_susi->state == SA_AMF_HA_STANDBY) */
      else if (su->list_of_susi->state == SA_AMF_HA_ACTIVE)
      {
         /* The SU has active SI assignments Send D2N-INFO_SU_SI_ASSIGN 
          * modify quiesced to this SU. Change state to SG_realign. 
          * Add this SU to the operation list.
          */
         
         if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            /* log error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         /* add the SU to the operation list and change the SG FSM to SG realign. */
         avd_sg_su_oper_list_add(cb,su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         
         /* If the other SUs switch field is TRUE, it is in service, 
          * having quiesced assigned state Change switch field to false.
          */
          
         a_su = su->sg_of_su->su_oper_list.su;
         
         if ((a_su->su_switch == AVSV_SI_TOGGLE_SWITCH) &&
            (a_su->readiness_state == NCS_IN_SERVICE) &&
            (a_su->list_of_susi->state == SA_AMF_HA_QUIESCED))
         {
            m_AVD_SET_SU_SWITCH(cb,a_su,AVSV_SI_TOGGLE_STABLE);           
         }else if (a_su->list_of_susi->state == SA_AMF_HA_QUIESCED)
         {
            /* the other SU has quiesced assignments meaning either it is
             * out of service or locked. So just send a remove request
             * to that SU.
             */
            avd_sg_su_si_del_snd(cb,a_su);
         }

      } /* if (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      
   } /* else (su->sg_of_su->su_oper_list.su == su) */
 
   return NCSCC_RC_SUCCESS;
}

 /*****************************************************************************
 * Function: avd_sg_2n_su_fault_si_oper
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_su_fault_func.
 * It is called if the SG is in AVD_SG_FSM_SI_OPER.
 *
 * Input: cb - the AVD control block
 *        su - The pointer to the service unit.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static uns32 avd_sg_2n_su_fault_si_oper(AVD_CL_CB *cb,AVD_SU *su)
{   
   AVD_SU_SI_REL *l_susi,*o_susi;
   SaAmfHAStateT old_state;
   AVD_SU_SI_STATE old_fsm_state;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_su_fault_si_oper");

   if (su->sg_of_su->admin_si->list_of_sisu->su == su)
   {
      l_susi = su->sg_of_su->admin_si->list_of_sisu;
      o_susi = su->sg_of_su->admin_si->list_of_sisu->si_next;
   }else
   {
      l_susi = su->sg_of_su->admin_si->list_of_sisu->si_next;
      o_susi = su->sg_of_su->admin_si->list_of_sisu;
   }
      
   if (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH)
   {
      /* the SI switch operation is TRUE */
      
      
      /* If this SI relation with the SU is quiesced assigning or this SI
       * relation with the SU is quiesced assigned and the other SU is
       * active assigning. Change the SI switch state to false. Remove the
       * SI from the SI admin pointer. Add the SU to the SU operation list.
       * Change state to SU_operation.
       */
      if(((l_susi->state == SA_AMF_HA_QUIESCED) && (l_susi->fsm == AVD_SU_SI_STATE_MODIFY))
         || ((l_susi->state == SA_AMF_HA_QUIESCED) && (l_susi->fsm == AVD_SU_SI_STATE_ASGND)
         && (o_susi->state == SA_AMF_HA_ACTIVE) && (o_susi->fsm == AVD_SU_SI_STATE_MODIFY)))
      {
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         avd_sg_su_oper_list_add(cb,su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } else if ((l_susi->state == SA_AMF_HA_QUIESCED) && (l_susi->fsm == AVD_SU_SI_STATE_ASGND)
      && (o_susi->state == SA_AMF_HA_STANDBY) && (o_susi->fsm == AVD_SU_SI_STATE_ASGND))
      {
         /* If this SI relation with the SU is quiesced assigned and the 
          * other SU is standby assigned. Change the SI switch state to false.
          * Remove the SI from the SI admin pointer. 
          * Send D2N-INFO_SU_SI_ASSIGN modify active all to the other SU. 
          * Add the SU to the operation list. Change state to SU_operation state.
          */
         
         if(avd_sg_su_si_mod_snd(cb,o_susi->su,SA_AMF_HA_ACTIVE) == NCSCC_RC_FAILURE)
         {
            /* log error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)o_susi->su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_susi->su->name_net.value,o_susi->su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         avd_sg_su_oper_list_add(cb,o_susi->su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } else if ((l_susi->state == SA_AMF_HA_QUIESCED) && (l_susi->fsm == AVD_SU_SI_STATE_ASGND)
      && (o_susi->state == SA_AMF_HA_ACTIVE) && (o_susi->fsm == AVD_SU_SI_STATE_ASGND))
      {
         /* If this SI relation with the SU is quiesced assigned and the 
          * other SU is active assigned. Change the SI switch state to false.
          * Remove the SI from the SI admin pointer. 
          * Send D2N-INFO_SU_SI_ASSIGN remove all to the SU. Add that SU
          * to operation list. Change state to SG_realign.
          */
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
                
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE); 
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
      }else if((l_susi->state == SA_AMF_HA_STANDBY) && (l_susi->fsm == AVD_SU_SI_STATE_ASGND)
               && ((o_susi->state == SA_AMF_HA_QUIESCED) || (o_susi->state == SA_AMF_HA_QUIESCING)))
      {
         /* If this SI relation with the SU is standby assigned and the 
          * other SU is quiesced/quiescing assigned/assigning. Send 
          * D2N-INFO_SU_SI_ASSIGN with modified active all to the other SU.
          * Change the switch value of the SI to false. Remove the SI from
          * the admin pointer. Change state to SG_realign. Add that SU to
          * operation list. Send D2N-INFO_SU_SI_ASSIGN with remove all to
          * this SU. Add this SU to operation list.
          */
         if(avd_sg_su_si_mod_snd(cb,o_susi->su,SA_AMF_HA_ACTIVE) == NCSCC_RC_FAILURE)
         {
            /* log error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)o_susi->su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_susi->su->name_net.value,o_susi->su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         avd_sg_su_si_del_snd(cb,su);
         
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         avd_sg_su_oper_list_add(cb,o_susi->su,FALSE);
         avd_sg_su_oper_list_add(cb,su,FALSE);
               
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);           
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      }else if((l_susi->state == SA_AMF_HA_ACTIVE) && (l_susi->fsm == AVD_SU_SI_STATE_MODIFY)
               && (o_susi->state == SA_AMF_HA_QUIESCED) && (o_susi->fsm == AVD_SU_SI_STATE_ASGND))
      {
         /* If this SI relation with the SU is active assigning and the
          * other SU is quiesced assigned. Change the switch value of the
          * SI to false. Remove the SI from the admin pointer. 
          * Send D2N-INFO_SU_SI_ASSIGN with modified quiesced all to this
          * SU. Add this and the other SU to operation list. 
          * Change state to SG_realign.
          */            
         
         if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            /* log error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         avd_sg_su_oper_list_add(cb,o_susi->su,FALSE);
         avd_sg_su_oper_list_add(cb,su,FALSE);

         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);           
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      }else
      {
         /* Log a fatal error. */
         m_AVD_LOG_INVALID_VAL_ERROR(l_susi->state);
         return NCSCC_RC_FAILURE;
      }         
      
   }/* if (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH) */ 
   else if (su->sg_of_su->admin_si->admin_state == NCS_ADMIN_STATE_LOCK)
   {         
      /* the SI admin operation is LOCK */
      
      if (l_susi != AVD_SU_SI_REL_NULL)
      {
         if(l_susi->state == SA_AMF_HA_QUIESCED)
         {
            /* this SI relation with the SU is quiesced assigning. Remove
             * the SI from the SI admin pointer. Send D2N-INFO_SU_SI_ASSIGN
             * with modified quiesced all to this SU. Add the SU to the 
             * SU operation list. Change state to SU_operation state.
             */
            if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
            {
               /* log error */                  
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));               
            avd_sg_su_oper_list_add(cb,su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }else
         {
            /* The SU has standby assignments. Send D2N-INFO_SU_SI_ASSIGN
             * with remove all to the SU. 
             */
            if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
         }
      }else /* if (l_susi != AVD_SU_SI_REL_NULL)*/
      {
         /* The SU has standby assignments. Send D2N-INFO_SU_SI_ASSIGN
          * with remove all to the SU. 
          */
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
      } /* else (l_susi != AVD_SU_SI_REL_NULL)*/
      
   }else if (su->sg_of_su->admin_si->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
   {
      /* the SI admin operation is shutdown */
      if (l_susi != AVD_SU_SI_REL_NULL)
      {
         if(l_susi->state == SA_AMF_HA_QUIESCING)
         {
            /* this SI relation with the SU is quiescing assigning/assigned
             * Change the SI admin state to unlock. Remove the SI from the 
             * SI admin pointer. Send D2N-INFO_SU_SI_ASSIGN with modified 
             * quiesced all to this SU. Add the SU to the SU operation list. 
             * Change state to SU_operation state.
             */
            
            if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
            {
               /* log error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            m_AVD_SET_SI_ADMIN(cb,(su->sg_of_su->admin_si),NCS_ADMIN_STATE_UNLOCK);
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));               
            avd_sg_su_oper_list_add(cb,su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }else
         {
            /* The SU has standby assignments. Change the SI admin state to
             * unlock. Remove the SI from the SI admin pointer. 
             * Send D2N-INFO_SU_SI_ASSIGN modify active for the SI to 
             * the other SU. Change state to SG_realign state. Add both the 
             * SUs to the operation list. Send D2N-INFO_SU_SI_ASSIGN remove 
             * all to this SU.
             */
            old_state = o_susi->state;
            old_fsm_state = o_susi->fsm;
            o_susi->state = SA_AMF_HA_ACTIVE;
            o_susi->fsm = AVD_SU_SI_STATE_MODIFY;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, o_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, o_susi);
            if (avd_snd_susi_msg(cb,o_susi->su,o_susi,AVSV_SUSI_ACT_MOD) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)o_susi));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_susi->su->name_net.value,o_susi->su->name_net.length);
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_susi->si->name_net.value,o_susi->si->name_net.length);
               o_susi->state = old_state;
               o_susi->fsm = old_fsm_state;
               m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, o_susi, AVSV_CKPT_AVD_SU_SI_REL);
               avd_gen_su_ha_state_changed_trap(cb, o_susi);
               return NCSCC_RC_FAILURE;
            }
            
            m_AVD_SET_SI_ADMIN(cb,(su->sg_of_su->admin_si),NCS_ADMIN_STATE_UNLOCK);
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
            avd_sg_su_si_del_snd(cb,su);
            avd_sg_su_oper_list_add(cb,su,FALSE);
            avd_sg_su_oper_list_add(cb,o_susi->su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);              
         }
      }else /* if (l_susi != AVD_SU_SI_REL_NULL)*/
      {
         /* The SU has standby assignments. Change the SI admin state to
          * unlock. Remove the SI from the SI admin pointer. 
          * Send D2N-INFO_SU_SI_ASSIGN modify active for the SI to 
          * the other SU. Change state to SG_realign state. Add both the 
          * SUs to the operation list. Send D2N-INFO_SU_SI_ASSIGN remove 
          * all to this SU.
          */
         o_susi = su->sg_of_su->admin_si->list_of_sisu;
         old_state = o_susi->state;
         old_fsm_state = o_susi->fsm;
         o_susi->state = SA_AMF_HA_ACTIVE;
         o_susi->fsm = AVD_SU_SI_STATE_MODIFY;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, o_susi, AVSV_CKPT_AVD_SU_SI_REL);
         avd_gen_su_ha_state_changed_trap(cb, o_susi);
         if (avd_snd_susi_msg(cb,o_susi->su,o_susi,AVSV_SUSI_ACT_MOD) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)o_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_susi->su->name_net.value,o_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_susi->si->name_net.value,o_susi->si->name_net.length);
            o_susi->state = old_state;
            o_susi->fsm = old_fsm_state;
            avd_gen_su_ha_state_changed_trap(cb, o_susi);
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, o_susi, AVSV_CKPT_AVD_SU_SI_REL);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SI_ADMIN(cb,(su->sg_of_su->admin_si),NCS_ADMIN_STATE_UNLOCK);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         avd_sg_su_si_del_snd(cb,su);
         avd_sg_su_oper_list_add(cb,su,FALSE);
         avd_sg_su_oper_list_add(cb,o_susi->su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* else (l_susi != AVD_SU_SI_REL_NULL)*/
      
   }else
   {
      /* log a fatal error */
      m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
      m_AVD_LOG_INVALID_VAL_ERROR(su->sg_of_su->admin_si->admin_state);
      return NCSCC_RC_FAILURE;
   }
   
   return NCSCC_RC_SUCCESS;

}

 /*****************************************************************************
 * Function: avd_sg_2n_su_fault_func
 *
 * Purpose:  This function is called when a SU readiness state changes to
 * OOS due to a fault. It will do the functionality specified in
 * SG FSM.
 *
 * Input: cb - the AVD control block
 *        su - The pointer to the service unit.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_su_fault_func(AVD_CL_CB *cb,AVD_SU *su)
{
   AVD_SU *a_su;
   AVD_SU_SI_REL *l_susi,*o_susi;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_su_fault_func");
   m_AVD_LOG_RCVD_VAL(((long)su));
   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   
   if(su->list_of_susi == AVD_SU_SI_REL_NULL)
      return NCSCC_RC_SUCCESS;
   
   /* Do the functionality based on the current state.*/
   switch(su->sg_of_su->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      
      if (su->list_of_susi->state == SA_AMF_HA_ACTIVE)
      {
         /* this is a active SU. */
         /* change the state for all assignments to quiesced. */
         if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            /* log a fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         /* add the SU to the operation list and change the SG FSM to SU operation. */
         avd_sg_su_oper_list_add(cb,su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         
      }else /* if (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      {
         /* means standby */
         /* change the state for all assignments to unassign and send delete mesage. */
         if(avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         /* add the SU to the operation list and change the SG FSM to SG realign. */
         avd_sg_su_oper_list_add(cb,su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         
      } /* else (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SG_REALIGN:
     
      l_susi = su->list_of_susi;
      while ((l_susi != AVD_SU_SI_REL_NULL) && (l_susi->fsm == AVD_SU_SI_STATE_UNASGN))
      {
         l_susi = l_susi->su_next;
      }

      if (l_susi == AVD_SU_SI_REL_NULL)
      {
         /* Do nothing as already the su si is being removed */
      }else 
      if (l_susi->state == SA_AMF_HA_ACTIVE)
      {
         /* If (the SU has active assignment Send D2N-INFO_SU_SI_ASSIGN
          * modify quiesced all. to the SU. Add the SU to the operation list.
          */
         if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         avd_sg_su_oper_list_add(cb,su,FALSE);
      }else
      {
         /* the SU has standby assignment. send D2N-INFO_SU_SI_ASSIGN remove
          * all to the SU. Add the SU to the operation list.
          */
         if(avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
      }
      
      break; /* case AVD_SG_FSM_SG_REALIGN: */
   case AVD_SG_FSM_SU_OPER:
      
      if (avd_sg_2n_su_fault_su_oper(cb,su) == NCSCC_RC_FAILURE)
         return NCSCC_RC_FAILURE;
      
      break; /* case AVD_SG_FSM_SU_OPER: */
   case AVD_SG_FSM_SI_OPER:
   
      if (avd_sg_2n_su_fault_si_oper(cb,su) == NCSCC_RC_FAILURE)
         return NCSCC_RC_FAILURE;
      
      break; /* case AVD_SG_FSM_SI_OPER: */
   case AVD_SG_FSM_SG_ADMIN:
      

      if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK)
      {
         /* the SG is lock no action. */
         return NCSCC_RC_SUCCESS;
      }else if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {
         /* the SI relationships to the SU is quiescing. Change admin state to
          * lock, make the readiness state of all the SUs OOS.  Send 
          * D2N-INFO_SU_SI_ASSIGN modify quiesced all to the SU. Identify its
          * standby SU assignment send D2N-INFO_SU_SI_ASSIGN remove all for the
          * SU. 
          */
         if (su->list_of_susi->state == SA_AMF_HA_QUIESCING)
         {
            if (avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            o_susi = AVD_SU_SI_REL_NULL;
            a_su = su->sg_of_su->list_of_su;
            while (a_su != AVD_SU_NULL)
            {
               m_AVD_SET_SU_REDINESS(cb,a_su,NCS_OUT_OF_SERVICE);
               if ((a_su->list_of_susi != AVD_SU_SI_REL_NULL) && (
                  a_su->list_of_susi->state == SA_AMF_HA_STANDBY))
               {
                  o_susi = a_su->list_of_susi;
               }
               a_su = a_su->sg_list_su_next;
            }
            m_AVD_SET_SG_ADMIN(cb,(su->sg_of_su),NCS_ADMIN_STATE_LOCK);
            if (o_susi != AVD_SU_SI_REL_NULL)
               avd_sg_su_si_del_snd(cb,o_susi->su);
         } /* if (su->list_of_susi->state == SA_AMF_HA_QUIESCING) */
         else
         {
             /* the SI relationships to the SU is standby Send 
              * D2N-INFO_SU_SI_ASSIGN remove all for the SU.
              */
            if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
         } /* else (su->list_of_susi->state == SA_AMF_HA_QUIESCING) */         
      }else
      {
         /* log fatal error */
         m_AVD_LOG_INVALID_VAL_ERROR(su->sg_of_su->admin_state);
         return NCSCC_RC_FAILURE;
      }
     
      break; /* case AVD_SG_FSM_SG_ADMIN: */   
   default:
      /* log fatal error about the invalid value */
      m_AVD_LOG_INVALID_VAL_FATAL(su->sg_of_su->sg_fsm_state);
      return NCSCC_RC_FAILURE;
      break;
   } /* switch(su->sg_of_su->sg_fsm_state) */

   return NCSCC_RC_SUCCESS;
}

 /*****************************************************************************
 * Function: avd_sg_2n_su_insvc_func
 *
 * Purpose:  This function is called when a SU readiness state changes
 * to inservice from out of service. The SG is of type 2N redundancy
 * model. It will do the functionality specified in the SG FSM.
 *
 * Input: cb - the AVD control block
 *        su - The pointer to the service unit.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: none.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_su_insvc_func(AVD_CL_CB *cb,AVD_SU *su)
{

   AVD_SU *l_su;   
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_su_insvc_func");
   m_AVD_LOG_RCVD_VAL(((long)su));
   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   
   /* An SU will not become in service when the SG is being locked or shutdown.
    */
   if (su->sg_of_su->sg_fsm_state == AVD_SG_FSM_SG_ADMIN)
   {
      m_AVD_LOG_INVALID_VAL_FATAL(su->sg_of_su->sg_fsm_state);
      return NCSCC_RC_FAILURE;
   }
   
   /* If the SG FSM state is not stable just return success. */
   if (su->sg_of_su->sg_fsm_state != AVD_SG_FSM_STABLE)
   {
      return NCSCC_RC_SUCCESS;
   }   
   
   if((cb->init_state != AVD_APP_STATE) &&
      (su->sg_of_su->sg_ncs_spec == SA_FALSE))
   {
      return NCSCC_RC_SUCCESS;
   }
   
   if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
   {
      avd_sg_app_su_inst_func(cb, su->sg_of_su);

      /* all the assignments have already been done in the SG. */
      return NCSCC_RC_SUCCESS;
   }
   
   /* Add the SU to the list and change the FSM state */
   avd_sg_su_oper_list_add(cb,l_su,FALSE);
   
   m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   return NCSCC_RC_SUCCESS;

}

 /*****************************************************************************
 * Function: avd_sg_2n_susi_sucss_sg_reln
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_susi_sucss_func.
 * It is called if the SG is in AVD_SG_FSM_SG_REALIGN.
 *
 * Input: cb - the AVD control block
 *        su - In case of entire SU related operation the SU for
 *               which the ack is received.
 *        susi - The pointer to the service unit service instance relationship.
 *        act  - The action received in the ack message.
 *        state - The HA state in the message.
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static uns32 avd_sg_2n_susi_sucss_sg_reln(AVD_CL_CB *cb,AVD_SU *su,AVD_SU_SI_REL *susi,
                                AVSV_SUSI_ACT act, SaAmfHAStateT state)
{
   AVD_SU_SI_REL *i_susi, *s_susi, *o_susi, *a_susi;
   AVD_SU *o_su, *l_su;
   NCS_BOOL flag,as_flag;
   
   m_AVD_CHK_OPLIST(su,flag);
   
   if (flag == FALSE)
   {
      /* Log fatal error  */         
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
      return NCSCC_RC_FAILURE;
   }
   
   if (susi == AVD_SU_SI_REL_NULL)
   {
      /* assign all */
      if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED))
      {
         a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
         
         /* quiesced all and SU is in the operation list */
         if (su->readiness_state == NCS_OUT_OF_SERVICE)
         {
            /* this SU is out of service Send a D2N-INFO_SU_SI_ASSIGN with 
             * remove all to the SU. 
             */
            if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */                  
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            /* if the other SU is inservice with quiesced/standby assigned
             * send D2N-INFO_SU_SI_ASSIGN with active all to that SU. 
             * If the another SU in the operation list is OOS and assigned 
             * Send a D2N-INFO_SU_SI_ASSIGN with remove all to the SU.
             */
            o_su = AVD_SU_NULL;
            o_susi = AVD_SU_SI_REL_NULL;
            if (a_susi->su != su)
            {
               o_su = a_susi->su;
               o_susi = a_susi;
               m_AVD_CHK_OPLIST(o_su,flag);
            }else if ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su != su))
            {
               o_su = s_susi->su;
               o_susi = s_susi;
               m_AVD_CHK_OPLIST(o_su,flag);
            }
            
            if ((o_su != AVD_SU_NULL) && (flag == TRUE) &&
               (o_su->readiness_state == NCS_IN_SERVICE) &&
               ((o_susi->state == SA_AMF_HA_QUIESCED) ||
               (o_susi->state == SA_AMF_HA_STANDBY)) &&
               (o_susi->fsm == AVD_SU_SI_STATE_ASGND))
            {
               avd_sg_su_si_mod_snd(cb,o_su,SA_AMF_HA_ACTIVE);
            }else if ((o_su != AVD_SU_NULL) && (flag == TRUE) &&
               (o_su->readiness_state == NCS_OUT_OF_SERVICE))
            {
               as_flag = TRUE;
               i_susi = o_su->list_of_susi;
               while ((i_susi != AVD_SU_SI_REL_NULL) && 
                  (as_flag == TRUE))
               {
                  if (i_susi->fsm != AVD_SU_SI_STATE_UNASGN)
                     as_flag = FALSE;
                  
                  i_susi = i_susi->su_next;
               }
               
               if (as_flag == FALSE)
                  avd_sg_su_si_del_snd(cb,su);
            }
            
         } /* if (su->readiness_state == NCS_OUT_OF_SERVICE) */
         
      } /* if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED)) */
      else if (act == AVSV_SUSI_ACT_DEL)
      {
         o_su = AVD_SU_NULL;
         
         /* SU in the operation list and remove all */
         if (su->sg_of_su->su_oper_list.su != su)
         {
            o_su = su->sg_of_su->su_oper_list.su;
         }else if(su->sg_of_su->su_oper_list.next != AVD_SG_OPER_NULL)
         {
            o_su = su->sg_of_su->su_oper_list.next->su;
         }
         
         if (o_su != AVD_SU_NULL)
         {
            as_flag = FALSE;
            i_susi = o_su->list_of_susi;
            while (i_susi != AVD_SU_SI_REL_NULL)
            {
               if (i_susi->fsm != AVD_SU_SI_STATE_ASGND)
               {
                  as_flag = TRUE;
                  break;
               }
               i_susi = i_susi->su_next;
            }
            
            if (as_flag == TRUE)
            {
               /* another SU in the operation list with atleast one
                * SI assignment not in assigned state. Free all the 
                * SI assignments for the SU. Remove the SU from the
                * operation list.
                */
               
               avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
               if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
                  m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            }else if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                     (o_su->list_of_susi->state == SA_AMF_HA_ACTIVE))
            {
               /* another inservice SU has assignment all assigned active .
                * Free all the SI assignments for the SU.  Remove all the SUs
                * from the operation list.
                */
               avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
               while (su->sg_of_su->su_oper_list.su != AVD_SU_NULL)
               {                  
                  avd_sg_su_oper_list_del(cb,su->sg_of_su->su_oper_list.su,FALSE);
               }
               
               if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
               {
                  /* all the assignments have already been done in the SG. */
                  m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
                  m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
                  avd_sg_app_su_inst_func(cb,su->sg_of_su);
               }else
               {
                  /* Add the SU to the list  */
                  avd_sg_su_oper_list_add(cb,l_su,FALSE);
               }
               
            } /* if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                     (o_su->list_of_susi->state == SA_AMF_HA_ACTIVE)) */
            else if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                     (o_su->list_of_susi->state == SA_AMF_HA_QUIESCED) &&
                     (o_su->list_of_susi->fsm == AVD_SU_SI_STATE_ASGND))
            {
               avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
               if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
                  m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               avd_sg_su_si_mod_snd(cb,o_su,SA_AMF_HA_ACTIVE);
            } /* if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                     (o_su->list_of_susi->state == SA_AMF_HA_QUIESCED) &&
                     (o_su->list_of_susi->fsm == AVD_SU_SI_STATE_ASGND)) */
            
         } 
         else /* if (o_su != AVD_SU_NULL)*/
         {
            a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
            if (a_susi == AVD_SU_SI_REL_NULL)
            {
               o_su = AVD_SU_NULL;
               o_susi = a_susi;
            }
            else if (a_susi->su != su)
            {
               o_su = a_susi->su;
               o_susi = a_susi;
            }else if ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su != su))
            {
               o_su = s_susi->su;
               o_susi = s_susi;
            }
            
            if (o_su != AVD_SU_NULL)
            {
               if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                     (o_su->list_of_susi->state == SA_AMF_HA_ACTIVE))
               {
                  /* another inservice SU has assignment all assigned active .
                   * Free all the SI assignments for the SU.  Remove all the SUs
                   * from the operation list.
                   */
                  avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
                  while (su->sg_of_su->su_oper_list.su != AVD_SU_NULL)
                  {                  
                     avd_sg_su_oper_list_del(cb,su->sg_of_su->su_oper_list.su,FALSE);
                  }
                  
                  if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
                  {
                     /* all the assignments have already been done in the SG. */
                     m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
                     m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
                     avd_sg_app_su_inst_func(cb,su->sg_of_su);
                  }else
                  {
                     /* Add the SU to the list  */
                     avd_sg_su_oper_list_add(cb,l_su,FALSE);
                  }
                  
               } /* if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                        (o_su->list_of_susi->state == SA_AMF_HA_ACTIVE)) */
               else if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                        (o_su->list_of_susi->state == SA_AMF_HA_STANDBY))
               {
                  if (avd_sg_su_si_mod_snd(cb,o_su,SA_AMF_HA_ACTIVE) == NCSCC_RC_FAILURE)
                  {
                     /* log fatal error */                                          
                     m_AVD_LOG_INVALID_VAL_ERROR(((long)o_su));
                     m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(o_su->name_net.value,o_su->name_net.length);
                     return NCSCC_RC_FAILURE;
                  }
                  
                  avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
                  while (su->sg_of_su->su_oper_list.su != AVD_SU_NULL)
                  {                  
                     avd_sg_su_oper_list_del(cb,su->sg_of_su->su_oper_list.su,FALSE);
                  }
                  
                  avd_sg_su_oper_list_add(cb,o_su,FALSE);
               } /* if ((o_su->list_of_susi != AVD_SU_SI_REL_NULL) &&
                        (o_su->list_of_susi->state == SA_AMF_HA_STANDBY)) */
            }else /* if (o_su != AVD_SU_NULL) */
            { 
               /* no other SU has any assignments. */
               avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
               while (su->sg_of_su->su_oper_list.su != AVD_SU_NULL)
               {                  
                  avd_sg_su_oper_list_del(cb,su->sg_of_su->su_oper_list.su,FALSE);
               }
               
               if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
               {
                  /* all the assignments have already been done in the SG. */
                  m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
                  m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
                  avd_sg_app_su_inst_func(cb,su->sg_of_su);
               }else
               {
                  /* Add the SU to the list  */
                  avd_sg_su_oper_list_add(cb,l_su,FALSE);
               }
               
            } /* else (o_su != AVD_SU_NULL) */
            
         } /* else (o_su != AVD_SU_NULL)*/
         
      } /* if (act == AVSV_SUSI_ACT_DEL) */
      else if ((act == AVSV_SUSI_ACT_MOD) && ((state == SA_AMF_HA_ACTIVE) ||
         (state == SA_AMF_HA_STANDBY)))
      {
         /* active all or standby all. Remove the SU from the operation list. */
         if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
         
         
         if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
         {
            /* all the assignments have already been done in the SG. */
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }else
         {
            /* Add the SU to the list  */
            avd_sg_su_oper_list_add(cb,l_su,FALSE);
         }
      } /* if ((act == AVSV_SUSI_ACT_MOD) && ((state == SA_AMF_HA_ACTIVE) ||
         (state == SA_AMF_HA_STANDBY))) */
      else
      {
         /* Log a fatal error */
         m_AVD_LOG_INVALID_VAL_FATAL(act);
         m_AVD_LOG_INVALID_VAL_FATAL(state);
         m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
         return NCSCC_RC_FAILURE;
         
      }

   } /* if (susi == AVD_SU_SI_REL_NULL) */
   else 
   {
      /* assign single SUSI */
      if (act == AVSV_SUSI_ACT_DEL)
      {
         /* remove for single SU SI. Free the SUSI relationship. If all the 
          * relationships are in the assigned state, remove the SU from the 
          * operation list and do the realignment of the SG.
          */
         /* free all the CSI assignments  */
         avd_compcsi_list_del(cb,susi,FALSE);
         /* Unassign the SUSI */
         m_AVD_SU_SI_TRG_DEL(cb,susi);
         
         as_flag = FALSE;
         i_susi = su->list_of_susi;
         while (i_susi != AVD_SU_SI_REL_NULL)
         {
            if (i_susi->fsm != AVD_SU_SI_STATE_ASGND)
            {
               as_flag = TRUE;
               break;
            }
            i_susi = i_susi->su_next;
         }
         
         if (as_flag == FALSE)
         {
            /* All are assigned. Remove the SU from the operation list. */
            if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            
            if (su->sg_of_su->su_oper_list.su == AVD_SU_NULL)
            {
               if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
               {
                  /* all the assignments have already been done in the SG. */
                  m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
                  m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
                  avd_sg_app_su_inst_func(cb,su->sg_of_su);
               }else
               {
                  /* Add the SU to the list  */
                  avd_sg_su_oper_list_add(cb,l_su,FALSE);
               }
            }
         } /* if (as_flag == FALSE) */
         
      } /* if (act == AVSV_SUSI_ACT_DEL) */
      else if ((state == SA_AMF_HA_ACTIVE) || (state == SA_AMF_HA_STANDBY))
      {
         as_flag = FALSE;
         i_susi = su->list_of_susi;
         while (i_susi != AVD_SU_SI_REL_NULL)
         {
            if (i_susi->fsm != AVD_SU_SI_STATE_ASGND)
            {
               as_flag = TRUE;
               break;
            }
            i_susi = i_susi->su_next;
         }
         
         if (as_flag == FALSE)
         {
            /* All SUSI relationship are in assigned state. check for realiging
             * the SG.
             */
            /* All are assigned. Remove the SU from the operation list. */
            if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            
            if (su->sg_of_su->su_oper_list.su == AVD_SU_NULL)
            {
               if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
               {
                  /* all the assignments have already been done in the SG. */
                  m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
                  m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
                  avd_sg_app_su_inst_func(cb,su->sg_of_su);
               }else
               {
                  /* Add the SU to the list  */
                  avd_sg_su_oper_list_add(cb,l_su,FALSE);
               }
   
            }            
            
         } /* if (as_flag == FALSE) */
         
      } /* if ((state == SA_AMF_HA_ACTIVE) || (state == SA_AMF_HA_STANDBY)) */
      else
      {
         /* Log fatal error. */
         m_AVD_LOG_INVALID_VAL_FATAL(state);          
         m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
         return NCSCC_RC_FAILURE;
      }

   } /* else (susi == AVD_SU_SI_REL_NULL) */
   
   return NCSCC_RC_SUCCESS;
      
}

 /*****************************************************************************
 * Function: avd_sg_2n_susi_sucss_su_oper
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_susi_sucss_func.
 * It is called if the SG is in AVD_SG_FSM_SU_OPER.
 *
 * Input: cb - the AVD control block
 *        su - In case of entire SU related operation the SU for
 *               which the ack is received.
 *        susi - The pointer to the service unit service instance relationship.
 *        act  - The action received in the ack message.
 *        state - The HA state in the message.
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static uns32 avd_sg_2n_susi_sucss_su_oper(AVD_CL_CB *cb,AVD_SU *su,AVD_SU_SI_REL *susi,
                                AVSV_SUSI_ACT act, SaAmfHAStateT state)
{
   AVD_SU_SI_REL *s_susi, *a_susi;
   AVD_SU *l_su;
   NCS_BOOL flag;
   AVD_AVND *su_node_ptr = NULL;
   
   if(su->list_of_susi == AVD_SU_SI_REL_NULL)
      return NCSCC_RC_SUCCESS;

   
   
   if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
      (state == SA_AMF_HA_QUIESCED) &&
      (su->sg_of_su->su_oper_list.su == su))
   {
      /* quiesced all and SU is in the operation list */         

      a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
      
      if ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su->readiness_state == NCS_IN_SERVICE))
      {
         /* the standby SU is in-service. Send a D2N-INFO_SU_SI_ASSIGN with
          * modified active all to the standby SU.
          */
         if (avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)s_susi->su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(s_susi->su->name_net.value,s_susi->su->name_net.length);
            return NCSCC_RC_FAILURE;
         }          
           
      }else /* if ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su->readiness_state == NCS_IN_SERVICE)) */
      {
         /* Send a D2N-INFO_SU_SI_ASSIGN with remove all to the SU. 
          * Change state to SG_realign
          */
         
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* else ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su->readiness_state == NCS_IN_SERVICE)) */         

      m_AVD_GET_SU_NODE_PTR(cb,su,su_node_ptr);
      /* the admin state of the SU is shutdown change it to lock. */
      if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {               
         m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
      }else if (su_node_ptr->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {
         m_AVD_IS_NODE_LOCK((su_node_ptr),flag);
         if (flag == TRUE)
         {
            m_AVD_SET_AVND_SU_ADMIN(cb,(su_node_ptr),NCS_ADMIN_STATE_LOCK);
         }
      }
      
   } /* if ((susi == AVD_SU_SI_REL_NULL) && 
      (su->list_of_susi->state == SA_AMF_HA_QUIESCED) &&
      (su->sg_of_su->su_oper_list.su == su)) */
   else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL) && 
         (su->sg_of_su->su_oper_list.su != su))
   {
      /* delete all and SU is not in the operation list. Free the SUSI 
       * relationships for the SU. 
       */
      
      avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
      
   } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL) && 
         (su->sg_of_su->su_oper_list.su != su))*/
   else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL) && 
         (su->sg_of_su->su_oper_list.su == su))
   {
      /*remove all and SU is in the operation list */
      
      a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
      if (((a_susi->su == su) && 
         ((a_susi->state == SA_AMF_HA_QUIESCED) ||
         (a_susi->state == SA_AMF_HA_QUIESCING))) ||
         ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su == su) &&
         (s_susi->state == SA_AMF_HA_QUIESCED))) 
      {
         /*the SI relationships to the SU is quiesced or  quiescing */            
         
         if ((s_susi != AVD_SU_SI_REL_NULL) && 
            (s_susi->state == SA_AMF_HA_STANDBY) &&
            (s_susi->su->readiness_state == NCS_IN_SERVICE))
         {
            /* the standby SU is in-service. Send a D2N-INFO_SU_SI_ASSIGN with
             * modified active all to the standby SU.
             */
            if (avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)s_susi->su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(s_susi->su->name_net.value,s_susi->su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            /* Add that SU to operation list and Change state to SG_realign state. */
            avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);
            
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);  
         } /* if ((s_susi != AVD_SU_SI_REL_NULL) && 
            (s_susi->state == SA_AMF_HA_STANDBY) &&
            (s_susi->su->readiness_state == NCS_IN_SERVICE)) */
         else if((a_susi->state == SA_AMF_HA_ACTIVE) && (a_susi->su != su))
         {
            /* An in-service active assigning SU. Add that SU to 
             * operation list and Change state to SG_realign state.
             */
            avd_sg_su_oper_list_add(cb,a_susi->su,FALSE);
            
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }/* if((a_susi->state == SA_AMF_HA_ACTIVE) && (a_susi->su != su)) */
         else 
         {
            /* single SU that failed while taking quiesced or 
             * quiescing assignmenents. 
             */
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            
            /* realign the SG. */

         }
         
         m_AVD_GET_SU_NODE_PTR(cb,su,su_node_ptr);
         
         /* the admin state of the SU is shutdown change it to lock. */
         if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {               
            m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
         }else if (su_node_ptr->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {
            m_AVD_IS_NODE_LOCK((su_node_ptr),flag);
            if (flag == TRUE)
            {
               m_AVD_SET_AVND_SU_ADMIN(cb,(su_node_ptr),NCS_ADMIN_STATE_LOCK);
            }
         }else if (su->su_switch == AVSV_SI_TOGGLE_SWITCH)
         {
            /* this SU switch state is TRUE change to FALSE. */
            m_AVD_SET_SU_SWITCH(cb,su,AVSV_SI_TOGGLE_STABLE);
         }
         
         /* Remove the SU from operation list */
         if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
         
      } /* if (((a_susi->su == su) && 
         ((a_susi->state == SA_AMF_HA_QUIESCED) ||
         (a_susi->state == SA_AMF_HA_QUIESCING))) ||
         ((s_susi != AVD_SU_SI_REL_NULL) && (s_susi->su == su) &&
         (s_susi->state == SA_AMF_HA_QUIESCED))) */
      else
      {
         /* Log error remove shouldnt happen in this state other than 
          * for quiesced SU SI assignment.
          */
         m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
         m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
         return NCSCC_RC_FAILURE;
      }
      
      /* Free all the SI assignments to this SU. */ 
      avd_sg_su_asgn_del_util(cb,su, TRUE,FALSE);

      if (su->sg_of_su->sg_fsm_state == AVD_SG_FSM_STABLE)
      {
         if ((l_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) == AVD_SU_NULL)
         {
            /* all the assignments have already been done in the SG. */
            return NCSCC_RC_SUCCESS;
         }
         
         /* Add the SU to the list and change the FSM state */
         avd_sg_su_oper_list_add(cb,l_su,FALSE);

         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);         
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         
      }            
         
   } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL) && 
         (su->sg_of_su->su_oper_list.su == su))*/
   else if((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_ACTIVE))
   {
      /* The message is assign active all */
      
      if (su->sg_of_su->su_oper_list.su->su_switch == AVSV_SI_TOGGLE_SWITCH)
      {
         /* the SU in the operation list has admin operation switch Send a 
          * D2N-INFO_SU_SI_ASSIGN with modify all standby to the SU in
          * operation list, Change the switch state to false for the SU.
          * Change the state to SG_realign.
          */
         if (avd_sg_su_si_mod_snd(cb,su->sg_of_su->su_oper_list.su,SA_AMF_HA_STANDBY) 
            == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         m_AVD_SET_SU_SWITCH(cb,(su->sg_of_su->su_oper_list.su),AVSV_SI_TOGGLE_STABLE);
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      }else
      {
         /* Send a D2N-INFO_SU_SI_ASSIGN with remove all to the
          * SU in operation list. Change the state to SG_realign.
          */
         if (avd_sg_su_si_del_snd(cb,su->sg_of_su->su_oper_list.su) == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      }
   } /* if((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_ACTIVE)) */   
   
   return NCSCC_RC_SUCCESS;
}



 /*****************************************************************************
 * Function: avd_sg_2n_susi_sucss_si_oper
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_susi_sucss_func.
 * It is called if the SG is in AVD_SG_FSM_SI_OPER.
 *
 * Input: cb - the AVD control block
 *        su - In case of entire SU related operation the SU for
 *               which the ack is received.
 *        susi - The pointer to the service unit service instance relationship.
 *        act  - The action received in the ack message.
 *        state - The HA state in the message.
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static uns32 avd_sg_2n_susi_sucss_si_oper(AVD_CL_CB *cb,AVD_SU *su,AVD_SU_SI_REL *susi,
                                AVSV_SUSI_ACT act, SaAmfHAStateT state)
{
   AVD_SU_SI_REL *i_susi, *s_susi, *a_susi;
   AVD_SU_SI_STATE old_fsm_state;
   SaAmfHAStateT old_state;
   
   if (susi != AVD_SU_SI_REL_NULL)
   {
      if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED)
         && (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_LOCK))
      {
         /* the message with modified quiesced for a SI in the admin pointer
          * with admin values lock. Send a D2N-INFO_SU_SI_ASSIGN with remove
          * for this SI to this SU. Remove the SI from the admin pointer. Add
          * the SU to operation list also add the other SU to which this SI
          * has assignment to the operation list. Change state to SG_realign.
          */
         old_fsm_state = susi->fsm;
         susi->fsm = AVD_SU_SI_STATE_UNASGN;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
         if (avd_snd_susi_msg(cb,susi->su,susi,AVSV_SUSI_ACT_DEL)
            == NCSCC_RC_FAILURE)
         {
            /* Log a fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->su->name_net.value,susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->si->name_net.value,susi->si->name_net.length);
            susi->fsm = old_fsm_state;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
            return NCSCC_RC_FAILURE;
         }

         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         avd_sg_su_oper_list_add(cb,susi->su,FALSE);
         
         if (susi->si_next != AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_oper_list_add(cb,susi->si_next->su,FALSE);
         }
         
         if (susi->si->list_of_sisu != susi)
         {
            avd_sg_su_oper_list_add(cb,susi->si->list_of_sisu->su,FALSE);
         }
            
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED)
         && (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_LOCK)) */
      else if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED)
         && (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_SHUTDOWN))
      {
         /* message with modified quiesced for a SI in the admin pointer with 
          * admin values shutdown Send a D2N-INFO_SU_SI_ASSIGN with remove 
          * for this SI to this SU. Change admin state to lock for this SI. 
          * Identify its standby SU assignment send D2N-INFO_SU_SI_ASSIGN 
          * remove for only this SI. Remove the SI from the admin pointer. 
          *  Add the SU to operation list also add the other SU to which this
          * SI has assignment to the operation list. Change state to SG_realign.
          */
         
         old_fsm_state = susi->fsm;
         susi->fsm = AVD_SU_SI_STATE_UNASGN;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
         if (avd_snd_susi_msg(cb,susi->su,susi,AVSV_SUSI_ACT_DEL)
            == NCSCC_RC_FAILURE)
         {
            /* Log a fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->su->name_net.value,susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->si->name_net.value,susi->si->name_net.length);
            susi->fsm = old_fsm_state;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
            return NCSCC_RC_FAILURE;
         }
        
         m_AVD_SET_SI_ADMIN(cb,(susi->si),NCS_ADMIN_STATE_LOCK);
         
         avd_sg_su_oper_list_add(cb,susi->su,FALSE);
         
         if (susi->si_next != AVD_SU_SI_REL_NULL)
         {
            susi->si_next->fsm = AVD_SU_SI_STATE_UNASGN;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, (susi->si_next), AVSV_CKPT_AVD_SU_SI_REL);
            avd_snd_susi_msg(cb,susi->si_next->su,susi->si_next,AVSV_SUSI_ACT_DEL);
            avd_sg_su_oper_list_add(cb,susi->si_next->su,FALSE);
         }
         
         if (susi->si->list_of_sisu != susi)
         {
            susi->si->list_of_sisu->fsm = AVD_SU_SI_STATE_UNASGN;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, (susi->si->list_of_sisu), AVSV_CKPT_AVD_SU_SI_REL);
            avd_snd_susi_msg(cb,susi->si->list_of_sisu->su,susi->si->list_of_sisu,AVSV_SUSI_ACT_DEL);
            avd_sg_su_oper_list_add(cb,susi->si->list_of_sisu->su,FALSE);
         }
         
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         
      } /* if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED)
         && (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_SHUTDOWN))*/
      else if ((act == AVSV_SUSI_ACT_DEL) && (susi->state == SA_AMF_HA_STANDBY))
      {
         /* message with remove for a SU SI relationship having standby value.
          * Delete and free the SU SI relationship.
          */
                
         avd_compcsi_list_del(cb,susi,FALSE);
         m_AVD_SU_SI_TRG_DEL(cb,susi);
         
         if ((su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH) &&
            (su->list_of_susi->si == su->sg_of_su->admin_si) &&
            (su->list_of_susi->su_next == AVD_SU_SI_REL_NULL))
         {
            /* the admin pointer SI switch state is true and only the SI
             * admin list SI has standby relation to the SU.
             * Send D2N-INFO_SU_SI_ASSIGN with modified active assignment
             * to the SU.
             */
            su->list_of_susi->state = SA_AMF_HA_ACTIVE;
            su->list_of_susi->fsm = AVD_SU_SI_STATE_MODIFY;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, (su->list_of_susi), AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, su->list_of_susi);

            avd_snd_susi_msg(cb,su->list_of_susi->su,su->list_of_susi,AVSV_SUSI_ACT_MOD);
         }
         
      } /* if ((act == AVSV_SUSI_ACT_DEL) && (susi->state == SA_AMF_HA_STANDBY)) */
      else if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_ACTIVE) && 
               (susi->si == su->sg_of_su->admin_si) && 
               (susi->si->si_switch == AVSV_SI_TOGGLE_SWITCH))
      {
         /* message with modified active for a SI in the admin pointer with
          * switch value true.
          */
         s_susi = AVD_SU_SI_REL_NULL;
         if (susi->si_next != AVD_SU_SI_REL_NULL)
         {
            s_susi = susi->si_next;
         }else if (susi->si->list_of_sisu != susi)
         {
            s_susi = susi->si->list_of_sisu;
         }
         
         if ((s_susi != AVD_SU_SI_REL_NULL) && 
            ((s_susi->su_next != AVD_SU_SI_REL_NULL) ||
            (s_susi->su->list_of_susi != s_susi)))
         {
            /* Send D2N-INFO_SU_SI_ASSIGN with remove for each of the 
             * SIs except this SI to the quiesced SU.
             */
            i_susi = s_susi->su->list_of_susi;
            
            while (i_susi != AVD_SU_SI_REL_NULL)
            {
               if (i_susi != s_susi)
               {
                  i_susi->fsm = AVD_SU_SI_STATE_UNASGN;
                  m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, i_susi, AVSV_CKPT_AVD_SU_SI_REL);
                  avd_snd_susi_msg(cb,i_susi->su,i_susi,AVSV_SUSI_ACT_DEL);
               }
               i_susi = i_susi->su_next;
            }
         } /* if ((s_susi != AVD_SU_SI_REL_NULL) && 
            ((s_susi->su_next != AVD_SU_SI_REL_NULL) ||
            (s_susi->su->list_of_susi != s_susi)))*/
         else if (s_susi != AVD_SU_SI_REL_NULL)
         {
            /* Only the SI admin list SI has quiesced relation to the other SU 
             * Send D2N-INFO_SU_SI_ASSIGN with modified standby assignment to 
             * the SU. Change the SI switch state to false. Remove the SI from
             * the admin list. Add the SU to the operation list. 
             * Change state to SG_realign.
             */
            old_state = s_susi->state;
            old_fsm_state = s_susi->fsm;
            s_susi->state = SA_AMF_HA_STANDBY;
            s_susi->fsm = AVD_SU_SI_STATE_MODIFY;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, s_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, s_susi);
            if (avd_snd_susi_msg(cb,s_susi->su,s_susi,AVSV_SUSI_ACT_MOD)
               == NCSCC_RC_FAILURE)
            {
               /* Log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)s_susi));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(s_susi->su->name_net.value,s_susi->su->name_net.length);
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(s_susi->si->name_net.value,s_susi->si->name_net.length);
               s_susi->state = old_state;
               s_susi->fsm = old_fsm_state;
               m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, s_susi, AVSV_CKPT_AVD_SU_SI_REL);
               avd_gen_su_ha_state_changed_trap(cb, s_susi);
               return NCSCC_RC_FAILURE;
            }
            
            m_AVD_SET_SI_SWITCH(cb,(susi->si),AVSV_SI_TOGGLE_STABLE);
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
            avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            
         } /* if (s_susi != AVD_SU_SI_REL_NULL) */
         else
         {
            /* Integrity issue the standby is not there.*/
            m_AVD_LOG_INVALID_VAL_FATAL(((long)s_susi));
         }
         
      } /* if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_ACTIVE) && 
               (susi->si == su->sg_of_su->admin_si) && 
               (susi->si->si_switch == AVSV_SI_TOGGLE_SWITCH)) */
      else if ((act == AVSV_SUSI_ACT_DEL) && (susi->state == SA_AMF_HA_QUIESCED))
      {
         /* message with remove for a SU SI relationship having quiesced value
          * Delete and free the SU SI relationship.
          */
                
         avd_compcsi_list_del(cb,susi,FALSE);
         m_AVD_SU_SI_TRG_DEL(cb,susi);
         
         if ((su->list_of_susi->si == su->sg_of_su->admin_si) &&
            (su->list_of_susi->su_next == AVD_SU_SI_REL_NULL))
         {
            /* only the SI admin list SI has quiesced relation to the SU
             * Send D2N-INFO_SU_SI_ASSIGN with modified standby assignment 
             * to the SU. Change the SI switch state to false. Remove the 
             * SI from the admin list. Add the SU to the operation list. 
             * Change state to SG_realign.
             */
            su->list_of_susi->state = SA_AMF_HA_STANDBY;
            su->list_of_susi->fsm = AVD_SU_SI_STATE_MODIFY;            
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, su->list_of_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, su->list_of_susi);
            
            avd_snd_susi_msg(cb,su,su->list_of_susi,AVSV_SUSI_ACT_MOD);
            
            m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
            avd_sg_su_oper_list_add(cb,su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         }
         
      } /* if ((act == AVSV_SUSI_ACT_DEL) && (susi->state == SA_AMF_HA_QUIESCED)) */
      else
      {
         /* log error */
         m_AVD_LOG_INVALID_VAL_FATAL(((long)susi));
         m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
         m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(susi->si->name_net.value,susi->si->name_net.length);
         m_AVD_LOG_INVALID_VAL_FATAL(su->sg_of_su->sg_fsm_state);
      }

   }else /* if (susi != AVD_SU_SI_REL_NULL) */
   {
      if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED) && 
         (su->sg_of_su->admin_si != AVD_SI_NULL) && 
         (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH) &&
   ((a_susi = avd_su_susi_struc_find(cb,su,su->sg_of_su->admin_si->name_net,FALSE))
      != AVD_SU_SI_REL_NULL))
      {
         /* message with modified quiesced all and an SI is in the admin
          * pointer with switch value true
          */
         s_susi = AVD_SU_SI_REL_NULL;
         if (a_susi->si_next != AVD_SU_SI_REL_NULL)
         {
            s_susi = a_susi->si_next;
         }else if (a_susi->si->list_of_sisu != a_susi)
         {
            s_susi = a_susi->si->list_of_sisu;
         }
         
         if ((s_susi != AVD_SU_SI_REL_NULL) && 
            ((s_susi->su_next != AVD_SU_SI_REL_NULL) ||
            (s_susi->su->list_of_susi != s_susi)))
         {
            /* Send D2N-INFO_SU_SI_ASSIGN with remove for each of the 
             * SIs except this SI to the standby SU.
             */
            i_susi = s_susi->su->list_of_susi;
            
            while (i_susi != AVD_SU_SI_REL_NULL)
            {
               if (i_susi != s_susi)
               {
                  i_susi->fsm = AVD_SU_SI_STATE_UNASGN;
                  m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, i_susi, AVSV_CKPT_AVD_SU_SI_REL);
                  avd_snd_susi_msg(cb,i_susi->su,i_susi,AVSV_SUSI_ACT_DEL);
               }
               i_susi = i_susi->su_next;
            }
         } /* if ((s_susi != AVD_SU_SI_REL_NULL) && 
            ((s_susi->su_next != AVD_SU_SI_REL_NULL) ||
            (s_susi->su->list_of_susi != s_susi)))*/
         else if (s_susi != AVD_SU_SI_REL_NULL)
         {
            /* Only the SI admin pointer SI has standby relation to the SU.
             * Send D2N-INFO_SU_SI_ASSIGN with modified active assignment
             * to the SUSI.
             */
            old_state = s_susi->state;
            old_fsm_state = s_susi->fsm;
            s_susi->state = SA_AMF_HA_ACTIVE;
            s_susi->fsm = AVD_SU_SI_STATE_MODIFY;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, s_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, s_susi);
            if (avd_snd_susi_msg(cb,s_susi->su,s_susi,AVSV_SUSI_ACT_MOD)
               == NCSCC_RC_FAILURE)
            {
               /* Log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)s_susi));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(s_susi->su->name_net.value,s_susi->su->name_net.length);
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(s_susi->si->name_net.value,s_susi->si->name_net.length);
               s_susi->state = old_state;
               s_susi->fsm = old_fsm_state;
               m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, s_susi, AVSV_CKPT_AVD_SU_SI_REL);
               avd_gen_su_ha_state_changed_trap(cb, s_susi);
               return NCSCC_RC_FAILURE;
            }
            
         } /* if (s_susi != AVD_SU_SI_REL_NULL) */
         else
         {
            /* Integrity issue the standby is not there.*/
            m_AVD_LOG_INVALID_VAL_FATAL(((long)s_susi));
         }
          
      } /* if ((act == AVSV_SUSI_ACT_MOD) && (state == SA_AMF_HA_QUIESCED) && 
         (su->sg_of_su->admin_si != AVD_SI_NULL) && 
         (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH) &&
   ((a_susi = avd_su_susi_struc_find(cb,su,su->sg_of_su->admin_si->name_net,FALSE))
      != AVD_SU_SI_REL_NULL))*/
      else
      {
         /* log error */
         m_AVD_LOG_INVALID_VAL_FATAL(((long)su));
         m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
         m_AVD_LOG_INVALID_VAL_FATAL(su->sg_of_su->sg_fsm_state);
      }
   } /* else (susi != AVD_SU_SI_REL_NULL)*/
   

   return NCSCC_RC_SUCCESS;
}

 
/*****************************************************************************
 * Function: avd_sg_2n_susi_sucss_func
 *
 * Purpose:  This function is called when a SU SI ack function is
 * received from the AVND with success value. The SG FSM for 2N redundancy
 * model will be run. The SUSI fsm state will
 * be changed to assigned or it will freed for the SU SI. 
 * 
 *
 * Input: cb - the AVD control block
 *        su - In case of entire SU related operation the SU for
 *               which the ack is received.
 *        susi - The pointer to the service unit service instance relationship.
 *        act  - The action received in the ack message.
 *        state - The HA state in the message.
 * 
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_susi_sucss_func(AVD_CL_CB *cb,AVD_SU *su,AVD_SU_SI_REL *susi,
                                AVSV_SUSI_ACT act, SaAmfHAStateT state)
{

   AVD_SU_SI_REL *s_susi, *a_susi;
   AVD_SU *i_su, *a_su;

   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_susi_sucss_func");
   m_AVD_LOG_RCVD_VAL(((long)su));
   m_AVD_LOG_RCVD_VAL(((long)susi));   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   
   switch(su->sg_of_su->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      /* Do the action specified in the message if delete else no action. */
      if (act == AVSV_SUSI_ACT_DEL)
      {
         if (susi == AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_asgn_del_util(cb, su, TRUE,FALSE);            
         }else
         {
            /* free all the CSI assignments  */
            avd_compcsi_list_del(cb,susi,FALSE);
            /* Unassign the SUSI */
            m_AVD_SU_SI_TRG_DEL(cb,susi);
         }
      }      
      /* log informational error. */
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SG_REALIGN:      
   
      if (avd_sg_2n_susi_sucss_sg_reln(cb,su,susi,act,state) == NCSCC_RC_FAILURE)
         return NCSCC_RC_FAILURE;
      
      break; /* case AVD_SG_FSM_SG_REALIGN: */
   case AVD_SG_FSM_SU_OPER:
         
      if (avd_sg_2n_susi_sucss_su_oper(cb,su,susi,act,state) == NCSCC_RC_FAILURE)
         return NCSCC_RC_FAILURE;
      
      break; /* case AVD_SG_FSM_SU_OPER: */
   case AVD_SG_FSM_SI_OPER:

      if (avd_sg_2n_susi_sucss_si_oper(cb,su,susi,act,state) == NCSCC_RC_FAILURE)
         return NCSCC_RC_FAILURE;
      
      break; /* case AVD_SG_FSM_SI_OPER: */
   case AVD_SG_FSM_SG_ADMIN:
   
      if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK)
      {
         /* SG is admin lock */
         if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
            (state == SA_AMF_HA_QUIESCED))
         {
            /* quiesced all Send a D2N-INFO_SU_SI_ASSIGN with remove all for this SU. */
            if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
         }else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL))
         {
            /* Free all the SI assignments to this SU */
            avd_sg_su_asgn_del_util(cb,su, TRUE,FALSE);
            i_su = su->sg_of_su->list_of_su;
            while (i_su != AVD_SU_NULL)
            {
               if (i_su->list_of_susi != AVD_SU_SI_REL_NULL)
               {
                  /* found a assigned su break*/
                  break;
               }
               i_su = i_su->sg_list_su_next;
            }
            
            if (i_su == AVD_SU_NULL)
            {
               /* the SG doesnt have any SI assignments.Change state to stable. */
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
         } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL)) */
         else
         {
            /* log fatal error */            
            m_AVD_LOG_INVALID_VAL_FATAL(((long)su));
            m_AVD_LOG_INVALID_VAL_FATAL(((uns32)act));
            m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
      } /* if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK) */
      else if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {
         /* SG is admin shutdown */
         if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
            (state == SA_AMF_HA_QUIESCED))
         {
            /* quiesced all. Change admin state to lock and identify its 
             * standby SU assignment send D2N-INFO_SU_SI_ASSIGN remove all
             * for the SU. Send a D2N-INFO_SU_SI_ASSIGN with remove all for
             * this SU.
             */
            a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
            if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */           
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            if (s_susi != AVD_SU_SI_REL_NULL)
            {   
               avd_sg_su_si_del_snd(cb,s_susi->su);
            }
            m_AVD_SET_SG_ADMIN(cb,(su->sg_of_su),NCS_ADMIN_STATE_LOCK);
            a_su = su->sg_of_su->list_of_su;
            while (a_su != AVD_SU_NULL)
            {
               m_AVD_SET_SU_REDINESS(cb,a_su,NCS_OUT_OF_SERVICE);
               a_su = a_su->sg_list_su_next;
            }
         } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
            (state == SA_AMF_HA_QUIESCED))*/
         else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_DEL))
         {
            /* remove all. Free all the SI assignments to this SU */
            avd_sg_su_asgn_del_util(cb,su, TRUE,FALSE);
         }else
         {
            /* log a fatal error */           
            m_AVD_LOG_INVALID_VAL_FATAL(((long)su));
            m_AVD_LOG_INVALID_VAL_FATAL(((uns32)act));
            m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
      } /* if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_SHUTDOWN) */

      break; /* case AVD_SG_FSM_SG_ADMIN: */   
   default:
      /* log fatal error about the invalid value */
      m_AVD_LOG_INVALID_VAL_FATAL(((long)su));
      m_AVD_LOG_INVALID_VAL_FATAL(((uns32)su->sg_of_su->sg_fsm_state));
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
      return NCSCC_RC_FAILURE;
      break;
   } /* switch(su->sg_of_su->sg_fsm_state) */

   return NCSCC_RC_SUCCESS;
} 
 
/*****************************************************************************
 * Function: avd_sg_2n_susi_fail_func
 *
 * Purpose:  This function is called when a SU SI ack function is
 * received from the AVND with some error value. The message may be an
 * ack for a particular SU SI or for the entire SU. It will log an event
 * about the failure. Since if a CSI set callback returns error it is
 * considered as failure of the component, AvND would have updated that
 * info for each of the components that failed and also for the SU an
 * operation state message would be sent the processing will be done in that
 * event context. For faulted SU this event would be considered as
 * completion of action, for healthy SU no SUSI state change will be done. 
 * 
 *
 * Input: cb - the AVD control block
 *        su - In case of entire SU related operation the SU for
 *               which the ack is received.
 *        susi - The pointer to the service unit service instance relationship.
 *        act  - The action received in the ack message.
 *        state - The HA state in the message.
 *        
 *
 * Returns:  NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_susi_fail_func(AVD_CL_CB *cb,AVD_SU *su,AVD_SU_SI_REL *susi,
                                AVSV_SUSI_ACT act, SaAmfHAStateT state)
{
   AVD_SU_SI_REL *s_susi, *o_susi, *a_susi;
   AVD_SU *a_su;
   AVD_SU_SI_STATE old_fsm_state;
   NCS_BOOL flag;
   AVD_AVND *su_node_ptr = NULL;

   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_susi_fail_func");
   m_AVD_LOG_RCVD_VAL(((long)su));
   m_AVD_LOG_RCVD_VAL(((long)susi));   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   
   switch(su->sg_of_su->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      /* Do the action specified in the message if delete else no action. */
      if (act == AVSV_SUSI_ACT_DEL)
      {
         if (susi == AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_asgn_del_util(cb, su, TRUE,FALSE);
         }else
         {
            /* free all the CSI assignments  */
            avd_compcsi_list_del(cb,susi,FALSE);
            /* Unassign the SUSI */
            m_AVD_SU_SI_TRG_DEL(cb,susi);
         }
      }

      /* log fatal error. */      
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SG_REALIGN:
      
      if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (su->sg_of_su->su_oper_list.su == su)
         && (su->readiness_state == NCS_OUT_OF_SERVICE))
      {
         /* quiesced all and SU is OOS. Send a D2N-INFO_SU_SI_ASSIGN remove
          * all to the SU.
          */
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* Log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
      }else
      {
         /* No action as other call back failure will cause operation disable 
          * event to be sent by AvND.
          */
         m_AVD_LOG_RCVD_VAL(((long)su));
      }
      
      break; /* case AVD_SG_FSM_SG_REALIGN: */
   case AVD_SG_FSM_SU_OPER:
      if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (su->su_switch == AVSV_SI_TOGGLE_SWITCH)
         && (su->sg_of_su->su_oper_list.su == su))
      {
         /* quiesced all and SU is in the operation list and the SU switch
          * state is true. Change the SU switch state to false. Send a 
          * D2N-INFO_SU_SI_ASSIGN with modified active all for the SU. 
          * Change the state to SG_realign.
          */
         if (avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_ACTIVE) 
            == NCSCC_RC_FAILURE)
         {
            /* log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SU_SWITCH(cb,su,AVSV_SI_TOGGLE_STABLE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (su->su_switch == AVSV_SI_TOGGLE_SWITCH)
         (su->sg_of_su->su_oper_list.su == su)) */
      else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         ((state == SA_AMF_HA_QUIESCED) || (state == SA_AMF_HA_QUIESCING)) &&
         (su->sg_of_su->su_oper_list.su == su))
      {
         /* quiesced/quiescing all and SU is in the operation list and the 
          * SU admin state is lock/shutdown. Change the SU admin state to lock.
          * Send a D2N-INFO_SU_SI_ASSIGN with remove all for the SU. Stay in the
          * same state. 
          */
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* Log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_GET_SU_NODE_PTR(cb,su,su_node_ptr);
         
         /* the admin state of the SU is shutdown change it to lock. */
         if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {               
            m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
         }else if (su_node_ptr->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {
            m_AVD_IS_NODE_LOCK((su_node_ptr),flag);
            if (flag == TRUE)
            {
               m_AVD_SET_AVND_SU_ADMIN(cb,(su_node_ptr),NCS_ADMIN_STATE_LOCK);
            }
         }
      } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         ((state == SA_AMF_HA_QUIESCED) || (state == SA_AMF_HA_QUIESCING)) &&
         (su->sg_of_su->su_oper_list.su == su))*/
      else
      {
         /* Other cases log a informational message. AvND would treat that as
          * a operation disable for the SU and cause the SU to go OOS, which
          * will trigger the FSM.
          */
         
         m_AVD_LOG_RCVD_VAL(((long)su));
      }

      break; /* case AVD_SG_FSM_SU_OPER: */
   case AVD_SG_FSM_SI_OPER:
      
      if ((susi != AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_LOCK))
      {
         /* message with modified quiesced for a SI in the admin pointer with 
          * admin values lock. Send a D2N-INFO_SU_SI_ASSIGN with remove for 
          * this SI to this SU. Remove the SI from the admin pointer. Add the 
          * SU to operation list also add the other SU to which this SI has 
          * assignment to the operation list. Change state to SG_realign.
          */         
         
         old_fsm_state = susi->fsm;
         susi->fsm = AVD_SU_SI_STATE_UNASGN;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
         if (avd_snd_susi_msg(cb,susi->su,susi,AVSV_SUSI_ACT_DEL)
            == NCSCC_RC_FAILURE)
         {
            /* Log a fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->su->name_net.value,susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->si->name_net.value,susi->si->name_net.length);
            susi->fsm = old_fsm_state;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         avd_sg_su_oper_list_add(cb,susi->su,FALSE);
         
         o_susi = AVD_SU_SI_REL_NULL;
         
         if (susi->si_next != AVD_SU_SI_REL_NULL)
         {
            o_susi = susi->si_next;
         }else if (susi->si->list_of_sisu != susi)
         {
            o_susi =  susi->si->list_of_sisu;
         }
         
         if (o_susi != AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_oper_list_add(cb,o_susi->su,FALSE);
         }
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((susi != AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_LOCK)) */
      else if ((susi != AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         ((state == SA_AMF_HA_QUIESCED) || (state == SA_AMF_HA_QUIESCING)) && 
         (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_SHUTDOWN))
      {
         /* message with modified quiescing/quiesced for a SI in the  
          * admin pointer with admin values shutdown. 
          * Send a D2N-INFO_SU_SI_ASSIGN with remove for this SI to this SU.   
          * Change admin state to lock for this SI. Identify its standby 
          * SU assignment send D2N-INFO_SU_SI_ASSIGN remove for only this SI. 
          * Remove the SI from the admin pointer. Add the SU to operation list 
          * also add the other SU to which this SI has assignment to the 
          * operation list. Change state to SG_realign.  
          */
         
         old_fsm_state = susi->fsm;
         susi->fsm = AVD_SU_SI_STATE_UNASGN;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, susi, AVSV_CKPT_AVD_SU_SI_REL);
         if (avd_snd_susi_msg(cb,susi->su,susi,AVSV_SUSI_ACT_DEL)
            == NCSCC_RC_FAILURE)
         {
            /* Log a fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->su->name_net.value,susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(susi->si->name_net.value,susi->si->name_net.length);
            susi->fsm = old_fsm_state;
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SI_ADMIN(cb,(susi->si),NCS_ADMIN_STATE_LOCK);
         
         avd_sg_su_oper_list_add(cb,susi->su,FALSE);
         
         if (susi->si_next != AVD_SU_SI_REL_NULL)
         {
            susi->si_next->fsm = AVD_SU_SI_STATE_UNASGN;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, (susi->si_next), AVSV_CKPT_AVD_SU_SI_REL);
            avd_snd_susi_msg(cb,susi->si_next->su,susi->si_next,AVSV_SUSI_ACT_DEL);
            avd_sg_su_oper_list_add(cb,susi->si_next->su,FALSE);
         }
         
         if (susi->si->list_of_sisu != susi)
         {
            susi->si->list_of_sisu->fsm = AVD_SU_SI_STATE_UNASGN;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, (susi->si->list_of_sisu), AVSV_CKPT_AVD_SU_SI_REL);
            avd_snd_susi_msg(cb,susi->si->list_of_sisu->su,susi->si->list_of_sisu,AVSV_SUSI_ACT_DEL);
            avd_sg_su_oper_list_add(cb,susi->si->list_of_sisu->su,FALSE);
         }
         
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         
      } /* if ((susi != AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         ((state == SA_AMF_HA_QUIESCED) || (state == SA_AMF_HA_QUIESCING)) && 
         (susi->si == su->sg_of_su->admin_si)
         && (susi->si->admin_state == NCS_ADMIN_STATE_SHUTDOWN)) */
      else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (su->sg_of_su->admin_si != AVD_SI_NULL)
         && (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH) &&
((a_susi = avd_su_susi_struc_find(cb,su,su->sg_of_su->admin_si->name_net,FALSE))
      != AVD_SU_SI_REL_NULL))
      {
         /* Message with modified quiesced all and an SI is in the admin 
          * pointer with switch value true.  Send D2N-INFO_SU_SI_ASSIGN with 
          * modified active all to the SU. Change the switch value of the SI .
          * to false. Remove the SI from the admin pointer. Add the SU to 
          * operation list. Change state to SG_realign.
          */
         
         
         if (avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_ACTIVE) 
            == NCSCC_RC_FAILURE)
         {
            /* log fatal error */            
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         avd_sg_su_oper_list_add(cb,su,FALSE);
         
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
          
      } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && (su->sg_of_su->admin_si != AVD_SI_NULL)
         && (su->sg_of_su->admin_si == AVSV_SI_TOGGLE_SWITCH) &&
((a_susi = avd_su_susi_struc_find(cb,su,su->sg_of_su->admin_si->name_net,FALSE))
      != AVD_SU_SI_REL_NULL)) */
      else 
      {
         /* message with remove for a SU SI relationship having standby value.
          * message with modified active for a SI in the admin pointer with
          * switch value true; message with remove for a SU SI relationship
          * having quiesced value. Ignore this message. It will trigger the
          * SU to go disabled and an operation state message will be received
          * for the SU.
          */
         m_AVD_LOG_RCVD_VAL(((long)su));
      } 

      break; /* case AVD_SG_FSM_SI_OPER: */
   case AVD_SG_FSM_SG_ADMIN:
      
      if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && 
         (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK))
      {
         /* SG is admin lock. Message is quiesced all. 
          * Send a D2N-INFO_SU_SI_ASSIGN with remove all for this SU.
          */
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* Log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
      } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         (state == SA_AMF_HA_QUIESCED) && 
         (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK))*/
      else if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         ((state == SA_AMF_HA_QUIESCED) || (state == SA_AMF_HA_QUIESCING))&& 
         (su->sg_of_su->admin_state == NCS_ADMIN_STATE_SHUTDOWN))
      {
         /* SG is admin shutdown. The message is quiescing all or quiesced all.
          * Change admin state to lock and identify its standby SU assignment 
          * send D2N-INFO_SU_SI_ASSIGN remove all for the SU. 
          * Send a D2N-INFO_SU_SI_ASSIGN with remove all for this SU.
          */
         a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
         
         if (avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
         {
            /* Log fatal error */
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_si_del_snd(cb,su);
         }
         m_AVD_SET_SG_ADMIN(cb,(su->sg_of_su),NCS_ADMIN_STATE_LOCK);
         a_su = su->sg_of_su->list_of_su;
         while (a_su != AVD_SU_NULL)
         {
            m_AVD_SET_SU_REDINESS(cb,a_su,NCS_OUT_OF_SERVICE);
            a_su = a_su->sg_list_su_next;
         }
         
      } /* if ((susi == AVD_SU_SI_REL_NULL) && (act == AVSV_SUSI_ACT_MOD) &&
         ((state == SA_AMF_HA_QUIESCED) && 
         (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK))*/
      else
      {
         /* Other cases log a informational message. AvND would treat that as
          * a operation disable for the SU and cause the SU to go OOS, which
          * will trigger the FSM.
          */
         m_AVD_LOG_RCVD_VAL(((long)su));
      }

      break; /* case AVD_SG_FSM_SG_ADMIN: */   
   default:
      /* log fatal error about the invalid value */      
      m_AVD_LOG_INVALID_VAL_FATAL(((long)su));
      m_AVD_LOG_INVALID_VAL_FATAL(((uns32)su->sg_of_su->sg_fsm_state));
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
      return NCSCC_RC_FAILURE;
      break;
   } /* switch(su->sg_of_su->sg_fsm_state) */
   
   return NCSCC_RC_SUCCESS;

} 



 /*****************************************************************************
 * Function: avd_sg_2n_realign_func
 *
 * Purpose:  This function will call the chose assign function to check and
 * assign SIs. If any assigning is being done it adds the SU to the operation
 * list and sets the SG FSM state to SG realign. It resets the ncsSGAdjustState.
 * If everything is 
 * fine, it calls the routine to bring the preffered number of SUs to 
 * inservice state and change the SG state to stable. The functionality is
 * described in the SG FSM. The same function is used for both cluster_timer and
 * and sg_operator events as described in the SG FSM.
 *
 * Input: cb - the AVD control block
 *        sg - The pointer to the service group.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: none.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_realign_func(AVD_CL_CB *cb,AVD_SG *sg)
{
   AVD_SU *l_su;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_realign_func");
   m_AVD_LOG_RCVD_VAL(((long)sg));
   
   m_AVD_LOG_RCVD_VAL(sg->sg_fsm_state);
   /* If the SG FSM state is not stable just return success. */   
   
   if((cb->init_state != AVD_APP_STATE) &&
      (sg->sg_ncs_spec == SA_FALSE))
   {
      return NCSCC_RC_SUCCESS;
   }
   
   if (sg->sg_fsm_state != AVD_SG_FSM_STABLE)
   {
      m_AVD_SET_SG_ADJUST(cb,sg,AVSV_SG_STABLE);      
      avd_sg_app_su_inst_func(cb, sg);
      return NCSCC_RC_SUCCESS;
   }
   
   if ((l_su = avd_sg_2n_su_chose_asgn(cb,sg)) == AVD_SU_NULL)
   {
      /* all the assignments have already been done in the SG. */
      m_AVD_SET_SG_ADJUST(cb,sg,AVSV_SG_STABLE);
      avd_sg_app_su_inst_func(cb, sg);
      return NCSCC_RC_SUCCESS;
   }
   
   /* Add the SU to the list and change the FSM state */
   avd_sg_su_oper_list_add(cb,l_su,FALSE);
   
   m_AVD_SET_SG_ADJUST(cb,sg,AVSV_SG_STABLE);
   
   m_AVD_SET_SG_FSM(cb,sg,AVD_SG_FSM_SG_REALIGN);
   m_AVD_LOG_RCVD_VAL(sg->sg_fsm_state);
   
   return NCSCC_RC_SUCCESS;
}


/*****************************************************************************
 * Function: avd_sg_2n_node_fail_su_oper
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_node_fail_func.
 * It is called if the SG is in AVD_SG_FSM_SU_OPER.
 *
 * Input: cb - the AVD control block
 *        su - The SU that has faulted because of the node failure.
 *        
 *
 * Returns: None.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static void avd_sg_2n_node_fail_su_oper(AVD_CL_CB *cb,AVD_SU *su)
{
   AVD_SU_SI_REL *a_susi, *s_susi;
   AVD_SU *o_su;
   NCS_BOOL flag;
   AVD_AVND *su_node_ptr = NULL;

   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_node_fail_su_oper");
   
   if(su->sg_of_su->su_oper_list.su == su)
   {
      /* the SU is same as the SU in the list */
      a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
      
      if (((su->list_of_susi->state == SA_AMF_HA_QUIESCED) &&
         (su->list_of_susi->fsm == AVD_SU_SI_STATE_MODIFY)) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCING))
      {  

         /* the SI relationships to the SU is quiesced assigning. If this 
          * SU admin is shutdown change to LOCK. If this SU switch state
          * is TRUE change to FALSE. Remove the SU from operation list
          */            
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            if (s_susi->su->readiness_state == NCS_IN_SERVICE)
            {
               /* an in-service standby SU. Send D2N-INFO_SU_SI_ASSIGN 
                * modify active all. Add that SU to the operation list.
                * Change state to SG_realign state. Free all the SI 
                * assignments to this SU.
                */
               avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);
               
            }
            
            if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);            
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }else /* if (s_susi != AVD_SU_SI_REL_NULL) */
         {
            if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
            {
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,o_su,FALSE);
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
         } /* else (s_susi != AVD_SU_SI_REL_NULL) */
            
         m_AVD_GET_SU_NODE_PTR(cb,su,su_node_ptr);
         /* the admin state of the SU is shutdown change it to lock. */
         if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {               
            m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
         }else if (su_node_ptr->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {
            m_AVD_IS_NODE_LOCK((su_node_ptr),flag);
            if (flag == TRUE)
            {
               m_AVD_SET_AVND_SU_ADMIN(cb,(su_node_ptr),NCS_ADMIN_STATE_LOCK);
            }
         }else
         {
            m_AVD_SET_SU_SWITCH(cb,su,AVSV_SI_TOGGLE_STABLE);
         }            
         
      }/* if (((su->list_of_susi->state == SA_AMF_HA_QUIESCED) &&
         (su->list_of_susi->fsm == AVD_SU_SI_STATE_MODIFY)) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCING)) */
      else if (a_susi->su != su)
      {
         /* the SI relationships to the SU is quiesced assigned and the
          * other SU is being modified to Active. If this 
          * SU admin is shutdown change to LOCK. If this SU switch state
          * is TRUE change to FALSE. Remove the SU from operation list.
          * Add that SU to the operation list . Change state to 
          * SG_realign state. Free all the SI assignments to this SU.
          */

         if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);

         m_AVD_GET_SU_NODE_PTR(cb,su,su_node_ptr);
         
         /* the admin state of the SU is shutdown change it to lock. */
         if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {               
            m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
         }else if (su_node_ptr->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)
         {
            m_AVD_IS_NODE_LOCK((su_node_ptr),flag);
            if (flag == TRUE)
            {
               m_AVD_SET_AVND_SU_ADMIN(cb,(su_node_ptr),NCS_ADMIN_STATE_LOCK);
            }
         }else
         {
            m_AVD_SET_SU_SWITCH(cb,su,AVSV_SI_TOGGLE_STABLE);
         }
         
         avd_sg_su_oper_list_add(cb,a_susi->su,FALSE);   
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);                   
      } /* if (a_susi->su != su) */
     else
     {
        if (s_susi != AVD_SU_SI_REL_NULL)
        {
          if (s_susi->su->readiness_state == NCS_IN_SERVICE)
          {
               /* an in-service standby SU. Send D2N-INFO_SU_SI_ASSIGN 
                * modify active all. Add that SU to the operation list.
                * Change state to SG_realign state. Free all the SI 
                * assignments to this SU.
                */
               avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);
               
           }
            
            if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);            
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
        }
       else /* if (s_susi != AVD_SU_SI_REL_NULL) */
       {
          if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
       }
     }
   }else /* if(su->sg_of_su->su_oper_list.su == su) */
   {         
      /* the SU is not the same as the SU in the list */         
      if(su->list_of_susi->state == SA_AMF_HA_STANDBY)
      {
         /* the SU has standby SI assignments. if the other SUs switch field
          * is TRUE, it is in service, having quiesced assigning state.
          * Send D2N-INFO_SU_SI_ASSIGN modify active all to the other SU.
          * Change switch field to false. Change state to SG_realign. 
          * Free all the SI assignments to this SU.
          */
         if ((su->sg_of_su->su_oper_list.su->su_switch == AVSV_SI_TOGGLE_SWITCH)
             && (su->sg_of_su->su_oper_list.su->readiness_state ==
               NCS_IN_SERVICE))
         {         
           
            avd_sg_su_si_mod_snd(cb,su->sg_of_su->su_oper_list.su,SA_AMF_HA_ACTIVE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);            
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }
         
         m_AVD_SET_SU_SWITCH(cb,(su->sg_of_su->su_oper_list.su),AVSV_SI_TOGGLE_STABLE);
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            
      }/* if(su->list_of_susi->state == SA_AMF_HA_STANDBY) */
      else if (su->list_of_susi->state == SA_AMF_HA_ACTIVE)
      {
         /* the SU has active SI assignments. The other SUs switch field 
          * is TRUE, it is in service, having quiesced assigned state.
          * Send D2N-INFO_SU_SI_ASSIGN modify active all to the other SU. 
          * Change switch field to false. Change state to SG_realign. 
          * Free all the SI assignments to this SU.
          */
         
         if ((su->sg_of_su->su_oper_list.su->su_switch == AVSV_SI_TOGGLE_SWITCH)
             && (su->sg_of_su->su_oper_list.su->readiness_state ==
               NCS_IN_SERVICE))
         {         
           
            avd_sg_su_si_mod_snd(cb,su->sg_of_su->su_oper_list.su,SA_AMF_HA_ACTIVE);

            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }else
         {
            avd_sg_su_si_del_snd(cb,su->sg_of_su->su_oper_list.su);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }
         
         m_AVD_SET_SU_SWITCH(cb,(su->sg_of_su->su_oper_list.su),AVSV_SI_TOGGLE_STABLE);
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);

      } /* if (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      
   } /* else (su->sg_of_su->su_oper_list.su == su) */
   
   return;

}


/*****************************************************************************
 * Function: avd_sg_2n_node_fail_si_oper
 *
 * Purpose:  This function is a subfunctionality of avd_sg_2n_node_fail_func.
 * It is called if the SG is in AVD_SG_FSM_SI_OPER.
 *
 * Input: cb - the AVD control block
 *        su - The SU that has faulted because of the node failure.
 *        
 *
 * Returns: None.
 *
 * NOTES: None.
 *
 * 
 **************************************************************************/

static void avd_sg_2n_node_fail_si_oper(AVD_CL_CB *cb,AVD_SU *su)
{
   AVD_SU_SI_REL *a_susi, *s_susi;
   AVD_SU *o_su;

   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_node_fail_si_oper");
   
   if (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH)
   {
      /* the SI switch operation is TRUE */
      a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
      
      if ((a_susi->su == su) && (a_susi->state == SA_AMF_HA_QUIESCED))
      {
         /* This SI relation with the SU is quiesced assigning or this SI 
          * relation with the SU is quiesced assigned and the other SU is
          * standby assigned, other SIs wrt to the SUs are being removed.
          * Change the SI switch state to false. Remove the SI from the SI
          * admin pointer. Mark all the SI relationships to this SU as
          * unassigning. 
          */
         
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         if (s_susi->su->readiness_state == NCS_IN_SERVICE)
         {
            /* in-service standby SU. Send D2N-INFO_SU_SI_ASSIGN modify
             * active all. Add that SU to operation list. Change state 
             * to SG_realign state. Free all the SI assignments to this SU.
             */
            avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);               

         }
         
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((a_susi->su == su) && (a_susi->state == SA_AMF_HA_QUIESCED)) */
      else if ((a_susi->su != su) && (a_susi->state == SA_AMF_HA_ACTIVE) &&
               (a_susi->fsm != AVD_SU_SI_STATE_ASGND))
      {
         /* this SI relation with the SU is quiesced assigned and the other
          * SU is active assigning. 
          * Change the SI switch state to false. Remove the SI from the
          * SI admin pointer. Mark all the SI relationships to this SU as
          * unassigning. Add the other SU to operation list. Change 
          * state to SG_realign state. Free all the SI assignments to this SU.
          */
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         avd_sg_su_oper_list_add(cb,a_susi->su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((a_susi->su != su) && (a_susi->state == SA_AMF_HA_ACTIVE) &&
               (a_susi->fsm != AVD_SU_SI_STATE_ASGND)) */
      else if ((a_susi->su != su) && (a_susi->state == SA_AMF_HA_ACTIVE))
      {
         /* This SI relation with the SU is quiesced assigned and the other
          * SU is active assigned. Change the SI switch state to false. 
          * Remove the SI from the SI admin pointer. Free all the 
          * SI assignments to this SU. Do the realignment of the SG.
          */
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
         {
            /* add the SU to the operation list and change the SG FSM to SG realign. */
            avd_sg_su_oper_list_add(cb,o_su,FALSE); 
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }
      } /* if ((a_susi->su != su) && (a_susi->state == SA_AMF_HA_ACTIVE)) */
      else if ((s_susi->su == su) && (s_susi->state == SA_AMF_HA_STANDBY))
      {
         /* This SI relation with the SU is standby assigned and the
          * other SU is quiesced assigned/assigning. Send 
          * D2N-INFO_SU_SI_ASSIGN with modified active all to the other SU,
          * if inservice. Change the switch value of the SI to false. Remove
          * the SI from the admin pointer. Add that SU to operation list. 
          * Change state to SG_realign. Free all the SI assignments to
          * this SU.
          */
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         if (a_susi->su->readiness_state == NCS_IN_SERVICE)
         {
            /* in-service quiesced SU. Send D2N-INFO_SU_SI_ASSIGN modify
             * active all. Add that SU to operation list. Change state 
             * to SG_realign state. Free all the SI assignments to this SU.
             */
            avd_sg_su_si_mod_snd(cb,a_susi->su,SA_AMF_HA_ACTIVE);               

         }
         
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         avd_sg_su_oper_list_add(cb,a_susi->su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((s_susi->su == su) && (s_susi->state == SA_AMF_HA_STANDBY)) */
      else if ((a_susi->su == su) && (a_susi->state == SA_AMF_HA_ACTIVE))
      {
         /* This SI relation with the SU is active assigning and the other
          * SU is quiesced assigned.  Change the switch value of the SI to
          * false. Remove the SI from the admin pointer. 
          * Send D2N-INFO_SU_SI_ASSIGN with modified active all to the 
          * other SU. Add that SU to operation list. Change state to 
          * SG_realign. Free all the SI assignments to this SU.
          */
         m_AVD_SET_SI_SWITCH(cb,(su->sg_of_su->admin_si),AVSV_SI_TOGGLE_STABLE);
         m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
         if (s_susi->su->readiness_state == NCS_IN_SERVICE)
         {
            /* in-service quiesced SU. Send D2N-INFO_SU_SI_ASSIGN modify
             * active all. Add that SU to operation list. Change state 
             * to SG_realign state. Free all the SI assignments to this SU.
             */
            avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);               

         }
         
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);
         m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
         m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
      } /* if ((a_susi->su == su) && (a_susi->state == SA_AMF_HA_ACTIVE)) */
      
   } /* if (su->sg_of_su->admin_si->si_switch == AVSV_SI_TOGGLE_SWITCH) */
   else if (su->sg_of_su->admin_si->admin_state == NCS_ADMIN_STATE_LOCK)
   {
      /* the SI admin operation is LOCK */ 
      if ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCED)) 
      {
         /* this SI relation with the SU is quiesced assigning.
          * Remove the SI from the SI admin pointer.  Mark all the 
          * SI relationships to this SU as unassigning.
          */
         
         a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            if (s_susi->su->readiness_state == NCS_IN_SERVICE)
            {
               /* An in-service standby SU. Send D2N-INFO_SU_SI_ASSIGN 
                * modify active all. Add that SU to operation list. Change 
                * state to SG_realign state. Free all the SI assignments
                * to this SU.
                */
               avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);  
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
            
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));   
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         } /* if (s_susi != AVD_SU_SI_REL_NULL) */
         else
         {
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            
            if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
            {
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,o_su,FALSE);   
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
         }  /* else (s_susi != AVD_SU_SI_REL_NULL) */         
         
         
      }else /* if ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCED))  */
      {
         /* means standby free the assignments.*/            
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         
      } /* else ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCED))  */
      
   } /* if (su->sg_of_su->admin_si->admin_state == NCS_ADMIN_STATE_LOCK) */
   else if (su->sg_of_su->admin_si->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
   {
      /* the SI admin operation is shutdown */ 
      if ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCING)) 
      {
         /* this SI relation with the SU is quiescing assigning/assigned
          * Change the SI admin state to unlock Remove the SI from the 
          * SI admin pointer. Mark all the SI relationships to this SU as
          * unassigning. 
          */
         
         a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            if (s_susi->su->readiness_state == NCS_IN_SERVICE)
            {
               /* in-service standby SU. Send D2N-INFO_SU_SI_ASSIGN modify 
                * active all. Change state to SG_realign state. Add that 
                * SU to operation list. Free all the SI assignments to
                * this SU.
                */
               avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);  
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
            
            m_AVD_SET_SI_ADMIN(cb,(su->sg_of_su->admin_si),NCS_ADMIN_STATE_UNLOCK);               
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));   
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         } /* if (s_susi != AVD_SU_SI_REL_NULL) */
         else
         {
            m_AVD_SET_SI_ADMIN(cb,(su->sg_of_su->admin_si),NCS_ADMIN_STATE_LOCK);
            m_AVD_CLEAR_SG_ADMIN_SI(cb,(su->sg_of_su));
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            
            if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
            {
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,o_su,FALSE);   
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
         }  /* else (s_susi != AVD_SU_SI_REL_NULL) */         
         
         
      }else /* if ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCING))  */
      {
         /* means standby free the assignments.*/            
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         
      } /* else ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) ||
         (su->list_of_susi->state == SA_AMF_HA_QUIESCING))  */
      
   } /* if (su->sg_of_su->admin_si->admin_state == NCS_ADMIN_STATE_SHUTDOWN) */


   return;

}

/*****************************************************************************
 * Function: avd_sg_2n_node_fail_func
 *
 * Purpose:  This function is called when the node has already failed and
 *           the SIs have to be failed over. It does the functionality 
 *           specified in the design.
 *
 * Input: cb - the AVD control block
 *        su - The SU that has faulted because of the node failure.
 *        
 *
 * Returns: None.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

void avd_sg_2n_node_fail_func(AVD_CL_CB *cb,AVD_SU *su)
{
   AVD_SU_SI_REL *a_susi, *s_susi;
   AVD_SU *o_su, *i_su;
   NCS_BOOL flag;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_node_fail_func");
   m_AVD_LOG_RCVD_VAL(((long)su));
   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   
   if(su->list_of_susi == AVD_SU_SI_REL_NULL)
      return;
   
   switch(su->sg_of_su->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      
      if ((su->list_of_susi->state == SA_AMF_HA_ACTIVE) || (su->list_of_susi->state == SA_AMF_HA_QUIESCED))
      {
         /* this is a active SU. */
         /* Identify the standby SU and send D2N-INFO_SU_SI_ASSIGN message 
          * modify active all assignments. Remove and free all the SI
          * assignments to this SU. Add the standby SU to operation list and 
          * Change state to SG_realign. If standby SU doesnt exists stay in
          * the stable state.
          */
         
         a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            if ((s_susi->fsm != AVD_SU_SI_STATE_UNASGN) &&
               (s_susi->su->readiness_state == NCS_IN_SERVICE))
            {
               avd_sg_su_si_mod_snd(cb,s_susi->su,SA_AMF_HA_ACTIVE);
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,s_susi->su,FALSE);   
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
               
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         } /* if (s_susi != AVD_SU_SI_REL_NULL) */
         else
         {
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            
            if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
            {
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,o_su,FALSE);   
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
         }  /* else (s_susi != AVD_SU_SI_REL_NULL) */         
         
         
      }else /* if (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      {
         /* means standby */
         /* identify an SU in the list that is in-service and 
          * send D2N-INFO_SU_SI_ASSIGN message new standby for all the SIs on
          * this SU to that are in-service SU. Remove and free all the SI 
          * assignments to this SU. Add the newly assigned SU to operation list
          * and Change state to SG_realign. If no other in-service SU exists, 
          * stay in the stable state.
          */
         
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
         {
            /* add the SU to the operation list and change the SG FSM to SG realign. */
            avd_sg_su_oper_list_add(cb,o_su,FALSE);   
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }
         
      } /* else (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */

      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SG_REALIGN:

      if ( (su->list_of_susi->state != SA_AMF_HA_STANDBY) && 
           !( (su->list_of_susi->state == SA_AMF_HA_QUIESCED) && 
              (su->list_of_susi->fsm ==AVD_SU_SI_STATE_UNASGN) 
            )
         )
      {
         /* SU is not standby */
         a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);

         if (a_susi->su != su)
         {
            o_su = a_susi->su;
         }else if (s_susi != AVD_SU_SI_REL_NULL)
         {
            o_su = s_susi->su;
         }else
         {
            o_su = AVD_SU_NULL;
         }            

         if (o_su != AVD_SU_NULL)
         {
            if (o_su->readiness_state == NCS_IN_SERVICE)
            {
               /* the other SU has quiesced or standby assignments and is in the 
                * operation list and is in service. Send a D2N-INFO_SU_SI_ASSIGN
                * with modified active all to the other SU. Free the 
                * SU SI relationships of this SU.  Remove the SU from the SU 
                * operation list.
                */
               avd_sg_su_si_mod_snd(cb,o_su,SA_AMF_HA_ACTIVE);
               avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
               if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
                  m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_CHK_OPLIST(o_su,flag);
               if (flag == FALSE)
               {
                  avd_sg_su_oper_list_add(cb,o_su,FALSE);
               }
            }else
            {
               /* the other SU has quiesced or standby assigned and is in the 
                * operation list and is out of service.
                * Send a D2N-INFO_SU_SI_ASSIGN with remove all to that SU. 
                * Remove this SU from operation list. Free the 
                * SU SI relationships of this SU.
                */
               avd_sg_su_si_del_snd(cb,o_su);
               avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
               if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
                  m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_CHK_OPLIST(o_su,flag);
               if (flag == FALSE)
               {
                  avd_sg_su_oper_list_add(cb,o_su,FALSE);
               }
            }
         }else /* if (o_su != AVD_SU_NULL) */
         {
            avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
            if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            if (su->sg_of_su->su_oper_list.su == AVD_SU_NULL)
            {               
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);               
               if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
               {
                  /* add the SU to the operation list and change the SG FSM to SG realign. */
                  avd_sg_su_oper_list_add(cb,o_su,FALSE);   
                  m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
                  m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
               }
            }
         } /* else (o_su != AVD_SU_NULL) */
      
      } /* if (su->list_of_susi->state != SA_AMF_HA_STANDBY)*/
      else
      {
         /* the SU has standby assignments. Free all the SI assignments to 
          * this SU. Remove the SU from operation list . Verify that all the
          * SUSIs are assigned. Do the functionality specified in table with
          * stable state and new SI event.
          */
         avd_sg_su_asgn_del_util(cb,su,TRUE,FALSE);
         if (avd_sg_su_oper_list_del(cb,su,FALSE) != NCSCC_RC_SUCCESS)
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
         if (su->sg_of_su->su_oper_list.su == AVD_SU_NULL)
         {               
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);            
            if ((o_su = avd_sg_2n_su_chose_asgn(cb,su->sg_of_su)) != AVD_SU_NULL)
            {
               /* add the SU to the operation list and change the SG FSM to SG realign. */
               avd_sg_su_oper_list_add(cb,o_su,FALSE);   
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            }
         }
      } /* else (su->list_of_susi->state != SA_AMF_HA_STANDBY)*/

      break; /* case AVD_SG_FSM_SG_REALIGN: */
   case AVD_SG_FSM_SU_OPER:
      
      avd_sg_2n_node_fail_su_oper(cb,su);
   
      break; /* case AVD_SG_FSM_SU_OPER: */
   case AVD_SG_FSM_SI_OPER:
      
      avd_sg_2n_node_fail_si_oper(cb,su);
      
      break; /* case AVD_SG_FSM_SI_OPER: */
   case AVD_SG_FSM_SG_ADMIN:
      
      if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK)
      {
         /* SG is admin lock */
         
         /* Free all the SI assignments to this SU */
         avd_sg_su_asgn_del_util(cb,su, TRUE,FALSE);
         i_su = su->sg_of_su->list_of_su;
         while (i_su != AVD_SU_NULL)
         {
            if (i_su->list_of_susi != AVD_SU_SI_REL_NULL)
            {
               /* found a assigned su break*/
               break;
            }
            i_su = i_su->sg_list_su_next;
         }
         
         if (i_su == AVD_SU_NULL)
         {
            /* the SG doesnt have any SI assignments.Change state to stable. */
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }
         
      } /* if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_LOCK) */
      else if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {
         /* SG is admin shutdown */
         if (su->list_of_susi->state == SA_AMF_HA_QUIESCING)
         {
            /* the SI relationships to the SU is quiescing. Change admin state 
             * to lock, make the readiness state of all the SUs OOS.  Identify 
             * its standby SU assignment send D2N-INFO_SU_SI_ASSIGN remove all 
             * for the SU. If no standby, change state to stable. Free all the 
             * SI assignments to this SU.
             */
            a_susi = avd_sg_2n_act_susi(cb,su->sg_of_su,&s_susi);            
            if (s_susi != AVD_SU_SI_REL_NULL)
            {   
               avd_sg_su_si_del_snd(cb,s_susi->su);
            }else
            {
               m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_STABLE);
               m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state); 
            }
            
            avd_sg_su_asgn_del_util(cb,su, TRUE,FALSE);
            m_AVD_SET_SG_ADMIN(cb,(su->sg_of_su),NCS_ADMIN_STATE_LOCK);
            
            i_su = su->sg_of_su->list_of_su;
            while (i_su != AVD_SU_NULL)
            {
               m_AVD_SET_SU_REDINESS(cb,i_su,NCS_OUT_OF_SERVICE);
               i_su = i_su->sg_list_su_next;
            }
         } /* if (su->list_of_susi->state == SA_AMF_HA_QUIESCING)*/
         else 
         {
            /* It is standby SU Free all the SI assignments to this SU */
            avd_sg_su_asgn_del_util(cb,su, TRUE,FALSE);
         } /* else (su->list_of_susi->state == SA_AMF_HA_QUIESCING)*/
         
      } /* if (su->sg_of_su->admin_state == NCS_ADMIN_STATE_SHUTDOWN) */

      break; /* case AVD_SG_FSM_SG_ADMIN: */   
   default:
      /* log fatal error about the invalid value */     
      m_AVD_LOG_INVALID_VAL_FATAL(((long)su));
      m_AVD_LOG_INVALID_VAL_FATAL(((uns32)su->sg_of_su->sg_fsm_state));
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(su->name_net.value,su->name_net.length);
      return;
      break;
   } /* switch(su->sg_of_su->sg_fsm_state) */
   
   return;
}

 
/*****************************************************************************
 * Function: avd_sg_2n_su_admin_fail
 *
 * Purpose:  This function is called when SU become OOS because of the
 * LOCK or shutdown of the SU or node.The functionality will be as described in
 * the SG design FSM. 
 *
 * Input: cb - the AVD control block
 *        su - The SU that has failed because of the admin operation.
 *        avnd - The AvND structure of the node that is being operated upon.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function. The avnd pointer
 * value is valid only if this is a SU operation being done because of the node
 * admin change.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_su_admin_fail(AVD_CL_CB *cb,AVD_SU *su, AVD_AVND *avnd)
{
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_su_admin_fail");
   m_AVD_LOG_RCVD_VAL(((long)su));
   m_AVD_LOG_RCVD_VAL(((long)avnd));
   
   m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
   if((cb->init_state != AVD_APP_STATE) &&
      (su->sg_of_su->sg_ncs_spec == SA_FALSE))
   {
      return NCSCC_RC_FAILURE;
   }
   
   switch (su->sg_of_su->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      if ((su->admin_state == NCS_ADMIN_STATE_LOCK) ||
         ((avnd != AVD_AVND_NULL) && (avnd->su_admin_state == NCS_ADMIN_STATE_LOCK)))
      {
         if (su->list_of_susi->state == SA_AMF_HA_ACTIVE)
         {
            /* this is a active SU. */
            /* change the state for all assignments to quiesced. */
            if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
            {
               /* loga fatal error*/
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            /* add the SU to the operation list and change the SG FSM to SU operation. */
            avd_sg_su_oper_list_add(cb,su,FALSE);   
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
            
         }else /* if (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
         {
            /* means standby */
            /* change the state for all assignments to unassign and send delete mesage. */
            if(avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            /* add the SU to the operation list and change the SG FSM to SG realign. */
            avd_sg_su_oper_list_add(cb,su,FALSE);   
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         } /* else (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      } /* if ((su->admin_state == NCS_ADMIN_STATE_LOCK) ||
         ((avnd != AVD_AVND_NULL) && (avnd->su_admin_state == NCS_ADMIN_STATE_LOCK))) */
      else if ((su->admin_state == NCS_ADMIN_STATE_SHUTDOWN) ||
         ((avnd != AVD_AVND_NULL) && (avnd->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN)))
      {
         if (su->list_of_susi->state == SA_AMF_HA_ACTIVE)
         {
            /* this is a active SU. */
            /* change the state for all assignments to quiesceing. */
            if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCING) == NCSCC_RC_FAILURE)
            {
               /* loga fatal error*/
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            /* add the SU to the operation list and change the SG FSM to SU operation. */
            avd_sg_su_oper_list_add(cb,su,FALSE);   
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SU_OPER);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         }else /* if (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
         {
            /* means standby */
            /* change the state for all assignments to unassign and send delete mesage. */
            if(avd_sg_su_si_del_snd(cb,su) == NCSCC_RC_FAILURE)
            {
               /* log fatal error */
               m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
               m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
               return NCSCC_RC_FAILURE;
            }
            
            /* add the SU to the operation list and change the SG FSM to SG realign. */
            avd_sg_su_oper_list_add(cb,su,FALSE);
            if (su->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
               m_AVD_SET_SU_ADMIN(cb,su,NCS_ADMIN_STATE_LOCK);
            m_AVD_SET_SG_FSM(cb,(su->sg_of_su),AVD_SG_FSM_SG_REALIGN);
            m_AVD_LOG_RCVD_VAL(su->sg_of_su->sg_fsm_state);
         } /* else (su->list_of_susi->state == SA_AMF_HA_ACTIVE) */
      } /* if ((su->admin_state == NCS_ADMIN_STATE_SHUTDOWN) ||
         ((avnd != AVD_AVND_NULL) && (avnd->su_admin_state == NCS_ADMIN_STATE_SHUTDOWN))) */
      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SU_OPER:
      if ((su->sg_of_su->su_oper_list.su == su) &&
         (su->list_of_susi->state == SA_AMF_HA_QUIESCING) &&
         ((su->admin_state == NCS_ADMIN_STATE_LOCK) ||
         ((avnd != AVD_AVND_NULL) && (avnd->su_admin_state == NCS_ADMIN_STATE_LOCK))))
      {
         /* If the SU is in the operation list and the SU admin state is lock.
          * send D2N-INFO_SU_SI_ASSIGN modify quiesced message to the SU. 
          */
         if(avd_sg_su_si_mod_snd(cb,su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
            return NCSCC_RC_FAILURE;
         }
      }
      break; /* case AVD_SG_FSM_SU_OPER: */
   default:
      /* Log a error */     
      m_AVD_LOG_INVALID_VAL_ERROR(((long)su));
      m_AVD_LOG_INVALID_VAL_ERROR(((uns32)su->sg_of_su->sg_fsm_state));
      m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(su->name_net.value,su->name_net.length);
      return NCSCC_RC_FAILURE;
      break;
   } /* switch (su->sg_of_su->sg_fsm_state) */
   
   return NCSCC_RC_SUCCESS;
}

 
/*****************************************************************************
 * Function: avd_sg_2n_si_admin_down
 *
 * Purpose:  This function is called when SIs admin state is changed to
 * LOCK or shutdown. The functionality will be as described in
 * the SG design FSM. 
 *
 * Input: cb - the AVD control block
 *        si - The SI pointer.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_si_admin_down(AVD_CL_CB *cb,AVD_SI *si)
{
   AVD_SU_SI_REL *a_susi, *s_susi;
   AVD_SU_SI_STATE old_fsm_state;
   SaAmfHAStateT old_state;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_si_admin_down");
   m_AVD_LOG_RCVD_VAL(((long)si));
   
   m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
   
   if((cb->init_state != AVD_APP_STATE) &&
      (si->sg_of_si->sg_ncs_spec == SA_FALSE))
   {
      return NCSCC_RC_FAILURE;
   }
   if (si->list_of_sisu == AVD_SU_SI_REL_NULL)
   {
      return NCSCC_RC_SUCCESS;
   }
   switch (si->sg_of_si->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      if (si->admin_state == NCS_ADMIN_STATE_LOCK)
      {
         /* SI lock. Identify the SU that is active send D2N-INFO_SU_SI_ASSIGN
          * modify quiesced for only this SI, identify its standby SU assignment
          * send D2N-INFO_SU_SI_ASSIGN remove for only this SI. Change state to
          * SI_operation state. Add it to admin SI pointer.
          */
         if (si->list_of_sisu->state == SA_AMF_HA_ACTIVE)
         {
            a_susi = si->list_of_sisu;
            s_susi = si->list_of_sisu->si_next;
         }else
         {
            s_susi = si->list_of_sisu;
            a_susi = si->list_of_sisu->si_next;
         }
         

         /* change the state of the SI to quiesced */
         
         old_state = a_susi->state;
         old_fsm_state = a_susi->fsm;
         a_susi->state = SA_AMF_HA_QUIESCED;
         a_susi->fsm = AVD_SU_SI_STATE_MODIFY;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, a_susi, AVSV_CKPT_AVD_SU_SI_REL);
         avd_gen_su_ha_state_changed_trap(cb, a_susi);
         if(avd_snd_susi_msg(cb,a_susi->su,a_susi,AVSV_SUSI_ACT_MOD) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)a_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->su->name_net.value,a_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->si->name_net.value,a_susi->si->name_net.length);
            a_susi->state = old_state;
            a_susi->fsm = old_fsm_state;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, a_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, a_susi);
            return NCSCC_RC_FAILURE;
         }
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            s_susi->fsm = AVD_SU_SI_STATE_UNASGN;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, s_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_snd_susi_msg(cb,s_susi->su,s_susi,AVSV_SUSI_ACT_DEL);
         }
         
         /* add the SI to the admin list and change the SG FSM to SI operation. */
         m_AVD_SET_SG_ADMIN_SI(cb,si);
         m_AVD_SET_SG_FSM(cb,(si->sg_of_si),AVD_SG_FSM_SI_OPER);
         m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
      } /* if (si->admin_state == NCS_ADMIN_STATE_LOCK) */
      else if (si->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {
         /* SI shutdown. Identify the SU that is active 
          * send D2N-INFO_SU_SI_ASSIGN modify quiescing for only this SI. 
          * Change state to SI_operation state. Add it to admin SI pointer.
          */
         if (si->list_of_sisu->state == SA_AMF_HA_ACTIVE)
         {
            a_susi = si->list_of_sisu;
            s_susi = si->list_of_sisu->si_next;
         }else
         {
            s_susi = si->list_of_sisu;
            a_susi = si->list_of_sisu->si_next;
         }
         

         /* change the state of the SI to quiescing */
         
         old_state = a_susi->state;
         old_fsm_state = a_susi->fsm;
         a_susi->state = SA_AMF_HA_QUIESCING;
         a_susi->fsm = AVD_SU_SI_STATE_MODIFY;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, a_susi, AVSV_CKPT_AVD_SU_SI_REL);
         avd_gen_su_ha_state_changed_trap(cb, a_susi);
         if(avd_snd_susi_msg(cb,a_susi->su,a_susi,AVSV_SUSI_ACT_MOD) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)a_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->su->name_net.value,a_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->si->name_net.value,a_susi->si->name_net.length);
            a_susi->state = old_state;
            a_susi->fsm = old_fsm_state;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, a_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, a_susi);
            return NCSCC_RC_FAILURE;
         }
         
         /* add the SI to the admin list and change the SG FSM to SI operation. */
         m_AVD_SET_SG_ADMIN_SI(cb,si);
         m_AVD_SET_SG_FSM(cb,(si->sg_of_si),AVD_SG_FSM_SI_OPER);
         m_AVD_LOG_RCVD_VAL(si->sg_of_si->sg_fsm_state);
      } /* if (si->admin_state == NCS_ADMIN_STATE_SHUTDOWN) */
      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SI_OPER:
      if ((si->sg_of_si->admin_si == si) &&
         (si->admin_state == NCS_ADMIN_STATE_LOCK))
      {
         /* If the SI is in the admin pointer and the SI admin state is shutdown, 
          * change the admin state of the SI to lock and 
          * send D2N-INFO_SU_SI_ASSIGN modify quiesced message to the SU that
          * is being assigned quiescing state. Send a D2N-INFO_SU_SI_ASSIGN 
          * remove message for this SI to the other SU.
          */
         if (si->list_of_sisu->state == SA_AMF_HA_QUIESCING)
         {
            a_susi = si->list_of_sisu;
            s_susi = si->list_of_sisu->si_next;
         }else
         {
            s_susi = si->list_of_sisu;
            a_susi = si->list_of_sisu->si_next;
         }
         

         /* change the state of the SI to quiesced */
         
         old_state = a_susi->state;
         old_fsm_state = a_susi->fsm;
         a_susi->state = SA_AMF_HA_QUIESCED;
         a_susi->fsm = AVD_SU_SI_STATE_MODIFY;
         m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, a_susi, AVSV_CKPT_AVD_SU_SI_REL);
         avd_gen_su_ha_state_changed_trap(cb, a_susi);
         if(avd_snd_susi_msg(cb,a_susi->su,a_susi,AVSV_SUSI_ACT_MOD) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)a_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->su->name_net.value,a_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->si->name_net.value,a_susi->si->name_net.length);
            a_susi->state = old_state;
            a_susi->fsm = old_fsm_state;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, a_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_gen_su_ha_state_changed_trap(cb, a_susi);
            return NCSCC_RC_FAILURE;
         }
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            s_susi->fsm = AVD_SU_SI_STATE_UNASGN;
            m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(cb, s_susi, AVSV_CKPT_AVD_SU_SI_REL);
            avd_snd_susi_msg(cb,s_susi->su,s_susi,AVSV_SUSI_ACT_DEL);
         }
      }
      break; /* case AVD_SG_FSM_SI_OPER: */
   default:
      /* Log a error */    
      m_AVD_LOG_INVALID_VAL_ERROR(((long)si));
      m_AVD_LOG_INVALID_VAL_ERROR(((uns32)si->sg_of_si->sg_fsm_state));
      m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(si->name_net.value,si->name_net.length);
      return NCSCC_RC_FAILURE;
      break;
   } /* switch (si->sg_of_si->sg_fsm_state) */
   
   return NCSCC_RC_SUCCESS;
}

 
/*****************************************************************************
 * Function: avd_sg_2n_sg_admin_down
 *
 * Purpose:  This function is called when SGs admin state is changed to
 * LOCK or shutdown. The functionality will be as described in
 * the SG design FSM. 
 *
 * Input: cb - the AVD control block
 *        sg - The SG pointer.
 *        
 *
 * Returns: NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 *
 * NOTES: This is a 2N redundancy model specific function.
 *
 * 
 **************************************************************************/

uns32 avd_sg_2n_sg_admin_down(AVD_CL_CB *cb,AVD_SG *sg)
{
   AVD_SU_SI_REL * a_susi, *s_susi;
   
   m_AVD_LOG_FUNC_ENTRY("avd_sg_2n_sg_admin_down");
   m_AVD_LOG_RCVD_VAL(((long)sg));
   
   m_AVD_LOG_RCVD_VAL(sg->sg_fsm_state);
   
   if((cb->init_state != AVD_APP_STATE) &&
      (sg->sg_ncs_spec == SA_FALSE))
   {
      return NCSCC_RC_FAILURE;
   }
   
   switch (sg->sg_fsm_state)
   {
   case AVD_SG_FSM_STABLE:
      a_susi = avd_sg_2n_act_susi(cb,sg,&s_susi);
      if (a_susi == AVD_SU_SI_REL_NULL)
      {
         m_AVD_SET_SG_ADMIN(cb,sg,NCS_ADMIN_STATE_LOCK);
         m_AVD_SET_SG_FSM(cb,sg,AVD_SG_FSM_STABLE);
         return NCSCC_RC_SUCCESS;
      }
      
      if (sg->admin_state == NCS_ADMIN_STATE_LOCK)
      {
         /* SG lock. Identify the SU that is active send D2N-INFO_SU_SI_ASSIGN 
          * modify quiesced all for the SU; identify standby SU 
          * send D2N-INFO_SU_SI_ASSIGN remove all for standby SU.
          * Change state to SG admin state.
          */
         if(avd_sg_su_si_mod_snd(cb,a_susi->su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)a_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->su->name_net.value,a_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->si->name_net.value,a_susi->si->name_net.length);
            return NCSCC_RC_FAILURE;
         }         
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_si_del_snd(cb,s_susi->su);
         }
         
         m_AVD_SET_SG_FSM(cb,sg,AVD_SG_FSM_SG_ADMIN);
         m_AVD_LOG_RCVD_VAL(sg->sg_fsm_state);
      } /* if (sg->admin_state == NCS_ADMIN_STATE_LOCK) */
      else if (sg->admin_state == NCS_ADMIN_STATE_SHUTDOWN)
      {
         /* SG shutdown. Identify the SU that is active 
          * send D2N-INFO_SU_SI_ASSIGN modify quiescing all for this SU. 
          * Change state to SG_admin. If no active SU exist, change the SG 
          * admin state to LOCK, stay in stable state.
          */
         if(avd_sg_su_si_mod_snd(cb,a_susi->su,SA_AMF_HA_QUIESCING) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)a_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->su->name_net.value,a_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->si->name_net.value,a_susi->si->name_net.length);
            return NCSCC_RC_FAILURE;
         }
         
         m_AVD_SET_SG_FSM(cb,sg,AVD_SG_FSM_SG_ADMIN);
         m_AVD_LOG_RCVD_VAL(sg->sg_fsm_state);
      } /* if (sg->admin_state == NCS_ADMIN_STATE_SHUTDOWN) */
      break; /* case AVD_SG_FSM_STABLE: */
   case AVD_SG_FSM_SG_ADMIN:
      if (sg->admin_state == NCS_ADMIN_STATE_LOCK)
      {
         /* If the SG admin state is shutdown, change the admin state of 
          * the SG to lock and send D2N-INFO_SU_SI_ASSIGN modify quiesced 
          * message to the SU with quiescing assignment. Also 
          * send D2N-INFO_SU_SI_ASSIGN with remove all to the SU with 
          * standby assignment.
          */
         a_susi = avd_sg_2n_act_susi(cb,sg,&s_susi);
         if(avd_sg_su_si_mod_snd(cb,a_susi->su,SA_AMF_HA_QUIESCED) == NCSCC_RC_FAILURE)
         {
            /* loga fatal error*/
            m_AVD_LOG_INVALID_VAL_ERROR(((long)a_susi));
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->su->name_net.value,a_susi->su->name_net.length);
            m_AVD_LOG_INVALID_NAME_NET_VAL_ERROR(a_susi->si->name_net.value,a_susi->si->name_net.length);
            return NCSCC_RC_FAILURE;
         }         
         
         if (s_susi != AVD_SU_SI_REL_NULL)
         {
            avd_sg_su_si_del_snd(cb,s_susi->su);
         }
         
      } /* if (sg->admin_state == NCS_ADMIN_STATE_LOCK) */
      break; /* case AVD_SG_FSM_SG_ADMIN: */
   default:
      /* Log a error */    
      m_AVD_LOG_INVALID_VAL_FATAL(((long)sg));
      m_AVD_LOG_INVALID_VAL_FATAL(((uns32)sg->sg_fsm_state));
      m_AVD_LOG_INVALID_NAME_NET_VAL_FATAL(sg->name_net.value,sg->name_net.length);
      return NCSCC_RC_FAILURE;
      break;
   } /* switch (sg->sg_fsm_state) */
   
   return NCSCC_RC_SUCCESS;
}
