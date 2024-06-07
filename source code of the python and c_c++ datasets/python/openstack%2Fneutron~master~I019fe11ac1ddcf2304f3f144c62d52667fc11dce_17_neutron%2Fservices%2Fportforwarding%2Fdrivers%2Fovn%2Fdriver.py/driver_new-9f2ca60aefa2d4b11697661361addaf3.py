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

from oslo_log import log

from ovsdbapp import constants as ovsdbapp_const

from neutron_lib.callbacks import events
from neutron_lib.callbacks import registry
from neutron_lib.plugins import constants as plugin_constants
from neutron_lib.plugins import directory

from neutron.common.ovn import constants as ovn_const
from neutron.db import ovn_revision_numbers_db as db_rev
from neutron import manager
from neutron.objects import port_forwarding as port_forwarding_obj
from neutron.services.portforwarding.constants import LB_PROTOCOL_MAP
from neutron.services.portforwarding.constants import PORT_FORWARDING
from neutron.services.portforwarding.constants import PORT_FORWARDING_PLUGIN
from neutron.services.portforwarding.constants import PORT_FORWARDING_PREFIX

LOG = log.getLogger(__name__)


class OVNPortForwardingHandler(object):
    @staticmethod
    def _get_lb_protocol(pf_obj):
        return LB_PROTOCOL_MAP[pf_obj.protocol]

    @staticmethod
    def lb_name(fip_id, proto):
        return "{}-{}-{}".format(PORT_FORWARDING_PREFIX, fip_id, proto)

    @classmethod
    def lb_names(cls, fip_id):
        return [cls.lb_name(fip_id, proto)
                for proto in LB_PROTOCOL_MAP.values()]

    @classmethod
    def _get_lb_attributes(cls, pf_obj):
        lb_name = cls.lb_name(pf_obj.floatingip_id,
                              cls._get_lb_protocol(pf_obj))
        vip = "{}:{}".format(pf_obj.floating_ip_address, pf_obj.external_port)
        internal_ip = "{}:{}".format(pf_obj.internal_ip_address,
                                     pf_obj.internal_port)
        rtr_name = 'neutron-{}'.format(pf_obj.router_id)
        return lb_name, vip, [internal_ip], rtr_name

    def port_forwarding_update_revision_number(self, ovn_txn, nb_ovn,
                                               floatingip_id, fip_revision):
        """Update revision number of OVN lb entries based on floatingip id

           A single floating ip maps to 1 or 2 OVN load balancer entries,
           because while multiple vips can exist in a single OVN LB row,
           they represent one protocol. So, to handle all port forwardings
           for a given floating ip, OVN will have up to two LB entries: one
           for udp and one for tcp. These 2 LB entries are expected to have
           the same revision number, in sync with the revision of the floating
           ip. And that is set via this function.
        """
        for iter_lb_name in self.lb_names(floatingip_id):
            LOG.debug("Setting lb for port-forwarding %s to revision %s",
                      iter_lb_name, fip_revision)
            ovn_txn.add(nb_ovn.update_lb_external_ids(
                iter_lb_name,
                {ovn_const.OVN_REV_NUM_EXT_ID_KEY: str(fip_revision)}))

    def port_forwarding_created(self, ovn_txn, nb_ovn, pf_obj,
                                is_update=False):
        if not is_update:
            LOG.info("CREATE for port-forwarding %s vip %s:%s to %s:%s",
                     pf_obj.protocol,
                     pf_obj.floating_ip_address, pf_obj.external_port,
                     pf_obj.internal_ip_address, pf_obj.internal_port)
        # Add vip to its corresponding load balancer. There can be multiple
        # vips, so load balancer may already be present.
        lb_name, vip, internal_ips, rtr_name = self._get_lb_attributes(pf_obj)
        external_ids = {
            ovn_const.OVN_DEVICE_OWNER_EXT_ID_KEY: PORT_FORWARDING_PLUGIN,
            ovn_const.OVN_FIP_EXT_ID_KEY: pf_obj.floatingip_id,
            ovn_const.OVN_ROUTER_NAME_EXT_ID_KEY: rtr_name,
        }
        ovn_txn.add(
            nb_ovn.lb_add(lb_name, vip, internal_ips,
                          self._get_lb_protocol(pf_obj), may_exist=True,
                          external_ids=external_ids))
        # Ensure logical router has load balancer configured.
        ovn_txn.add(nb_ovn.lr_lb_add(rtr_name, lb_name, may_exist=True))

    def port_forwarding_updated(self, ovn_txn, nb_ovn, pf_obj, orig_pf_obj):
        LOG.info("UPDATE for port-forwarding %s vip %s:%s to %s:%s",
                 pf_obj.protocol,
                 pf_obj.floating_ip_address, pf_obj.external_port,
                 pf_obj.internal_ip_address, pf_obj.internal_port)
        self.port_forwarding_deleted(ovn_txn, nb_ovn, orig_pf_obj,
                                     is_update=True)
        self.port_forwarding_created(ovn_txn, nb_ovn, pf_obj,
                                     is_update=True)

    def port_forwarding_deleted(self, ovn_txn, nb_ovn, pf_obj,
                                is_update=False):
        if not is_update:
            LOG.info("DELETE for port-forwarding %s vip %s:%s to %s:%s",
                     pf_obj.protocol,
                     pf_obj.floating_ip_address, pf_obj.external_port,
                     pf_obj.internal_ip_address, pf_obj.internal_port)
        # Note: load balancer instance is expected to be removed by api once
        #       last vip is removed. Since router has weak ref to the lb, that
        #       gets taken care automatically as well.
        lb_name, vip, _internal_ips, _rtr = self._get_lb_attributes(pf_obj)
        ovn_txn.add(nb_ovn.lb_del(lb_name, vip, if_exists=True))


@registry.has_registry_receivers
class OVNPortForwarding(object):

    def __init__(self, l3_plugin):
        self._l3_plugin = l3_plugin
        self._pf_plugin_property = None
        self._handler = OVNPortForwardingHandler()

    @property
    def _pf_plugin(self):
        if self._pf_plugin_property is None:
            self._pf_plugin_property = directory.get_plugin(
                plugin_constants.PORTFORWARDING)
            if not self._pf_plugin_property:
                self._pf_plugin_property = (
                    manager.NeutronManager.load_class_for_provider(
                        'neutron.service_plugins', 'port_forwarding')())
        return self._pf_plugin_property

    def _get_pf_objs(self, context, fip_id):
        pf_dicts = self._pf_plugin.get_floatingip_port_forwardings(
            context, fip_id)
        return[port_forwarding_obj.PortForwarding(context=context, **pf_dict)
               for pf_dict in pf_dicts]

    def _get_fip_objs(self, context, payload):
        floatingip_ids = set()
        for pf_payload in payload:
            if pf_payload.current_pf:
                floatingip_ids.add(pf_payload.current_pf.floatingip_id)
            if pf_payload.original_pf:
                floatingip_ids.add(pf_payload.original_pf.floatingip_id)
        fip_objs = {}
        for floatingip_id in floatingip_ids:
            fip_objs[floatingip_id] = self._l3_plugin.get_floatingip(
                context, floatingip_id)
        return fip_objs

    def _handle_notification(self, _resource, event_type, _pf_plugin, payload):
        if not payload:
            return
        context = payload[0].context
        ovn_nb = self._l3_plugin._ovn
        txn = ovn_nb.transaction
        with txn(check_error=True) as ovn_txn:
            if event_type == events.AFTER_CREATE:
                for pf_payload in payload:
                    self._handler.port_forwarding_created(ovn_txn, ovn_nb,
                        pf_payload.current_pf)
            elif event_type == events.AFTER_UPDATE:
                for pf_payload in payload:
                    self._handler.port_forwarding_updated(ovn_txn, ovn_nb,
                        pf_payload.current_pf, pf_payload.original_pf)
            elif event_type == events.AFTER_DELETE:
                for pf_payload in payload:
                    self._handler.port_forwarding_deleted(ovn_txn, ovn_nb,
                        pf_payload.original_pf)

            # Collect the revision numbers of all floating ips visited and
            # update the corresponding load balancer entries affected.
            # Note that there may be 2 entries for a given floatingip_id;
            # one for each protocol.
            fip_objs = self._get_fip_objs(context, payload)
            for floatingip_id, fip_obj in fip_objs.items():
                fip_revision = fip_obj.get('revision_number', -1)
                self._handler.port_forwarding_update_revision_number(
                    ovn_txn, ovn_nb, floatingip_id, fip_revision)
        # Update revision of affected floating ips. Note that even in
        # cases where port forwarding is deleted, floating ip remains.
        for fip_obj in fip_objs.values():
            db_rev.bump_revision(context, fip_obj, ovn_const.TYPE_FLOATINGIPS)

    def _maintenance_callback(self, context, fip_id, is_delete):
        # NOTE: Since the maintenance callback is not granular to the level
        #       of the affected pfs AND the fact that pfs are all vips
        #       in a load balancer entry, it is cheap enough to simply rebuild.
        ovn_nb = self._l3_plugin._ovn
        txn = ovn_nb.transaction
        pf_objs = [] if is_delete else self._get_pf_objs(context, fip_id)
        LOG.debug("Maintenance port forwarding under fip %s (delete: %s) : %s",
                  fip_id, is_delete, pf_objs)
        with txn(check_error=True) as ovn_txn:
            for lb_name in self._handler.lb_names(fip_id):
                ovn_txn.add(ovn_nb.lb_del(lb_name, vip=None, if_exists=True))

            if not is_delete:
                for pf_obj in pf_objs:
                    self._handler.port_forwarding_created(
                        ovn_txn, ovn_nb, pf_obj)
                fip_obj = self._l3_plugin.get_floatingip(context, fip_id)
                fip_revision = fip_obj.get('revision_number', -1)
                self._handler.port_forwarding_update_revision_number(ovn_txn,
                    ovn_nb, fip_id, fip_revision)

        if not is_delete:
            db_rev.bump_revision(context, fip_obj, ovn_const.TYPE_FLOATINGIPS)

    def maintenance_create(self, context, floatingip):
        fip_id = floatingip['id']
        LOG.info("Maintenance CREATE port-forwarding entries under fip %s",
                 fip_id)
        self._maintenance_callback(context, fip_id, is_delete=False)

    def maintenance_update(self, context, floatingip, _fip_object):
        fip_id = floatingip['id']
        LOG.info("Maintenance UPDATE port-forwarding entries under fip %s",
                 fip_id)
        self._maintenance_callback(context, fip_id, is_delete=False)

    def maintenance_delete(self, context, fip_id, _fip_object):
        LOG.info("Maintenance DELETE port-forwarding entries under fip %s",
                 fip_id)
        self._maintenance_callback(context, fip_id, is_delete=True)

    def db_sync_create_or_update(self, context, fip_id, ovn_txn):
        LOG.info("db_sync UPDATE entries under fip %s", fip_id)
        # NOTE: Since the db_sync callback is not granular to the level
        #       of the affected pfs AND the fact that pfs are all vips
        #       in a load balancer entry, it is cheap enough to simply rebuild.
        ovn_nb = self._l3_plugin._ovn
        pf_objs = self._get_pf_objs(context, fip_id)
        LOG.debug("Db sync port forwarding under fip %s : %s", fip_id, pf_objs)
        for lb_name in self._handler.lb_names(fip_id):
            ovn_txn.add(ovn_nb.lb_del(lb_name, vip=None, if_exists=True))
        for pf_obj in pf_objs:
            self._handler.port_forwarding_created(ovn_txn, ovn_nb, pf_obj)
        fip_obj = self._l3_plugin.get_floatingip(context, fip_id)
        fip_revision = fip_obj.get('revision_number', -1)
        self._handler.port_forwarding_update_revision_number(ovn_txn,
            ovn_nb, fip_id, fip_revision)

    def db_sync_delete(self, context, fip_id, ovn_txn):
        LOG.info("db_sync DELETE entries under fip %s", fip_id)
        ovn_nb = self._l3_plugin._ovn
        for lb_name in self._handler.lb_names(fip_id):
            ovn_txn.add(ovn_nb.lb_del(lb_name, vip=None, if_exists=True))

    @staticmethod
    def ovn_lb_protocol(pf_protocol):
        return LB_PROTOCOL_MAP.get(pf_protocol, ovsdbapp_const.PROTO_TCP)

    @registry.receives(PORT_FORWARDING_PLUGIN, [events.AFTER_INIT])
    def register(self, resource, event, trigger, payload=None):
        for event_type in (events.AFTER_CREATE, events.AFTER_UPDATE,
                           events.AFTER_DELETE):
            registry.subscribe(self._handle_notification, PORT_FORWARDING,
                               event_type)
