#============================================================================
# This library is free software; you can redistribute it and/or
# modify it under the terms of version 2.1 of the GNU Lesser General Public
# License as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#============================================================================
# Copyright (C) 2006 International Business Machines Corp.
# Author: Reiner Sailer <sailer@us.ibm.com>
# Author: Bryan D. Payne <bdpayne@us.ibm.com>
#============================================================================

"""Labeling a domain configuration file or a resoruce.
"""
import sys, os
import string
import traceback
#from xml.marshal import generic
from xen.util import security

def usage():
    print "\nUsage: xm addlabel <label> dom <configfile> [<policy>]"
    print "       xm addlabel <label> res <resource> [<policy>]\n"
    print "  This program adds an acm_label entry into the 'configfile'"
    print "  for a domain or to the global resource label file for a"
    print "  resource. It derives the policy from the running hypervisor"
    print "  if it is not given (optional parameter). If a label already"
    print "  exists for the given domain or resource, then addlabel fails.\n"
    security.err("Usage")

def validate_config_file(configfile):
    """Performs a simple sanity check on the configuration file passed on
       the command line.  We basically just want to make sure that it's
       not a domain image file so we check for a few configuration values
       and then we are satisfied.  Returned 1 on success, otherwise 0.
    """
    # read in the config file
    globs = {}
    locs = {}
    try:
        execfile(configfile, globs, locs)
    except:
        print "Invalid configuration file."
        return 0

    # sanity check on the data from the file
    count = 0
    required = ['kernel', 'memory', 'name']
    for (k, v) in locs.items():
        if k in required:
            count += 1
    if count != 3:
        print "Invalid configuration file."
        return 0
    else:
        return 1


def add_resource_label(label, resource, policyref):
    """Adds a resource label to the global resource label file.
    """
    try:
        # sanity check: make sure this label can be instantiated later on
        ssidref = security.label2ssidref(label, policyref, 'res')

        # sanity check on resource name
        (type, file) = resource.split(":")
        if type == "phy":
            file = "/dev/" + file
        if not os.path.exists(file):
            print "Invalid resource '"+resource+"'"
            return

        # see if this resource is already in the file
        file = security.res_label_filename
        if not os.path.isfile(file):
            print "Resource file not found, creating new file at:"
            print "%s" % (file)
            fd = open(file, "w")
            fd.close();
            access_control = {}
        else:
            fd = open(file, "rb")
#            access_control = generic.load(fd)
            fd.close()

        if access_control.has_key(resource):
            security.err("This resource is already labeled.")

        # write the data to file
        new_entry = { resource : tuple([policyref, label]) }
        access_control.update(new_entry)
        fd = open(file, "wb")
#        generic.dump(access_control, fd)
        fd.close()

    except security.ACMError:
        pass
    except:
        traceback.print_exc(limit=1)

def add_domain_label(label, configfile, policyref):
    try:
        # sanity checks: make sure this label can be instantiated later on
        ssidref = security.label2ssidref(label, policyref, 'dom')

        new_label = "access_control = ['policy=%s,label=%s']\n" % (policyref, label)
        if not os.path.isfile(configfile):
            security.err("Configuration file \'" + configfile + "\' not found.")
        config_fd = open(configfile, "ra+")
        for line in config_fd:
            if not security.access_control_re.match(line):
                continue
            config_fd.close()
            security.err("Config file \'" + configfile + "\' is already labeled.")
        config_fd.write(new_label)
        config_fd.close()

    except security.ACMError:
        pass
    except:
        traceback.print_exc(limit=1)

def main (argv):
    try:
        policyref = None
        if len(argv) not in [4,5]:
            usage()
        label = argv[1]

        if len(argv) == 5:
            policyref = argv[4]
        elif security.on():
            policyref = security.active_policy
        else:
            security.err("No active policy. Policy must be specified in command line.")

        if argv[2].lower() == "dom":
            configfile = argv[3]
            if configfile[0] != '/':
                for prefix in [".", "/etc/xen"]:
                    configfile = prefix + "/" + configfile
                    if os.path.isfile(configfile):
                        fd = open(configfile, "rb")
                        break
            if not validate_config_file(configfile):
                usage()
            else:
                add_domain_label(label, configfile, policyref)
        elif argv[2].lower() == "res":
            resource = argv[3]
            add_resource_label(label, resource, policyref)
        else:
            usage()

    except security.ACMError:
        pass
    except:
        traceback.print_exc(limit=1)

if __name__ == '__main__':
    main(sys.argv)


