/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2016 The OpenSAF Foundation
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
 * Author(s): Ericsson AB
 *
 */

#ifndef BASE_UNIX_SERVER_SOCKET_H_
#define BASE_UNIX_SERVER_SOCKET_H_

#include <string>
#include "base/unix_socket.h"

namespace base {

// A class implementing a non-blocking UNIX domain server socket.
class UnixServerSocket : public UnixSocket {
 public:
  // Set the path name for this server socket. Note that this call does not
  // create the socket - you need to call Send() or Recv() for that to happen.
  explicit UnixServerSocket(const std::string& path);
  // Closes the server socket if it was open, and deletes the socket from the
  // file system.
  virtual ~UnixServerSocket();

 protected:
  virtual bool OpenHook(int sock);
  virtual void CloseHook();
};

}  // namespace base

#endif  // BASE_UNIX_SERVER_SOCKET_H_
