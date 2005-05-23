"""Spawn a command with pipes to its stdin, stdout, and optionally stderr.

The normal os.popen(cmd, mode) call spawns a shell command and provides a
file interface to just the input or output of the process depending on
whether mode is 'r' or 'w'.  This module provides the functions xpopen2(cmd)
and xpopen3(cmd) which return two or three pipes to the spawned command.
Optionally exclude a list of file descriptors from being closed, allowing
access to those file descriptors from the command.
"""

import os
import sys

try:
    MAXFD = os.sysconf('SC_OPEN_MAX')
except (AttributeError, ValueError):
    MAXFD = 256

_active = []

def _cleanup():
    for inst in _active[:]:
        inst.poll()

class xPopen3:
    """Class representing a child process.  Normally instances are created
    by the factory functions popen2() and popen3()."""

    sts = -1                    # Child not completed yet

    def __init__(self, cmd, capturestderr=False, bufsize=-1, passfd=()):
        """The parameter 'cmd' is the shell command to execute in a
        sub-process.  The 'capturestderr' flag, if true, specifies that
        the object should capture standard error output of the child process.
        The default is false.  If the 'bufsize' parameter is specified, it
        specifies the size of the I/O buffers to/from the child process."""
        _cleanup()
        self.passfd = passfd
        p2cread, p2cwrite = os.pipe()
        c2pread, c2pwrite = os.pipe()
        if capturestderr:
            errout, errin = os.pipe()
        self.pid = os.fork()
        if self.pid == 0:
            # Child
            os.dup2(p2cread, 0)
            os.dup2(c2pwrite, 1)
            if capturestderr:
                os.dup2(errin, 2)
            self._run_child(cmd)
        os.close(p2cread)
        self.tochild = os.fdopen(p2cwrite, 'w', bufsize)
        os.close(c2pwrite)
        self.fromchild = os.fdopen(c2pread, 'r', bufsize)
        if capturestderr:
            os.close(errin)
            self.childerr = os.fdopen(errout, 'r', bufsize)
        else:
            self.childerr = None
        _active.append(self)

    def _run_child(self, cmd):
        if isinstance(cmd, basestring):
            cmd = ['/bin/sh', '-c', cmd]
        for i in range(3, MAXFD):
            if i in self.passfd:
                continue
            try:
                os.close(i)
            except OSError:
                pass
        try:
            os.execvp(cmd[0], cmd)
        finally:
            os._exit(1)

    def poll(self):
        """Return the exit status of the child process if it has finished,
        or -1 if it hasn't finished yet."""
        if self.sts < 0:
            try:
                pid, sts = os.waitpid(self.pid, os.WNOHANG)
                if pid == self.pid:
                    self.sts = sts
                    _active.remove(self)
            except os.error:
                pass
        return self.sts

    def wait(self):
        """Wait for and return the exit status of the child process."""
        if self.sts < 0:
            pid, sts = os.waitpid(self.pid, 0)
            if pid == self.pid:
                self.sts = sts
                _active.remove(self)
        return self.sts


def xpopen2(cmd, bufsize=-1, mode='t', passfd=[]):
    """Execute the shell command 'cmd' in a sub-process.  If 'bufsize' is
    specified, it sets the buffer size for the I/O pipes.  The file objects
    (child_stdout, child_stdin) are returned."""
    inst = xPopen3(cmd, False, bufsize, passfd)
    return inst.fromchild, inst.tochild

def xpopen3(cmd, bufsize=-1, mode='t', passfd=[]):
    """Execute the shell command 'cmd' in a sub-process.  If 'bufsize' is
    specified, it sets the buffer size for the I/O pipes.  The file objects
    (child_stdout, child_stdin, child_stderr) are returned."""
    inst = xPopen3(cmd, True, bufsize, passfd)
    return inst.fromchild, inst.tochild, inst.childerr
