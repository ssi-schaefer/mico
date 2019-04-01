/*
 *  MICO --- an Open Source CORBA implementation
 *  Copyright (C) 1998 Frank Pilhofer
 *  Copyright (c) 1999-2019 by The Mico Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  For more information, visit the MICO Home Page at
 *  http://www.mico.org/
 */

#ifdef FAST_PCH
#include "imr_pch.h"
#endif // FAST_PCH
#ifdef __COMO__
#pragma hdrstop
#endif // __COMO__

#ifndef FAST_PCH

#include <CORBA.h>
#include <mico/impl.h>
#include <mico/util.h>
#include <mico/os-misc.h>
#include <mico/template_impl.h>
#include "poamediator.h"

#endif // FAST_PCH


using namespace std;

/*
 * ----------------------------------------------------------------------
 * The POA Mediator
 *
 * Persistent POAs connect here using create_impl(). The POA tells us
 * its IOR Template, which contains its address. We return our IOR. The
 * POA then produces object references pointing to us on its own. The
 * Objectkey of such references starts with the unique identifier for
 * that server followed by ':'.
 * When we receive an invocation for such a reference, we construct a
 * new object reference by taking the POAs IOR template plus the object
 * key, and forward the invocation.
 * ----------------------------------------------------------------------
 */

/*
 * Helpers
 */

POAMediatorImpl::SvInf::SvInf ()
    : lock(FALSE, MICOMT::Mutex::Recursive)
{
  pstate = Inactive;
  proc = NULL;
}

/*
 * ----------------------------------------------------------------------
 * POAMediatorImpl
 * ----------------------------------------------------------------------
 */

POAMediatorImpl::POAMediatorImpl (CORBA::ORB_ptr _orb, CORBA::Boolean _forward)
    : svmap_lock_(FALSE, MICOMT::Mutex::Recursive), requests_lock_(FALSE, MICOMT::Mutex::Recursive), invqueue (this, _orb), lock_ (FALSE, MICOMT::Mutex::Recursive)
{
  orb = _orb;
  forward = _forward;
  myior = orb->ior_template()->stringify();

  CORBA::Object_var obj =
    orb->resolve_initial_references ("ImplementationRepository");
  imr = CORBA::ImplRepository::_narrow (obj);
  orb->register_oa (this);
}

POAMediatorImpl::~POAMediatorImpl ()
{
  MICOMT::AutoLock l(lock_);

  orb->unregister_oa (this);

  MICOMT::AutoLock l2(svmap_lock_);

  MapSvInf::iterator it;

  for (it = svmap.begin(); it != svmap.end(); it++) {
    MICOMT::AutoLock lx((*it).second.lock);
    if ((*it).second.proc) {
      delete (*it).second.proc;
    }
  }
}

/*
 * POA Mediator interface
 */

char *
POAMediatorImpl::create_impl (const char * svid, const char * ior)
{
  CORBA::ImplRepository::ImplDefSeq_var ids =
    imr->find_by_name (svid);

  if (ids->length() == 0) {
    CORBA::ImplementationDef_var id =
      imr->create (CORBA::ImplementationDef::ActivatePOA,
		   CORBA::ImplementationDef::ObjectInfoList(0),
		   svid, "");
  }

  MICOMT::AutoLock l(svmap_lock_);
  MICOMT::AutoLock l2(svmap[svid].lock);

  svmap[svid].ior = CORBA::IOR (ior);
  return CORBA::string_dup (myior.c_str());
}

void
POAMediatorImpl::activate_impl (const char * svid)
{
  MICOMT::AutoLock l(svmap_lock_);
  MICOMT::AutoLock l2(svmap[svid].lock);
  if (svmap[svid].pstate == Stopped ||
      svmap[svid].pstate == Holding) {
    return;
  }

  svmap[svid].pstate = Active;

  /*
   * perform pending invocations
   */

  invqueue.exec_later();
}

void
POAMediatorImpl::deactivate_impl (const char * svid)
{
  MICOMT::AutoLock l(svmap_lock_);

  SvInf &inf = svmap[svid];
  MICOMT::AutoLock l2(inf.lock);
  
  assert (inf.pstate == Stopped || inf.proc);

  switch (inf.pstate) {
  case Inactive:
  case Started:
  case Failed:
    assert (0);

  case Active:
    inf.pstate = Inactive;
    break;

  case Holding:
    inf.pstate = Stopped;
    break;

  case Stopped:
    break;

  default:
    assert (0);
  };

  if (inf.proc) {
    inf.proc->detach ();
#ifndef HAVE_THREADS
    // can't delete still running thread object!
    delete inf.proc;
    inf.proc = 0;
#endif // HAVE_THREADS
  }
}

CORBA::Boolean
POAMediatorImpl::force_activation (CORBA::ImplementationDef_ptr impl)
{
  CORBA::String_var svid = impl->name ();

  MICOMT::AutoLock l(svmap_lock_);
  SvInf &inf = svmap[svid.in()];

  MICOMT::AutoLock l2(inf.lock);

  switch (inf.pstate) {
  case Inactive:
    /* OK */
    break;

  case Started:
  case Active:
    return 1;

  case Holding:
    inf.pstate = Active;
    return 1;

  case Stopped:
    if (inf.proc) {
      cerr << "*** server cannot be activated: " << svid << endl
	   << "    in stopped state, still wating for server to exit" << endl;
      return 0;
    }
    inf.pstate = Inactive;
    break;

  case Failed:
    inf.pstate = Inactive;
    break;

  default:
    assert (0);
  }
  return create_server (svid);
}

CORBA::Boolean
POAMediatorImpl::hold (CORBA::ImplementationDef_ptr impl)
{
  CORBA::String_var svid = impl->name ();

  MICOMT::AutoLock l(svmap_lock_);
  SvInf &inf = svmap[svid.in()];

  MICOMT::AutoLock l2(inf.lock);

  switch (inf.pstate) {
  case Inactive:
  case Failed:
    inf.pstate = Holding;
    return 1;

  case Started:
  case Active:
  case Holding:
    inf.pstate = Holding;
    return 1;

  case Stopped:
    return 1;

  default:
    assert (0);
  }

  /* unreachable */
  return 0;
}

CORBA::Boolean
POAMediatorImpl::stop (CORBA::ImplementationDef_ptr impl)
{
  CORBA::String_var svid = impl->name ();
  {
    MICOMT::AutoLock l(svmap_lock_);
    SvInf &inf = svmap[svid.in()];

    MICOMT::AutoLock l2(inf.lock);

    switch (inf.pstate) {
    case Inactive:
    case Failed:
    case Stopped:
      inf.pstate = Stopped;
      return 1;

    case Started:
    case Active:
    case Holding:
      assert (inf.proc);
      break;

    default:
      assert (0);
    }

    /*
     * Signal the server to exit
     */
    inf.pstate = Stopped;
    inf.proc->terminate ();
  }

  /*
   * Wait for server exit confirmation
   */
  CORBA::Timeout t (orb->dispatcher(), 5000);
  CORBA::Boolean finish = FALSE;
  while (!finish) {
    if (t.done())
      finish = TRUE;
    {
      MICOMT::AutoLock l(svmap_lock_);
      MICOMT::AutoLock l2(svmap[svid.in()].lock);
      if (svmap[svid.in()].proc == NULL) {
        finish = TRUE;
      }
    }
    orb->perform_work ();
  }

  {
    MICOMT::AutoLock l(svmap_lock_);
    MICOMT::AutoLock l2(svmap[svid.in()].lock);
    if (svmap[svid.in()].proc) {
      cerr << "*** server cannot be stopped: " << svid << endl;
      return 0;
    }
  }
  return 1;
}

CORBA::Boolean
POAMediatorImpl::_cxx_continue (CORBA::ImplementationDef_ptr impl)
{
  CORBA::String_var svid = impl->name ();
  MICOMT::AutoLock l(svmap_lock_);
  SvInf &inf = svmap[svid.in()];
  MICOMT::AutoLock l2(inf.lock);

  switch (inf.pstate) {
  case Inactive:
  case Started:
  case Active:
    return 1;

  case Failed:
  case Stopped:
  case Holding:
    break;

  default:
    assert (0);
  }

  inf.pstate = Inactive;
  invqueue.exec_later();
  return 1;
}

/*
 * Try to start up a server
 */

CORBA::Boolean
POAMediatorImpl::create_server (const char * svid)
{
  svmap_lock_.lock();
  // hopefully this will never throw so manual unlocking below is ok
  SvInf& inf = svmap[svid];
  svmap_lock_.unlock();

  MICOMT::AutoLock l2(inf.lock);

  if (inf.pstate == Started || inf.pstate == Active) {
    return TRUE;
  }

  /*
   * If the server is in the Failed state, we employ a similar strategy
   * as init does. We refuse to restart a server within 30 seconds after
   * its failure. Note that the Failed state is only entered if the server
   * crashes prior to its activation.
   * Without this 30 sec pause, the request that started the server would
   * be a ping of death, causing permanent restart.
   * We assume that the server has been fixed after the pause.
   * This can be overridden (i.e. restart a server before 30 secs have
   * elapsed) by using `imr activate'
   */

  if (inf.pstate == Failed) {
    OSMisc::TimeVal curtime = OSMisc::gettime();
    if ((curtime.tv_sec - inf.failed) < 30) {
      // less than 30 secs elapsed since first failure
      return FALSE;
    }
    inf.pstate = Inactive;
  }

  assert (inf.pstate == Inactive);

  /*
   * Look up entry in Implementation Repository
   */

  if (CORBA::is_nil (imr)) {
    return FALSE;
  }

  CORBA::ImplRepository::ImplDefSeq_var impls = imr->find_by_name (svid);
  if (impls->length() == 0)
    return FALSE;

  CORBA::ImplementationDef_var idv =
    CORBA::ImplementationDef::_duplicate (impls[(CORBA::ULong)0]);

  /*
   * Construct command line
   */

  CORBA::String_var s = idv->command();
  string command = (const char *) s;

  if (command.length() == 0) {
    return 0;
  }

  command += " -POAImplName ";
  command += svid;

  s = orb->object_to_string (imr);
  command += " -ORBImplRepoIOR ";
  command += s;

  CORBA::Object_var obj =
    orb->resolve_initial_references ("InterfaceRepository");

  if (!CORBA::is_nil (obj)) {
    s = orb->object_to_string (obj);
    command += " -ORBIfaceRepoIOR ";
    command += s;
  }

  s = orb->object_to_string(my_ref_);
  command += " -POARemoteIOR ";
  command += s;

  if (inf.proc != NULL && inf.proc->finished()) {
    // GCing still allocated previous UnixProcess object which happen
    // on multi-threaded build due to inabiliaty to delete still
    // running thread object in the process callback method
    delete inf.proc;
    inf.proc = 0;
  }
  inf.pstate = Started;
  inf.proc = new MICO::UnixProcess (command.c_str(), this);

  return inf.proc->run();
}

void
POAMediatorImpl::set_own_ref(CORBA::Object_ptr obj)
{
    MICOMT::AutoLock l(lock_);
    my_ref_ = CORBA::Object::_duplicate(obj);
}

/*
 * OA Interface
 */

const char *
POAMediatorImpl::get_oaid () const
{
    return "mico-poa-mediator";
}

CORBA::Boolean
POAMediatorImpl::has_object (CORBA::Object_ptr obj)
{
  /*
   * Extract Objectkey from IOR. It starts with the POAImplName.
   * Take care of escaped slashes.
   */

  const CORBA::Octet * key;
  CORBA::Long length, i, j;

  if (CORBA::is_nil (obj) || !obj->_ior() || !obj->_ior()->profile()) {
    return FALSE;
  }

  key = obj->_ior()->profile()->objectkey (length);

  if (!key || length < 0) {
    return FALSE;
  }

  for (i=0, j=0; i<length; i++) {
    if (key[i] == '\\') {
      j++;
    }
    else if (key[i] == '/' && (j%2) == 0) {
      break;
    }
    else {
      j = 0;
    }
  }

  string svid;

  for (j=0; j<i; j++) {
    if (key[j] == '\\') {
      j++;
    }
    svid += key[j];
  }

  /*
   * Look up ServerId in Map
   *
   * If it isn't in the map, but in the Implementation Repository, recreate
   * an svmap entry so that the server will be restarted upon invoke().
   */

  MICOMT::AutoLock l(svmap_lock_);
  MapSvInf::iterator it = svmap.find (svid);

  if (it == svmap.end()) {
    CORBA::ImplRepository::ImplDefSeq_var ims =
      imr->find_by_name (svid.c_str());
    if (ims->length() == 0) {
      return FALSE;
    }
    MICOMT::AutoLock l2(svmap[svid].lock);
    svmap[svid].pstate = Inactive;
  }

  return TRUE;
}

CORBA::Boolean
POAMediatorImpl::is_local () const
{
    return TRUE;
}


#ifdef USE_CSL2
CORBA::Principal_ptr
POAMediatorImpl::get_principal(CORBA::Object_ptr obj){
  assert(0);
  return (CORBA::Principal_ptr)0;
}
#endif /* USE_CSL2  */

CORBA::Boolean
POAMediatorImpl::invoke (CORBA::ORBMsgId id, CORBA::Object_ptr obj,
			 CORBA::ORBRequest *req,
			 CORBA::Principal_ptr pr,
			 CORBA::Boolean response_exp)
{
  /*
   * Extract Objectkey from IOR. It starts with the POAImplName.
   */

  const CORBA::Octet * key;
  CORBA::Long length, i, j;

  assert (!CORBA::is_nil (obj) && obj->_ior() && obj->_ior()->profile());
  key = obj->_ior()->profile()->objectkey (length);
  assert (key);

  for (i=0, j=0; i<length; i++) {
    if (key[i] == '\\') {
      j++;
    }
    else if (key[i] == '/' && (j%2) == 0) {
      break;
    }
    else {
      j = 0;
    }
  }

  string svid;

  for (j=0; j<i; j++) {
    if (key[j] == '\\') {
      j++;
    }
    svid += key[j];
  }

  /*
   * Look up ServerId in Map
   */
  MICOMT::AutoLock l(svmap_lock_);
  MapSvInf::iterator it = svmap.find (svid);

  if (it == svmap.end()) {
    /*
     * Server has disappeared? Oh well.
     */
    CORBA::OBJECT_NOT_EXIST ex;
    req->set_out_args (&ex);
    orb->answer_invoke (id, CORBA::InvokeSysEx,
			CORBA::Object::_nil(), req, 0);
    return TRUE;
  }

  MICOMT::AutoLock l2((*it).second.lock);

  /*
   * If the Server is stopped, queue the request
   */
  if ((*it).second.pstate == Stopped ||
      (*it).second.pstate == Holding) {
    invqueue.add (new MICO::ReqQueueRec (id, req, obj, pr, response_exp));
    return TRUE;
  }

  /*
   * Is Server active?
   */

  if ((*it).second.pstate != Active) {
    /*
     * No? Restart it.
     */
    if (!create_server (svid.c_str())) {
      /*
       * failed.
       */
      CORBA::OBJECT_NOT_EXIST ex;
      req->set_out_args (&ex);
      orb->answer_invoke (id, CORBA::InvokeSysEx,
			  CORBA::Object::_nil(), req, 0);
      return TRUE;
    }

    /*
     * Server has been started, but is not active yet. We must queue
     * the request until the server announces its readiness by calling
     * activate_impl().
     */

    invqueue.add (new MICO::ReqQueueRec (id, req, obj, pr, response_exp));
    return TRUE;
  }

  /*
   * Construct remote object reference
   */

  CORBA::IOR * ior = new CORBA::IOR ((*it).second.ior);
  ior->objectkey ((CORBA::Octet *) key, length);
  ior->objid (obj->_ior()->objid());

  CORBA::Object_var remote_ref = new CORBA::Object (ior);
  remote_ref->_setup_domains (CORBA::Object::_nil());

  if (response_exp && forward) {
    // forward to server ...
    orb->answer_invoke (id, CORBA::InvokeForward,
			remote_ref, req, 0);
    return TRUE;
  }
  /*
   * do the invocation (on the remote object)
   */
  CORBA::ORBMsgId_var orbid = orb->new_orbid (orb->new_msgid());

  if (response_exp) {
    MICOMT::AutoLock lr(requests_lock_);
    assert(requests.count(orbid) == 0);
    requests[orbid] = CORBA::ORBInvokeRec::_duplicate(id);
  }

  orb->invoke_async (remote_ref, req, pr, response_exp, this, orbid);
  return TRUE;
}

CORBA::Boolean
POAMediatorImpl::bind (CORBA::ORBMsgId id, const char *repoid,
		       const CORBA::ORB::ObjectTag &tag,
		       CORBA::Address *addr)
{
  if (addr && !addr->is_local()) {
    return FALSE;
  }

  /*
   * Search the Implementation Repository for a POA entry that serves
   * the given Repository Id
   */

  CORBA::ImplRepository::ImplDefSeq_var ims =
      imr->find_by_repoid_tag (repoid, tag);

  /*
   * Make sure that all available servers are active. Start them if
   * necessary. Queue request until all servers are up.
   */

  CORBA::Boolean queue = FALSE;
  CORBA::ULong count = 0;

  MICOMT::AutoLock l(svmap_lock_);

  for (CORBA::ULong i=0; i<ims->length(); i++) {
    if (ims[i]->mode() != CORBA::ImplementationDef::ActivatePOA) {
      continue;
    }

    count++;

    CORBA::String_var sname = ims[i]->name();
    SvInf &inf = svmap[sname.in()];

    MICOMT::AutoLock l2(inf.lock);

    if (inf.pstate != Active && inf.pstate != Stopped &&
	inf.pstate != Holding) {
      if (!create_server (ims[i]->name())) {
	orb->answer_bind (id, CORBA::LocateUnknown,
			  CORBA::Object::_nil());
	return TRUE;
      }

      queue = TRUE;
    }
  }

  if (count == 0) {
    /*
     * No servers available
     */
    return FALSE;
  }

  if (queue) {
    invqueue.add (new MICO::ReqQueueRec (id, repoid, tag));
    return TRUE;
  }

  /*
   * Try all active servers in our map
   */

  MapSvInf::iterator it;
  
  /*
   * allocate slots for all bind requests, so that we can decide in
   * the callback() whether there are more outstanding requests or
   * not.
   */

  vector<CORBA::ORBMsgId_var> msgids;
  for (it = svmap.begin(); it != svmap.end(); it++) {
    MICOMT::AutoLock l2((*it).second.lock);
    if ((*it).second.pstate != Active) {
      continue;
    }
    msgids.push_back (orb->new_orbid (orb->new_msgid()));
    {
        MICOMT::AutoLock l3(requests_lock_);
        requests[msgids.back()] = CORBA::ORBInvokeRec::_duplicate(id);
    }
  }

  /*
   * now send the requests
   */

  for (it = svmap.begin(); it != svmap.end(); it++) {
    MICOMT::AutoLock l2((*it).second.lock);
    if ((*it).second.pstate != Active) {
      continue;
    }

    CORBA::Address * addr = (CORBA::Address *) (*it).second.ior.addr();
    assert (addr);

    assert (msgids.size() > 0);
    CORBA::ORBMsgId orbid = orb->bind_async (repoid, tag, addr, this,
					     msgids.back());
    msgids.pop_back();
    assert (orbid != 0);
  }

  return TRUE;
}

CORBA::Boolean
POAMediatorImpl::locate (CORBA::ORBMsgId id, CORBA::Object_ptr)
{
  orb->answer_locate (id, CORBA::LocateHere, CORBA::Object::_nil(), 0);
  return TRUE;
}

CORBA::Object_ptr
POAMediatorImpl::skeleton (CORBA::Object_ptr)
{
  return CORBA::Object::_nil ();
}

void
POAMediatorImpl::cancel (CORBA::ORBMsgId id)
{
  MICOMT::AutoLock l(requests_lock_);
  int again = 1;
  while (again) {
    again = 0;
    MapIdId::iterator i;
    for (i = requests.begin(); i != requests.end(); ++i) {
      if ((*i).second == id) {
	orb->cancel ((*i).first);
	requests.erase (i);
	again = 1;
	break;
      }
    }
  }
}

void
POAMediatorImpl::shutdown_server ()
{
  {
    MICOMT::AutoLock l(svmap_lock_);
    /*
     * Signal servers
     */

    for (MapSvInf::iterator it = svmap.begin(); it != svmap.end(); it++) {
      MICOMT::AutoLock l2((*it).second.lock);
      if ((*it).second.proc != 0) {
        (*it).second.proc->terminate ();
      }
    }
  }
  /*
   * Wait for callbacks to arrive
   */

  CORBA::Boolean waiting;
  OSMisc::TimeVal t1 = OSMisc::gettime ();
  OSMisc::TimeVal t2;

  do {
    waiting = FALSE;
    {
      MICOMT::AutoLock l(svmap_lock_);

      for (MapSvInf::iterator it1 = svmap.begin(); it1 != svmap.end(); it1++) {
        MICOMT::AutoLock l2((*it1).second.lock);
#ifndef HAVE_THREADS
        if ((*it1).second.proc != 0) {
#else // HAVE_THREADS
        if ((*it1).second.proc != 0 && !((*it1).second.proc->finished())) {
#endif // HAVE_THREADS
          waiting = TRUE;
        }
      }
    }

    if (waiting) {
      orb->perform_work ();
    }
    t2 = OSMisc::gettime();
  }
  while (waiting && (t2.tv_sec - t1.tv_sec < 60));

  MICOMT::AutoLock l(svmap_lock_);
  for (MapSvInf::iterator it2 = svmap.begin(); it2 != svmap.end(); it2++) {
    MICOMT::AutoLock l2((*it2).second.lock);
#ifndef HAVE_THREADS
    if ((*it2).second.proc != 0) {
#else // HAVE_THREADS
    if ((*it2).second.proc != 0 && !((*it2).second.proc->finished())) {
#endif // HAVE_THREADS
      cerr << "*** server cannot be stopped: " << (*it2).first << endl;
    }
  }
}

void
POAMediatorImpl::shutdown (CORBA::Boolean wait_for_completion)
{
  invqueue.fail();
  orb->answer_shutdown (this);
}

void
POAMediatorImpl::answer_invoke (CORBA::ORBMsgId, CORBA::Object_ptr,
				CORBA::ORBRequest *, CORBA::InvokeStatus)
{
  assert (0);
}

CORBA::Boolean
POAMediatorImpl::validate_connection
(CORBA::Object_ptr obj,
 CORBA::PolicyList_out inconsistent_policies)
{
    // shouldn't be called
    assert(0);
    return FALSE;
}

void
POAMediatorImpl::timedout_invoke(CORBA::ORBMsgId)
{
    // shouldn't be called
    assert(0);
}

/*
 * ORB Callback
 */

CORBA::Boolean
POAMediatorImpl::waitfor (CORBA::ORB_ptr, CORBA::ORBMsgId msgid,
			  CORBA::ORBCallback::Event ev, CORBA::Long tmout)
{
    assert(0);
    return TRUE;
}

void
POAMediatorImpl::notify (CORBA::ORB_ptr porb, CORBA::ORBMsgId id,
			 CORBA::ORBCallback::Event ev)
{
  MICOMT::AutoLock l(requests_lock_);

  switch (ev) {
  case CORBA::ORBCallback::Invoke:
    {
      // an invocation completed ...
      CORBA::ORBRequest *req;
      CORBA::Object_var obj;
      GIOP::AddressingDisposition ad;
      CORBA::InvokeStatus stat = porb->get_invoke_reply (id, obj, req, ad);

      MapIdId::iterator i = requests.find (id);
      assert (i != requests.end());

      porb->answer_invoke ((*i).second, stat, obj, req, ad);
      requests.erase (i);
    }
    break;

  case CORBA::ORBCallback::Bind:
    {
      // a bind completed ...
      CORBA::Object_var obj;
      CORBA::LocateStatus stat = porb->get_bind_reply (id, obj);

      MapIdId::iterator i = requests.find (id);
      if (i == requests.end()) {
          // the invocation was already canceled/removed from requests
          // by some successful (returning LocateHere) bind call. No
          // need to do anythig here except perhaps cancel the invocation.
          porb->cancel(id);
          break;
      }
      assert (i != requests.end());

      CORBA::ORBInvokeRec_var id2 = CORBA::ORBInvokeRec::_duplicate((*i).second);
      requests.erase (i);

      if (stat == CORBA::LocateHere) {
	// found matching object ...
	porb->answer_bind (id2, stat, obj);
	// cancel all binds resulting from the same bind "broadcast"
	cancel (id2);
      } else {
	/*
	 * not found, see if there are more outstanding requests
	 * from the same bind "broadcast" ...
	 */
	for (i = requests.begin(); i != requests.end(); ++i) {
	  if ((*i).second == id2)
	    break;
	}

	if (i == requests.end()) {
	  // ... its the last bind request; answer anyway
	  porb->answer_bind (id2, stat, obj);
	}

	// ignore, there are more outstanding bind requests ...
      }
    }
    break;

  default:
    assert (0);
  }
}

/*
 * Process Callback
 */

void
POAMediatorImpl::callback (MICO::Process * proc,
			   MICO::ProcessCallback::Event ev)
{
  MICOMT::AutoLock l(svmap_lock_);

#ifdef HAVE_THREADS
  // in case of using thread instead of signal handler
  // we cannot delete UnixProcess/Thread object as this
  // is still running when it calls this method so we do not delete
  // it, sometimes we need to do some garbage collecting of finished
  // Processes/Threads. And the time is now.
  MapSvInf::iterator gcit;
  for (gcit = svmap.begin(); gcit != svmap.end(); gcit++) {
    MICOMT::AutoLock l2((*gcit).second.lock);
    if ((*gcit).second.proc != NULL
        && (*gcit).second.proc->finished()) {
        delete (*gcit).second.proc;
        (*gcit).second.proc = 0;
    }
  }
#endif // HAVE_THREADS

  /*
   * Find appropriate server
   */

  MapSvInf::iterator it;
  for (it = svmap.begin(); it != svmap.end(); it++) {
    MICOMT::AutoLock l2((*it).second.lock);
    if ((*it).second.proc == proc) {
      break;
    }
  }

  assert (it != svmap.end());

  MICOMT::AutoLock l2((*it).second.lock);

  /*
   * What's happened?
   */

  switch (ev) {
  case MICO::ProcessCallback::Exited:
    if ((*it).second.pstate == Active) {
      (*it).second.pstate = Inactive;
#ifndef HAVE_THREADS
      // can't delete still running thread object!
      delete (*it).second.proc;
      (*it).second.proc = 0;
#endif // HAVE_THREADS
    }
    else if ((*it).second.pstate == Started) {
      OSMisc::TimeVal ct = OSMisc::gettime();
      cerr << "*** server exited abnormally or could not be run:" << endl
	   << "    name: " << (*it).first << endl;
      (*it).second.pstate = Failed;
      (*it).second.failed = ct.tv_sec;

      /*
       * Fail pending requests
       */

      invqueue.exec_later();
    }
    else if ((*it).second.pstate == Stopped) {
#ifndef HAVE_THREADS
      // can't delete still running thread object!
      delete (*it).second.proc;
      (*it).second.proc = 0;
#endif // HAVE_THREADS
    }
    else if ((*it).second.pstate == Holding) {
#ifndef HAVE_THREADS
      // can't delete still running thread object!
      delete (*it).second.proc;
      (*it).second.proc = 0;
#endif // HAVE_THREADS
      (*it).second.pstate = Stopped;
    }
    else {
      assert (0);
    }
    break;

  default:
    assert (0);
  }
}
