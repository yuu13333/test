# Copyright 2018 OpenStack Foundation
# All Rights Reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License"); you may
#    not use this file except in compliance with the License. You may obtain
#    a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
#    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
#    License for the specific language governing permissions and limitations
#    under the License.

import collections

import netaddr
from oslo_concurrency import lockutils
from oslo_log import log as logging

from neutron.agent.linux import ip_lib
from neutron.api.rpc.callbacks.consumer import registry
from neutron.api.rpc.callbacks import events
from neutron.api.rpc.callbacks import resources
from neutron.api.rpc.handlers import resources_rpc
from neutron.common import rpc as n_rpc
from neutron_lib.agent import l3_extension
from neutron_lib import constants as lib_constants

LOG = logging.getLogger(__name__)
DEFAULT_PORT_FORWARDING_CHAIN = 'fip-pf'
PORT_FORWARDING_PREFIX = 'fip_portforwarding-'
PORT_FORWARDING_CHAIN_PREFIX = 'pf-'
# This is the same with iptables_manager constant value, but as the value is
# not move to constants file. So define it for port forwarding here.
MAX_CHAIN_LEN_WRAP = 11


class RouterFipPortForwardingMapping(object):
    def __init__(self):
        self.managed_port_forwardings = {}
        """
        fip_port_forwarding = {
            fip_id_1: set(pf_id1, pf_id2),
            fip_id_2: set(pf_id3, pf_id4)
        }
        """
        self.fip_port_forwarding = collections.defaultdict(set)
        """
        router_fip_mapping = {
            router_id_1: set(fip_id_1, fip_id_2),
            router_id_1: set(fip_id_3, fip_id_4)
        }
        """
        self.router_fip_mapping = collections.defaultdict(set)

    def set_port_forwardings(self, port_forwardings):
        for port_forwarding in port_forwardings:
            self.set_fip_port_forwarding(port_forwarding.floating_ip_id,
                                         port_forwarding,
                                         port_forwarding.router_id)

    def update_port_forwardings(self, port_forwardings):
        for port_forwarding in port_forwardings:
            self.managed_port_forwardings[port_forwarding.id] = port_forwarding

    def get_port_forwarding(self, port_forwarding_id):
        return self.managed_port_forwardings.get(port_forwarding_id)

    def del_port_forwardings(self, port_forwardings):
        for port_forwarding in port_forwardings:
            if not self.get_port_forwarding(port_forwarding.id):
                continue
            del self.managed_port_forwardings[port_forwarding.id]
            self.fip_port_forwarding[port_forwarding.floating_ip_id].remove(
                port_forwarding.id)
            if not self.fip_port_forwarding[port_forwarding.floating_ip_id]:
                del self.fip_port_forwarding[port_forwarding.floating_ip_id]
                self.router_fip_mapping[port_forwarding.router_id].remove(
                    port_forwarding.floating_ip_id)
                if not self.router_fip_mapping[port_forwarding.router_id]:
                    del self.router_fip_mapping[port_forwarding.router_id]

    def set_fip_port_forwarding(self, fip_id, pf, router_id):
        self.router_fip_mapping[router_id].add(fip_id)
        self.fip_port_forwarding[fip_id].add(pf.id)
        self.managed_port_forwardings[pf.id] = pf

    def clear_by_fip(self, fip_id, router_id):
        self.router_fip_mapping[router_id].remove(fip_id)
        if len(self.router_fip_mapping[router_id]) == 0:
            del self.router_fip_mapping[router_id]
        for pf_id in self.fip_port_forwarding[fip_id]:
            del self.managed_port_forwardings[pf_id]
        del self.fip_port_forwarding[fip_id]

    def check_port_forwarding_changes(self, new_pf):
        old_pf = self.get_port_forwarding(new_pf.id)
        return old_pf != new_pf


class PortForwardingAgentExtension(l3_extension.L3AgentExtension):
    SUPPORTED_RESOURCE_TYPES = [resources.PORTFORWARDING]

    def initialize(self, connection, driver_type):
        self.resource_rpc = resources_rpc.ResourcesPullRpcApi()
        self._register_rpc_consumers()
        self.mapping = RouterFipPortForwardingMapping()

    def _register_rpc_consumers(self):
        registry.register(self._handle_notification,
                          resources.PORTFORWARDING)

        self._connection = n_rpc.Connection()
        endpoints = [resources_rpc.ResourcesPushRpcCallback()]
        topic = resources_rpc.resource_type_versioned_topic(
            resources.PORTFORWARDING)
        self._connection.create_consumer(topic, endpoints, fanout=True)
        self._connection.consume_in_threads()

    def consume_api(self, agent_api):
        self.agent_api = agent_api

    @lockutils.synchronized('port-forwarding')
    def _handle_notification(self, context, resource_type,
                             forwardings, event_type):
        for forwarding in forwardings:
            self._process_port_forwarding_event(
                context, forwarding, event_type)
            self._store_local([forwarding], event_type)

    def _store_local(self, pf_objs, event_type):
        if event_type == events.CREATED:
            self.mapping.set_port_forwardings(pf_objs)
        elif event_type == events.UPDATED:
            self.mapping.update_port_forwardings(pf_objs)
        elif event_type == events.DELETED:
            self.mapping.del_port_forwardings(pf_objs)

    def _get_fip_rules(self, port_forward, wrap_name):
        chain_rule_list = []
        pf_chain_name = self._get_port_forwarding_chain_name(port_forward.id)
        chain_rule_list.append((DEFAULT_PORT_FORWARDING_CHAIN,
                                '-j %s-%s' %
                                (wrap_name, pf_chain_name)))
        floating_ip_address = str(port_forward.floating_ip_address)
        protocol = port_forward.protocol
        internal_ip_address = str(port_forward.internal_ip_address)
        internal_port = port_forward.internal_port
        external_port = port_forward.external_port
        chain_rule = (pf_chain_name,
                      '-d %s/32 -p %s -m %s --dport %s '
                      '-j DNAT --to-destination %s:%s' % (
                          floating_ip_address, protocol, protocol,
                          external_port, internal_ip_address,
                          internal_port))
        chain_rule_list.append(chain_rule)
        return chain_rule_list

    def _rule_apply(self, iptables_manager, port_forwarding, rule_tag):
        iptables_manager.ipv4['nat'].clear_rules_by_tag(rule_tag)
        if DEFAULT_PORT_FORWARDING_CHAIN not in iptables_manager.ipv4[
             'nat'].chains:
            self._install_default_rules(iptables_manager)

        for chain, rule in self._get_fip_rules(
                port_forwarding, iptables_manager.wrap_name):
            if chain not in iptables_manager.ipv4['nat'].chains:
                iptables_manager.ipv4['nat'].add_chain(chain)
            iptables_manager.ipv4['nat'].add_rule(chain, rule, tag=rule_tag)

    def _process_create(self, port_forwardings, ri, interface_name, namespace,
                        iptables_manager):
        if not port_forwardings:
            return
        device = ip_lib.IPDevice(interface_name, namespace=namespace)

        is_distributed = ri.router.get('distributed')
        ha_port = ri.router.get(lib_constants.HA_INTERFACE_KEY, None)
        fip_statuses = {}
        for port_forwarding in port_forwardings:
            if port_forwarding.id in self.mapping.managed_port_forwardings:
                continue
            existing_cidrs = ri.get_router_cidrs(device)
            fip_ip = str(port_forwarding.floating_ip_address)
            fip_cidr = str(netaddr.IPNetwork(fip_ip))
            if fip_cidr not in existing_cidrs:
                try:
                    if not is_distributed:
                        fip_statuses[
                            port_forwarding.floating_ip_id] = \
                            ri.add_floating_ip(
                                {'floating_ip_address': fip_ip},
                                interface_name, device)
                    else:
                        if not ha_port:
                            device.addr.add(fip_cidr)
                            ip_lib.send_ip_addr_adv_notif(namespace,
                                                          interface_name,
                                                          fip_ip)
                        else:
                            ri._add_vip(fip_cidr, interface_name)
                        fip_statuses[
                            port_forwarding.floating_ip_id] = 'ACTIVE'
                except Exception:
                    # Any error will causes the fip status to be set 'ERROR'
                    fip_statuses[port_forwarding.floating_ip_id] = 'ERROR'
                    LOG.warning("Unable to configure floating IP %(fip_id)s "
                                "for port forwarding %(pf_id)s",
                                {'fip_id': port_forwarding.floating_ip_id,
                                 'pf_id': port_forwarding.id})
            else:
                if not ha_port:
                    ip_lib.send_ip_addr_adv_notif(namespace,
                                                  interface_name,
                                                  fip_ip)

        if ha_port and ha_port['status'] == lib_constants.PORT_STATUS_ACTIVE:
            ri.enable_keepalived()

        for port_forwarding in port_forwardings:
            rule_tag = PORT_FORWARDING_PREFIX + port_forwarding.id
            self._rule_apply(iptables_manager, port_forwarding, rule_tag)

        iptables_manager.apply()
        self._sending_port_forwarding_fip_status(ri, fip_statuses)

    def _sending_port_forwarding_fip_status(self, ri, statuses):
        if not statuses:
            return
        LOG.debug('Sending Port Forwarding floating ip '
                  'statuses: %s', statuses)
        # Update floating IP status on the neutron server
        ri.agent.plugin_rpc.update_floatingip_statuses(
            ri.agent.context, ri.router_id, statuses)

    def _get_resource_by_router(self, ri):
        is_distributed = ri.router.get('distributed')
        ex_gw_port = ri.get_ex_gw_port()
        if not is_distributed:
            interface_name = ri.get_external_device_interface_name(ex_gw_port)
            namespace = ri.ns_name
            iptables_manager = ri.iptables_manager
        else:
            interface_name = ri.get_snat_external_device_interface_name(
                ex_gw_port)
            namespace = ri.snat_namespace.name
            iptables_manager = ri.snat_iptables_manager
        return interface_name, namespace, iptables_manager

    def _check_if_need_process(self, ri):
        if not ri or not ri.get_ex_gw_port():
            # agent not hold the router. pass
            # This router doesn't own a gw port. pass
            return False
        is_distributed = ri.router.get('distributed')
        agent_mode = ri.agent_conf.agent_mode
        if (is_distributed and
                agent_mode in [lib_constants.L3_AGENT_MODE_DVR_NO_EXTERNAL,
                               lib_constants.L3_AGENT_MODE_DVR]):
            # just support centralized cases
            return False
        return True

    def _process_port_forwarding_event(self, context, port_forwarding,
                                       event_type):
        router_id = port_forwarding.router_id
        ri = self._get_router_info(router_id)
        if not self._check_if_need_process(ri):
            return

        (interface_name, namespace,
         iptables_manager) = self._get_resource_by_router(ri)

        if event_type == events.CREATED:
            self._process_create(
                [port_forwarding], ri, interface_name, namespace,
                iptables_manager)
        elif event_type == events.UPDATED:
            self._process_update([port_forwarding], iptables_manager,
                                 interface_name, namespace)
        elif event_type == events.DELETED:
            self._process_delete(
                context, [port_forwarding], ri, interface_name, namespace,
                iptables_manager)

    def _process_update(self, port_forwardings, iptables_manager,
                        interface_name, namespace):
        if not port_forwardings:
            return
        device = ip_lib.IPDevice(interface_name, namespace=namespace)
        for port_forwarding in port_forwardings:
            if not self.mapping.check_port_forwarding_changes(port_forwarding):
                continue
            current_chain = self._get_port_forwarding_chain_name(
                port_forwarding.id)
            iptables_manager.ipv4['nat'].empty_chain(current_chain)
            iptables_manager.ipv4['nat'].remove_chain(current_chain)
            ori_pf = self.mapping.managed_port_forwardings[port_forwarding.id]
            device.delete_socket_conntrack_state(
                str(ori_pf.floating_ip_address), ori_pf.external_port,
                protocol=ori_pf.protocol)
            rule_tag = PORT_FORWARDING_PREFIX + port_forwarding.id
            self._rule_apply(iptables_manager, port_forwarding, rule_tag)
        iptables_manager.apply()

    def _process_delete(self, context, port_forwardings, ri, interface_name,
                        namespace, iptables_manager):
        if not port_forwardings:
            return
        device = ip_lib.IPDevice(interface_name, namespace=namespace)
        for port_forwarding in port_forwardings:
            current_chain = self._get_port_forwarding_chain_name(
                port_forwarding.id)
            iptables_manager.ipv4['nat'].empty_chain(current_chain)
            iptables_manager.ipv4['nat'].remove_chain(current_chain)
            fip_address = str(port_forwarding.floating_ip_address)
            device.delete_socket_conntrack_state(
                fip_address, port_forwarding.external_port,
                protocol=port_forwarding.protocol)

        iptables_manager.apply()

        fip_id_cidrs = set([(pf.floating_ip_id,
                             str(pf.floating_ip_address)) for pf in
                            port_forwardings])
        self._sync_and_remove_fip(context, fip_id_cidrs, device, ri)

    def _sync_and_remove_fip(self, context, fip_id_cidrs, device, ri):
        if not fip_id_cidrs:
            return
        ha_port = ri.router.get(lib_constants.HA_INTERFACE_KEY)
        fip_ids = [item[0] for item in fip_id_cidrs]
        pfs = self.resource_rpc.bulk_pull(context, resources.PORTFORWARDING,
                                          filter_kwargs={
                                              'floating_ip_id': fip_ids})
        exist_fips = collections.defaultdict(set)
        fip_status = {}
        for pf in pfs:
            exist_fips[pf.floating_ip_id].add(pf.id)

        for fip_id_cidr in fip_id_cidrs:
            if fip_id_cidr[0] not in exist_fips:
                if ha_port:
                    ri._remove_vip(fip_id_cidr[1])
                else:
                    device.delete_addr_and_conntrack_state(fip_id_cidr[1])
                fip_status[fip_id_cidr[0]] = 'DOWN'

        if ha_port:
            ri.enable_keepalived()
        self._sending_port_forwarding_fip_status(ri, fip_status)
        for fip_id in fip_status.keys():
            self.mapping.clear_by_fip(fip_id, ri.router_id)

    def _get_router_info(self, router_id):
        router_info = self.agent_api.get_router_info(router_id)
        if router_info:
            return router_info
        LOG.debug("Router %s is not managed by this agent. "
                  "It was possibly deleted concurrently.", router_id)

    def _get_port_forwarding_chain_name(self, pf_id):
        chain_name = PORT_FORWARDING_CHAIN_PREFIX + pf_id
        return chain_name[:MAX_CHAIN_LEN_WRAP]

    def _install_default_rules(self, iptables_manager):
        default_rule = '-j %s-%s' % (iptables_manager.wrap_name,
                                     DEFAULT_PORT_FORWARDING_CHAIN)
        iptables_manager.ipv4['nat'].add_chain(DEFAULT_PORT_FORWARDING_CHAIN)
        iptables_manager.ipv4['nat'].add_rule('PREROUTING', default_rule)
        iptables_manager.apply()

    def check_local_port_forwardings(self, context, ri, sync_mappings):
        fip_ids = [id[0] for id in sync_mappings[
            'floatingip_port_forwarding_mapping']]
        pfs = self.resource_rpc.bulk_pull(context, resources.PORTFORWARDING,
                                          filter_kwargs={
                                              'floating_ip_id': fip_ids})

        (interface_name, namespace,
         iptable_manager) = self._get_resource_by_router(ri)
        local_pfs = set(self.mapping.managed_port_forwardings.keys())
        new_pfs = []
        updated_pfs = []
        current_pfs = set()
        for pf in pfs:
            if pf.id in self.mapping.managed_port_forwardings:
                if self.mapping.check_port_forwarding_changes(pf):
                    updated_pfs.append(pf)
            else:
                new_pfs.append(pf)
            current_pfs.add(pf.id)
        remove_pf_ids_set = local_pfs - current_pfs
        remove_pfs = []
        for local_pf_id in local_pfs:
            if local_pf_id in remove_pf_ids_set:
                remove_pfs.append(
                    self.mapping.managed_port_forwardings[local_pf_id])
        if updated_pfs:
            self._process_update(updated_pfs, iptable_manager,
                                 interface_name, namespace)
            self._store_local(updated_pfs, events.UPDATED)
        if new_pfs:
            self._process_create(new_pfs, ri, interface_name,
                                 namespace, iptable_manager)
            self._store_local(new_pfs, events.CREATED)
        if remove_pfs:
            self._process_delete(context, remove_pfs, ri, interface_name,
                                 namespace, iptable_manager)
            self._store_local(remove_pfs, events.DELETED)

    def process_port_forwarding(self, context, data):
        ri = self._get_router_info(data['id'])
        mappings = ri.port_forwarding_mappings
        if not self._check_if_need_process(ri) or not mappings:
            return

        self.check_local_port_forwardings(context, ri, mappings)

    @lockutils.synchronized('port-forwarding')
    def add_router(self, context, data):
        """Handle a router add event.

        Called on router create.

        :param context: RPC context.
        :param data: Router data.
        """
        self.process_port_forwarding(context, data)

    @lockutils.synchronized('port-forwarding')
    def update_router(self, context, data):
        """Handle a router update event.

        Called on router update.

        :param context: RPC context.
        :param data: Router data.
        """
        self.process_port_forwarding(context, data)

    def delete_router(self, context, data):
        """Handle a router delete event.

        :param context: RPC context.
        :param data: Router data.
        """
        pass

    def ha_state_change(self, context, data):
        pass
