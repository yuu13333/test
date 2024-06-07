# Copyright (c) 2018 OpenStack Foundation
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

import functools

import netaddr

from neutron._i18n import _

from neutron.api.rpc.callbacks import events as rpc_events
from neutron.api.rpc.handlers import resources_rpc
from neutron.db import _resource_extend as resource_extend
from neutron.db import _utils as db_utils
from neutron.db import api as db_api
from neutron.db import db_base_plugin_common
from neutron.extensions import floating_ip_port_forwarding as fip_pf
from neutron.objects import base as base_obj
from neutron.objects import port_forwarding as pf
from neutron.objects import router
from neutron.services.portforwarding.common import exceptions as pf_exc

from neutron_lib.api.definitions import floating_ip_port_forwarding as apidef
from neutron_lib.callbacks import registry
from neutron_lib import constants as lib_conts
from neutron_lib import exceptions as lib_exc
from neutron_lib.exceptions import l3 as lib_l3_exc
from neutron_lib.objects import exceptions as obj_exc
from neutron_lib.plugins import constants
from neutron_lib.plugins import directory


def make_result_with_fields(f):
    @functools.wraps(f)
    def inner(*args, **kwargs):
        fields = kwargs.get('fields')
        result = f(*args, **kwargs)
        if fields is None:
            return result
        elif isinstance(result, list):
            return [db_utils.resource_fields(r, fields) for r in result]
        else:
            return db_utils.resource_fields(result, fields)

    return inner


@resource_extend.has_resource_extenders
@registry.has_registry_receivers
class PortForwardingPlugin(fip_pf.PortForwardingPluginBase):
    """Implementation of the Neutron Port Forwarding Service Plugin.

    This class implements a Port Forwarding plugin.
    """

    supported_extension_aliases = ['floating-ip-port-forwarding']

    __native_pagination_support = True
    __native_sorting_support = True

    def __init__(self):
        super(PortForwardingPlugin, self).__init__()
        self.push_api = resources_rpc.ResourcesPushRpcApi()
        self.l3_plugin = directory.get_plugin(constants.L3)
        self.core_plugin = directory.get_plugin()

    def _get_subnet_check_valid_internal_ip(self, request_ip, exist_fixed_ips):
        for fixed_ip in exist_fixed_ips:
            if netaddr.IPNetwork(
                    fixed_ip['ip_address']) == netaddr.IPNetwork(request_ip):
                return fixed_ip['subnet_id']

    def _find_a_router_for_fippf(self, context, pf_dict, fip_obj):
        internal_port_id = pf_dict['internal_port_id']
        internal_port = self.core_plugin.get_port(context, internal_port_id)
        v4_fixed_ips = [fixed_ip for fixed_ip in internal_port['fixed_ips']
                        if (netaddr.IPNetwork(fixed_ip['ip_address']
                                              ).version ==
                            lib_conts.IP_VERSION_4)]
        # Get the internal ip address, if not specified, choose the first ipv4
        # address.
        internal_ip_address = pf_dict.get('internal_ip_address')
        if not internal_ip_address:
            internal_ip_address = v4_fixed_ips[0]['ip_address']
            pf_dict['internal_ip_address'] = internal_ip_address

        # check the matched fixed ip
        internal_subnet_id = self._get_subnet_check_valid_internal_ip(
            internal_ip_address, v4_fixed_ips)
        if not internal_subnet_id:
            message = _(
                "Requested internal IP address %(internal_ip_address)s is not "
                "suitable for internal neutron port %(internal_port_id)s, as "
                "its fixed_ips are %(fixed_ips)s") % {
                'internal_ip_address': internal_ip_address,
                'internal_port_id': internal_port['id'],
                'fixed_ips': v4_fixed_ips}
            raise lib_exc.BadRequest(resource=apidef.RESOURCE_NAME,
                                     msg=message)

        internal_subnet = self.core_plugin.get_subnet(
            context, internal_subnet_id)
        external_network_id = fip_obj.floating_network_id
        try:
            router_id = self.l3_plugin.get_router_for_floatingip(
                context, internal_port, internal_subnet, external_network_id)
        except lib_l3_exc.ExternalGatewayForFloatingIPNotFound:
            message = _(
                "External network %(external_net_id)s is not reachable from "
                "subnet %(internal_subnet_id)s. Cannot set "
                "Port forwarding for Port %(internal_port_id)s with "
                "Floating IP %(port_forwarding_id)s") % {
                'external_net_id': external_network_id,
                'internal_subnet_id': internal_subnet_id,
                'internal_port_id': internal_port_id,
                'port_forwarding_id': fip_obj.id}
            raise lib_exc.BadRequest(resource=apidef.RESOURCE_NAME,
                                     msg=message)
        return router_id

    @db_base_plugin_common.convert_result_to_dict
    @db_api.context_manager.writer
    def create_floatingip_port_forwarding(self, context, floatingip_id,
                                          port_forwarding):
        port_forwarding = port_forwarding.get(apidef.RESOURCE_NAME)
        port_forwarding['floatingip_id'] = floatingip_id
        pf_obj = pf.PortForwarding(context, **port_forwarding)

        try:
            with db_api.context_manager.writer.using(context):
                fip_obj = router.FloatingIP.get_object(context,
                                                       id=floatingip_id)

                router_id = self._find_a_router_for_fippf(context,
                                                          port_forwarding,
                                                          fip_obj)
                # If this func does not raise an exception, means the
                # router_id matched.
                # case1: fip_obj.router_id = None
                # case2: fip_obj.router_id is the same with we selected.
                self._check_router_match(context, fip_obj,
                                         router_id, port_forwarding)
                if not fip_obj.router_id:
                    fip_obj.router_id = router_id
                    fip_obj.update()
                pf_obj.create()
        except obj_exc.NeutronDbObjectDuplicateEntry:
            (__, conflict_params) = self.try_find_exist(context, floatingip_id,
                                                        port_forwarding)
            message = _("Duplicate portforwarding, a port forwarding with "
                        "same attributes already exists, conflict params like "
                        "%s") % conflict_params
            raise lib_exc.BadRequest(resource=apidef.RESOURCE_NAME,
                                     msg=message)
        self.push_api.push(context, [pf_obj], rpc_events.CREATED)
        return pf_obj

    @db_base_plugin_common.convert_result_to_dict
    def update_floatingip_port_forwarding(self, context, id, floatingip_id,
                                          port_forwarding):
        pf_data = port_forwarding.get(apidef.RESOURCE_NAME)
        new_internal_port_id = None
        if pf_data and pf_data.get('internal_port_id'):
            new_internal_port_id = pf_data.get('internal_port_id')
        try:
            with db_api.context_manager.writer.using(context):
                pf_obj = pf.PortForwarding.get_object(context, id=id)
                if not pf_obj:
                    raise pf_exc.PortForwardingNotFound(id=id)
                ori_internal_port_id = pf_obj.internal_port_id
                if new_internal_port_id and (new_internal_port_id !=
                                             ori_internal_port_id):
                    fip_obj = router.FloatingIP.get_object(context,
                                                           id=floatingip_id)
                    router_id = self._find_a_router_for_fippf(context,
                                                              port_forwarding,
                                                              fip_obj)
                    self._check_router_match(context, fip_obj,
                                             router_id, port_forwarding)

                pf_obj.update_fields(pf_data, reset_changes=True)
                pf_obj.update()
        except obj_exc.NeutronDbObjectDuplicateEntry:
            (__, conflict_params) = self.try_find_exist(context, floatingip_id,
                                                        pf_obj.to_dict())
            message = _("Duplicate portforwarding, a port forwarding with "
                        "same attributes already exists, conflict params like "
                        "%s") % conflict_params
            raise lib_exc.BadRequest(resource=apidef.RESOURCE_NAME,
                                     msg=message)
        self.push_api.push(context, [pf_obj], rpc_events.UPDATED)
        return pf_obj

    def _check_router_match(self, context, fip_obj, router_id, pf_dict):
        internal_port_id = pf_dict['internal_port_id']
        if fip_obj.router_id and fip_obj.router_id != router_id:
            objs = pf.PortForwarding.get_objects(
                context, floatingip_id=fip_obj.id,
                internal_ip_address=pf_dict['internal_ip_address'],
                internal_port=pf_dict['internal_port'])
            if objs:
                message = _("Floating IP %(floatingip_id)s with params: "
                            "internal_ip_address: %(internal_ip_address)s, "
                            "internal_port: %(internal_port)s "
                            "already exists") % {
                    'floatingip_id': fip_obj.id,
                    'internal_ip_address': pf_dict['internal_ip_address'],
                    'internal_port': pf_dict['internal_port']}
            else:
                message = _("The Floating IP %(floatingip_id)s had been set "
                            "on router %(router_id)s, the internal Neutron "
                            "port %(internal_port_id)s can not reach it") % {
                    'floatingip_id': fip_obj.id,
                    'router_id': fip_obj.router_id,
                    'internal_port_id': internal_port_id}
            raise lib_exc.BadRequest(resource=apidef.RESOURCE_NAME,
                                     msg=message)

    def try_find_exist(self, context, floatingip_id, port_forwarding,
                       specify_params=None):
        # Because the session had been flushed by NeutronDbObjectDuplicateEntry
        # so if we want to use the context to get another db queries, we need
        # to rollback first.
        context.session.rollback()
        if not specify_params:
            params = [{'floatingip_id': floatingip_id,
                       'external_port': port_forwarding['external_port']},
                      {'internal_port_id': port_forwarding['internal_port_id'],
                       'internal_ip_address': port_forwarding[
                           'internal_ip_address'],
                       'internal_port': port_forwarding['internal_port']}]
        else:
            params = specify_params
        for param in params:
            objs = pf.PortForwarding.get_objects(context, **param)
            if objs:
                return (objs[0], param)

    def _get_fip_obj(self, context, fip_id):
        fip_obj = router.FloatingIP.get_object(context, id=fip_id)
        if not fip_obj:
            raise lib_l3_exc.FloatingIPNotFound(floatingip_id=fip_id)

    @make_result_with_fields
    @db_base_plugin_common.convert_result_to_dict
    def get_floatingip_port_forwarding(self, context, id, floatingip_id,
                                       fields=None):
        self._get_fip_obj(context, floatingip_id)
        obj = pf.PortForwarding.get_object(context, id=id)
        if not obj:
            raise pf_exc.PortForwardingNotFound(id=id)
        return obj

    def _validate_filter_for_port_forwarding(self, request_filter):
        if not request_filter:
            return
        for filter_member_key in request_filter.keys():
            if filter_member_key in pf.FIELDS_NOT_SUPPORT_FILTER:
                raise pf_exc.PortForwardingNotSupportFilterField(
                    filter=filter_member_key)

    @make_result_with_fields
    @db_base_plugin_common.convert_result_to_dict
    def get_floatingip_port_forwardings(self, context, floatingip_id=None,
                                        filters=None, fields=None, sorts=None,
                                        limit=None, marker=None,
                                        page_reverse=False):
        self._get_fip_obj(context, floatingip_id)
        filters = filters or {}
        self._validate_filter_for_port_forwarding(filters)
        pager = base_obj.Pager(sorts, limit, page_reverse, marker)
        return pf.PortForwarding.get_objects(
            context, _pager=pager, floatingip_id=floatingip_id, **filters)

    def delete_floatingip_port_forwarding(self, context, id, floatingip_id):
        pf_obj = pf.PortForwarding.get_object(context, id=id)
        if not pf_obj:
            raise pf_exc.PortForwardingNotFound(id=id)
        with db_api.context_manager.writer.using(context):
            pf_objs = pf.PortForwarding.get_objects(
                context, floatingip_id=floatingip_id)
            if len(pf_objs) == 1:
                fip_obj = router.FloatingIP.get_object(
                    context, id=pf_obj.floatingip_id)
                fip_obj.update_fields({'router_id': None})
                fip_obj.update()
            pf_obj.delete()
        self.push_api.push(context, [pf_obj], rpc_events.DELETED)

    def _get_port_forwarding_by_routers(self, context, router_ids):
        return pf.PortForwarding.get_port_forwarding_obj_by_routers(
            context, router_ids)

    def sync_port_forwarding_fip(self, context, routers):
        if not routers:
            return

        router_ids = [router.get('id') for router in routers]
        router_pf_fip_set = collections.defaultdict(set)
        fip_pfs = collections.defaultdict(set)
        router_fip = collections.defaultdict(set)
        for (router_id,
             fip_addr, pf_id, fip_id) in self._get_port_forwarding_by_routers(
                context, router_ids):
            router_pf_fip_set[router_id].add(fip_addr + '/32')
            fip_pfs[fip_id].add(pf_id)
            router_fip[router_id].add(fip_id)

        for router in routers:
            if router['id'] in router_fip:
                router['port_forwardings_fip_set'] = router_pf_fip_set[
                    router['id']]
                fip_ids = router_fip[router['id']]
                map_list = []
                for id in fip_ids:
                    map_list.append((id, fip_pfs[id]))
                router['port_forwarding_mappings'] = {
                    'floatingip_port_forwarding_mapping': map_list}
