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
# Copyright (C) 2004, 2005 Mike Wray <mike.wray@hp.com>
# Copyright (c) 2006 Xensource Inc.
#============================================================================

import os
import socket
import xen.lowlevel.xc
from xen.xend import uuid
from xen.xend.XendError import XendError
from xen.xend.XendRoot import instance as xendroot
from xen.xend.XendStorageRepository import XendStorageRepository
from xen.xend.XendLogging import log
from xen.xend.XendPIF import *
from xen.xend.XendNetwork import *
from xen.xend.XendStateStore import XendStateStore

class XendNode:
    """XendNode - Represents a Domain 0 Host."""
    
    def __init__(self):
        """Initalises the state of all host specific objects such as

        * Host
        * Host_CPU
        * PIF
        * Network
        * Storage Repository
        """
        
        self.xc = xen.lowlevel.xc.xc()
        self.state_store = XendStateStore(xendroot().get_xend_state_path())

        # load host state from XML file
        saved_host = self.state_store.load_state('host')
        if saved_host and len(saved_host.keys()) == 1:
            self.uuid = saved_host.keys()[0]
            host = saved_host[self.uuid]
            self.name = host.get('name_label', socket.gethostname())
            self.desc = host.get('name_description', '')
            self.cpus = {}
        else:
            self.uuid = uuid.createString()
            self.name = socket.gethostname()
            self.desc = ''
            self.cpus = {}
            
        # load CPU UUIDs
        saved_cpus = self.state_store.load_state('cpu')
        for cpu_uuid, cpu in saved_cpus.items():
            self.cpus[cpu_uuid] = cpu

        # verify we have enough cpus here
        physinfo = self.physinfo_dict()
        cpu_count = physinfo['nr_cpus']
        cpu_features = physinfo['hw_caps']

        # If the number of CPUs don't match, we should just reinitialise 
        # the CPU UUIDs.
        if cpu_count != len(self.cpus):
            self.cpus = {}
            for i in range(cpu_count):
                cpu_uuid = uuid.createString()
                cpu_info = {'uuid': cpu_uuid,
                            'host': self.uuid,
                            'number': i,
                            'features': cpu_features}
                self.cpus[cpu_uuid] = cpu_info

        self.pifs = {}
        self.networks = {}

        # initialise networks
        saved_networks = self.state_store.load_state('network')
        if saved_networks:
            for net_uuid, network in saved_networks.items():
                self.networks[net_uuid] = XendNetwork(net_uuid,
                                network.get('name_label'),
                                network.get('name_description', ''),
                                network.get('default_gateway', ''),
                                network.get('default_netmask', ''))
        else:
            gateway, netmask = linux_get_default_network()
            net_uuid = uuid.createString()
            net = XendNetwork(net_uuid, 'net0', '', gateway, netmask)
            self.networks[net_uuid] = net

        # initialise PIFs
        saved_pifs = self.state_store.load_state('pif')
        if saved_pifs:
            for pif_uuid, pif in saved_pifs.items():
                if pif['network'] in self.networks:
                    network = self.networks[pif['network']]
                    self.pifs[pif_uuid] = XendPIF(pif_uuid,
                                                  pif['name'],
                                                  pif['MTU'],
                                                  pif['MAC'],
                                                  network,
                                                  self)
        else:
            for name, mtu, mac in linux_get_phy_ifaces():
                network = self.networks.values()[0]
                pif_uuid = uuid.createString()
                pif = XendPIF(pif_uuid, name, mtu, mac, network, self)
                self.pifs[pif_uuid] = pif

        # initialise storage
        saved_sr = self.state_store.load_state('sr')
        if saved_sr and len(saved_sr) == 1:
            sr_uuid = saved_sr.keys()[0]
            self.sr = XendStorageRepository(sr_uuid)
        else:
            sr_uuid = uuid.createString()
            self.sr = XendStorageRepository(sr_uuid)

    def save(self):
        # save state
        host_record = {self.uuid: {'name_label':self.name,
                                   'name_description':self.desc}}
        self.state_store.save_state('host',host_record)
        self.state_store.save_state('cpu', self.cpus)
        pif_records = dict([(k, v.get_record(transient = False))
                            for k, v in self.pifs.items()])
        self.state_store.save_state('pif', pif_records)

        self.save_networks()

        sr_record = {self.sr.uuid: self.sr.get_record()}
        self.state_store.save_state('sr', sr_record)

    def save_networks(self):
        net_records = dict([(k, v.get_record(transient = False))
                            for k, v in self.networks.items()])
        self.state_store.save_state('network', net_records)

    def shutdown(self):
        return 0

    def reboot(self):
        return 0

    def notify(self, _):
        return 0

        
    #
    # Ref validation
    #
    
    def is_valid_host(self, host_ref):
        return (host_ref == self.uuid)

    def is_valid_cpu(self, cpu_ref):
        return (cpu_ref in self.cpus)

    def is_valid_network(self, network_ref):
        return (network_ref in self.networks)

    #
    # Storage Repo
    #

    def get_sr(self):
        return self.sr

    #
    # Host Functions
    #

    def xen_version(self):
        info = self.xc.xeninfo()
        try:
            from xen import VERSION
            return {'Xen': '%(xen_major)d.%(xen_minor)d' % info,
                    'Xend': VERSION}
        except (ImportError, AttributeError):
            return {'Xen': '%(xen_major)d.%(xen_minor)d' % info,
                    'Xend': '3.0.3'}

    def get_name(self):
        return self.name

    def set_name(self, new_name):
        self.name = new_name

    def get_description(self):
        return self.desc

    def set_description(self, new_desc):
        self.desc = new_desc

    def get_uuid(self):
        return self.uuid

    #
    # Host CPU Functions
    #

    def get_host_cpu_by_uuid(self, host_cpu_uuid):
        if host_cpu_uuid in self.cpus:
            return host_cpu_uuid
        raise XendError('Invalid CPU UUID')

    def get_host_cpu_refs(self):
        return self.cpus.keys()

    def get_host_cpu_uuid(self, host_cpu_ref):
        if host_cpu_ref in self.cpus:
            return host_cpu_ref
        else:
            raise XendError('Invalid CPU Reference')

    def get_host_cpu_features(self, host_cpu_ref):
        try:
            return self.cpus[host_cpu_ref]['features']
        except KeyError:
            raise XendError('Invalid CPU Reference')

    def get_host_cpu_number(self, host_cpu_ref):
        try:
            return self.cpus[host_cpu_ref]['number']
        except KeyError:
            raise XendError('Invalid CPU Reference')        
            
    def get_host_cpu_load(self, host_cpu_ref):
        return 0.0


    #
    # Network Functions
    #
    
    def get_network_refs(self):
        return self.networks.keys()

    def get_network(self, network_ref):
        return self.networks[network_ref]


    #
    # Getting host information.
    #

    def info(self):
        return (self.nodeinfo() + self.physinfo() + self.xeninfo() +
                self.xendinfo())

    def nodeinfo(self):
        (sys, host, rel, ver, mch) = os.uname()
        return [['system',  sys],
                ['host',    host],
                ['release', rel],
                ['version', ver],
                ['machine', mch]]

    def physinfo(self):
        info = self.xc.physinfo()

        info['nr_cpus'] = (info['nr_nodes'] *
                           info['sockets_per_node'] *
                           info['cores_per_socket'] *
                           info['threads_per_core'])
        info['cpu_mhz'] = info['cpu_khz'] / 1000
        # physinfo is in KiB
        info['total_memory'] = info['total_memory'] / 1024
        info['free_memory']  = info['free_memory'] / 1024

        ITEM_ORDER = ['nr_cpus',
                      'nr_nodes',
                      'sockets_per_node',
                      'cores_per_socket',
                      'threads_per_core',
                      'cpu_mhz',
                      'hw_caps',
                      'total_memory',
                      'free_memory',
                      ]

        return [[k, info[k]] for k in ITEM_ORDER]


    def xeninfo(self):
        info = self.xc.xeninfo()

        ITEM_ORDER = ['xen_major',
                      'xen_minor',
                      'xen_extra',
                      'xen_caps',
                      'xen_pagesize',
                      'platform_params',
                      'xen_changeset',
                      'cc_compiler',
                      'cc_compile_by',
                      'cc_compile_domain',
                      'cc_compile_date',
                      ]

        return [[k, info[k]] for k in ITEM_ORDER]

    def xendinfo(self):
        return [['xend_config_format', 3]]

    # dictionary version of *info() functions to get rid of
    # SXPisms.
    def nodeinfo_dict(self):
        return dict(self.nodeinfo())
    def xendinfo_dict(self):
        return dict(self.xendinfo())
    def xeninfo_dict(self):
        return dict(self.xeninfo())
    def physinfo_dict(self):
        return dict(self.physinfo())
    def info_dict(self):
        return dict(self.info())
    

def instance():
    global inst
    try:
        inst
    except:
        inst = XendNode()
        inst.save()
    return inst
