/*
*   Copyright (C) 2016,2018,2020,2021 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "YSFReflector.h"
#include "YSFDefines.h"
#include "BlockList.h"
#include "StopWatch.h"
#include "Network.h"
#include "Version.h"
#include "Thread.h"
#include "Log.h"
#include "GitVersion.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "YSFReflector.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/YSFReflector.ini";
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <cstring>

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "YSFReflector version %s git #%.7s\n", VERSION, gitversion);
				return 0;
			} else if (arg.substr(0, 1) == "-") {
				::fprintf(stderr, "Usage: YSFReflector [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

	CYSFReflector* reflector = new CYSFReflector(std::string(iniFile));
	reflector->run();
	delete reflector;

	return 0;
}

CYSFReflector::CYSFReflector(const std::string& file) :
m_conf(file),
m_repeaters()
{
	CUDPSocket::startup();
}

CYSFReflector::~CYSFReflector()
{
	CUDPSocket::shutdown();
}

void CYSFReflector::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "YSFReflector: cannot read the .ini file\n");
		return;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::fprintf(stderr, "Couldn't fork() , exiting\n");
			return;
		} else if (pid != 0) {
			exit(EXIT_SUCCESS);
		}

		// Create new session and process group
		if (::setsid() == -1) {
			::fprintf(stderr, "Couldn't setsid(), exiting\n");
			return;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1) {
			::fprintf(stderr, "Couldn't cd /, exiting\n");
			return;
		}

		// If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::fprintf(stderr, "Could not get the mmdvm user, exiting\n");
				return;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			// Set user and group ID's to mmdvm:mmdvm
			if (setgid(mmdvm_gid) != 0) {
				::fprintf(stderr, "Could not set mmdvm GID, exiting\n");
				return;
			}

			if (setuid(mmdvm_uid) != 0) {
				::fprintf(stderr, "Could not set mmdvm UID, exiting\n");
				return;
			}

			// Double check it worked (AKA Paranoia)
			if (setuid(0) != -1) {
				::fprintf(stderr, "It's possible to regain root - something is wrong!, exiting\n");
				return;
			}
		}
	}
#endif

#if !defined(_WIN32) && !defined(_WIN64)
        ret = ::LogInitialise(m_daemon, m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel(), m_conf.getLogFileRotate());
#else
        ret = ::LogInitialise(false, m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel(), m_conf.getLogFileRotate());
#endif
	if (!ret) {
		::fprintf(stderr, "YSFReflector: unable to open the log file\n");
		return;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	if (m_daemon) {
		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
	}
#endif

	CNetwork network(m_conf.getNetworkPort(), m_conf.getId(), m_conf.getName(), m_conf.getDescription(), m_conf.getNetworkDebug());

	ret = network.open();
	if (!ret) {
		::LogFinalise();
		return;
	}

	CBlockList blockList(m_conf.getBlockListFile(), m_conf.getBlockListTime());
	blockList.start();

	network.setCount(0);

	CStopWatch stopWatch;
	stopWatch.start();

	CTimer dumpTimer(1000U, 120U);
	dumpTimer.start();

	CTimer pollTimer(1000U, 5U);
	pollTimer.start();

	LogMessage("YSFReflector-%s is starting", VERSION);
	LogMessage("Built %s %s (GitID #%.7s)", __TIME__, __DATE__, gitversion);

	CTimer watchdogTimer(1000U, 0U, 1500U);

	for (;;) {
		unsigned char buffer[200U];
		sockaddr_storage addr;
		unsigned int addrLen;

		unsigned int len = network.readData(buffer, 200U, addr, addrLen);
		if (len > 0U) {
			CYSFRepeater* rpt = findRepeater(addr);
			if (::memcmp(buffer, "YSFP", 4U) == 0) {
				if (rpt == NULL) {
					rpt = new CYSFRepeater;
					rpt->m_callsign = std::string((char*)(buffer + 4U), 10U);
					::memcpy(&rpt->m_addr, &addr, sizeof(struct sockaddr_storage));
					rpt->m_addrLen  = addrLen;
					m_repeaters.push_back(rpt);
					network.setCount(m_repeaters.size());

					char buff[80U];
					LogMessage("Adding %s (%s)", rpt->m_callsign.c_str(), CUDPSocket::display(addr, buff, 80U));
				}
				rpt->m_timer.start();
				network.writePoll(addr, addrLen);
			} else if (::memcmp(buffer + 0U, "YSFU", 4U) == 0 && rpt != NULL) {
				char buff[80U];
				LogMessage("Removing %s (%s) unlinked", rpt->m_callsign.c_str(), CUDPSocket::display(addr, buff, 80U));

				for (std::vector<CYSFRepeater*>::iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
					if (CUDPSocket::match((*it)->m_addr, rpt->m_addr)) {
						delete *it;
						m_repeaters.erase(it);
						break;
					}
				}
				network.setCount(m_repeaters.size());
			} else if (::memcmp(buffer + 0U, "YSFD", 4U) == 0 && rpt != NULL) {
			    unsigned char incomingTag[YSF_CALLSIGN_LENGTH];
                unsigned char incomingSrc[YSF_CALLSIGN_LENGTH];
                unsigned char incomingDst[YSF_CALLSIGN_LENGTH];
                  // Extract metadata from buffer
                  ::memcpy(incomingTag, buffer + 4U, YSF_CALLSIGN_LENGTH);
                  ::memcpy(incomingSrc, buffer + 14U, YSF_CALLSIGN_LENGTH);
                  ::memcpy(incomingDst, buffer + 24U, YSF_CALLSIGN_LENGTH);

                  // Blocklist check (re-check mid-TX if needed)
                  bool isBlocked = false;
                  if (!m_txActive) {
                      isBlocked = blockList.check(incomingSrc);
                  } else {
                      isBlocked = blockList.check(incomingSrc) || blockList.check(m_currentSrc);
                  }

                  if (isBlocked) {
                      if (m_txActive) {
                          m_txActive = false;
                          watchdogTimer.stop();
                          LogMessage("Data from %10.10s at %10.10s blocked", incomingSrc, incomingTag);
                      } else {
                          LogMessage("Data from %10.10s at %10.10s blocked", incomingSrc, incomingTag);
                      }
                      continue;
                  }

                  // TX Lock Logic
                  if (!m_txActive) {
                      // New transmission
                      m_txActive = true;
                      ::memcpy(m_currentTag, incomingTag, YSF_CALLSIGN_LENGTH);
                      ::memcpy(m_currentSrc, incomingSrc, YSF_CALLSIGN_LENGTH);
                      ::memcpy(m_currentDst, incomingDst, YSF_CALLSIGN_LENGTH);
                      ::memcpy(&m_currentAddr, &addr, sizeof(sockaddr_storage));
                      m_currentAddrLen = addrLen;
                      watchdogTimer.start();

                      LogMessage("Transmission from %.10s at %.10s to TG %.10s", m_currentSrc, m_currentTag, m_currentDst);
                  } else {
                      // Check if continuation from same source
                      bool isSameTag = (::memcmp(incomingTag, m_currentTag, YSF_CALLSIGN_LENGTH) == 0);
                      bool isSameRepeater = CUDPSocket::match(addr, m_currentAddr);

                      if (!isSameTag || !isSameRepeater) {
                          LogMessage("Ignoring overlapping TX from %.10s", incomingSrc);
                          continue;
                      }

                      // Update partial metadata
                      if (::memcmp(m_currentSrc, "??????????", YSF_CALLSIGN_LENGTH) == 0)
                          ::memcpy(m_currentSrc, incomingSrc, YSF_CALLSIGN_LENGTH);
                      if (::memcmp(m_currentDst, "??????????", YSF_CALLSIGN_LENGTH) == 0)
                          ::memcpy(m_currentDst, incomingDst, YSF_CALLSIGN_LENGTH);
                  }

                  // Forward data to other repeaters
                  for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
                      if (!CUDPSocket::match((*it)->m_addr, addr))
                          network.writeData(buffer, (*it)->m_addr, (*it)->m_addrLen);
                  }

                  // End-of-TX detection
                  if ((buffer[34U] & 0x01U) == 0x01U) {
                      LogMessage("Received end of transmission from %.10s at %.10s to TG %.10s", m_currentSrc, m_currentTag, m_currentDst);
                      m_txActive = false;
                      watchdogTimer.stop();
                      ::memset(m_currentTag, 0, YSF_CALLSIGN_LENGTH);
                      ::memset(m_currentSrc, 0, YSF_CALLSIGN_LENGTH);
                      ::memset(m_currentDst, 0, YSF_CALLSIGN_LENGTH);
                  }
              }
		}

		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		pollTimer.clock(ms);
		if (pollTimer.hasExpired()) {
			for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it)
				network.writePoll((*it)->m_addr, (*it)->m_addrLen);
			pollTimer.start();
		}

		// Remove any repeaters that haven't reported for a while
		for (std::vector<CYSFRepeater*>::iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it)
			(*it)->m_timer.clock(ms);

		auto it = m_repeaters.begin();
        while (it != m_repeaters.end()) {
            if ((*it)->m_timer.hasExpired()) {
                char buff[80U];
                LogMessage("Removing %s (%s) disappeared", (*it)->m_callsign.c_str(), CUDPSocket::display((*it)->m_addr, buff, 80U));
                delete *it;
                it = m_repeaters.erase(it);
                network.setCount(m_repeaters.size());
            } else {
                ++it;
            }
        }

		watchdogTimer.clock(ms);
		if (watchdogTimer.isRunning() && watchdogTimer.hasExpired()) {
            if (m_txActive) {
                LogMessage("Network watchdog has expired from %.10s at %.10s to TG %.10s", m_currentSrc, m_currentTag, m_currentDst);
                m_txActive = false;
                ::memset(m_currentTag, 0, YSF_CALLSIGN_LENGTH);
                ::memset(m_currentSrc, 0, YSF_CALLSIGN_LENGTH);
                ::memset(m_currentDst, 0, YSF_CALLSIGN_LENGTH);
            }
            watchdogTimer.stop();
        }

		dumpTimer.clock(ms);
		if (dumpTimer.hasExpired()) {
			dumpRepeaters();
			dumpTimer.start();
		}

		blockList.clock(ms);

		if (ms < 5U)
			CThread::sleep(5U);
	}

	network.close();

	::LogFinalise();
}

CYSFRepeater* CYSFReflector::findRepeater(const sockaddr_storage& addr) const
{
	for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
		if (CUDPSocket::match(addr, (*it)->m_addr))
			return *it;
	}

	return NULL;
}

void CYSFReflector::dumpRepeaters() const
{
	if (m_repeaters.size() == 0U) {
		LogMessage("No repeaters linked on TG 226");
		return;
	}

	LogMessage("Currently linked repeaters on TG 226:");

	for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
		char buffer[80U];
		LogMessage("    %s: %s %u/%u", (*it)->m_callsign.c_str(),
									   CUDPSocket::display((*it)->m_addr, buffer, 80U),
									   (*it)->m_timer.getTimer(),
									   (*it)->m_timer.getTimeout());
	}
}
