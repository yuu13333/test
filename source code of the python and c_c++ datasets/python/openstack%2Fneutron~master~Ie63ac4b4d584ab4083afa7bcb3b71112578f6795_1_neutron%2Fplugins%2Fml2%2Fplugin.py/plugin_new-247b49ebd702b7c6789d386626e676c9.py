# Copyright (c) 2013 OpenStack Foundation
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

from eventlet import greenthread
from neutron_lib.api.definitions import extra_dhcp_opt as edo_ext
from neutron_lib.api.definitions import port_security as psec
from neutron_lib.api.definitions import portbindings
from neutron_lib.api.definitions import provider_net
from neutron_lib.api import validators
from neutron_lib.callbacks import events
from neutron_lib.callbacks import exceptions
from neutron_lib.callbacks import registry
from neutron_lib.callbacks import resources
from neutron_lib import constants as const
from neutron_lib import exceptions as exc
from neutron_lib.exceptions import port_security as psec_exc
from neutron_lib.plugins import directory
from neutron_lib.plugins.ml2 import api
from oslo_config import cfg
from oslo_db import exception as os_db_exception
from oslo_log import helpers as log_helpers
from oslo_log import log
from oslo_serialization import jsonutils
from oslo_utils import excutils
from oslo_utils import importutils
from oslo_utils import uuidutils
import sqlalchemy
from sqlalchemy.orm import exc as sa_exc

from neutron._i18n import _, _LE, _LI, _LW
from neutron.agent import securitygroups_rpc as sg_rpc
from neutron.api.rpc.agentnotifiers import dhcp_rpc_agent_api
from neutron.api.rpc.handlers import dhcp_rpc
from neutron.api.rpc.handlers import dvr_rpc
from neutron.api.rpc.handlers import metadata_rpc
from neutron.api.rpc.handlers import resources_rpc
from neutron.api.rpc.handlers import securitygroups_rpc
from neutron.api.v2 import attributes
from neutron.common import constants as n_const
from neutron.common import rpc as n_rpc
from neutron.common import topics
from neutron.common import utils
from neutron.db import _model_query as model_query
from neutron.db import _resource_extend as resource_extend
from neutron.db import _utils as db_utils
from neutron.db import address_scope_db
from neutron.db import agents_db
from neutron.db import agentschedulers_db
from neutron.db import allowedaddresspairs_db as addr_pair_db
from neutron.db import api as db_api
from neutron.db import db_base_plugin_v2
from neutron.db import dvr_mac_db
from neutron.db import external_net_db
from neutron.db import extradhcpopt_db
from neutron.db.models import securitygroup as sg_models
from neutron.db import models_v2
from neutron.db import provisioning_blocks
from neutron.db.quota import driver  # noqa
from neutron.db import securitygroups_rpc_base as sg_db_rpc
from neutron.db import segments_db
from neutron.db import subnet_service_type_db_models as service_type_db
from neutron.db import vlantransparent_db
from neutron.extensions import allowedaddresspairs as addr_pair
from neutron.extensions import availability_zone as az_ext
from neutron.extensions import multiprovidernet as mpnet
from neutron.extensions import providernet as provider
from neutron.extensions import vlantransparent
from neutron.plugins.ml2.common import exceptions as ml2_exc
from neutron.plugins.ml2 import config  # noqa
from neutron.plugins.ml2 import db
from neutron.plugins.ml2 import driver_context
from neutron.plugins.ml2.extensions import qos as qos_ext
from neutron.plugins.ml2 import managers
from neutron.plugins.ml2 import models
from neutron.plugins.ml2 import ovo_rpc
from neutron.plugins.ml2 import rpc
from neutron.quota import resource_registry
from neutron.services.qos import qos_consts
from neutron.services.segments import plugin as segments_plugin

LOG = log.getLogger(__name__)

MAX_BIND_TRIES = 10


SERVICE_PLUGINS_REQUIRED_DRIVERS = {
    'qos': [qos_ext.QOS_EXT_DRIVER_ALIAS]
}


def _ml2_port_result_filter_hook(query, filters):
    values = filters and filters.get(portbindings.HOST_ID, [])
    if not values:
        return query
    bind_criteria = models.PortBinding.host.in_(values)
    return query.filter(models_v2.Port.port_binding.has(bind_criteria))


@resource_extend.has_resource_extenders
@registry.has_registry_receivers
class Ml2Plugin(db_base_plugin_v2.NeutronDbPluginV2,
                dvr_mac_db.DVRDbMixin,
                external_net_db.External_net_db_mixin,
                sg_db_rpc.SecurityGroupServerRpcMixin,
                agentschedulers_db.AZDhcpAgentSchedulerDbMixin,
                addr_pair_db.AllowedAddressPairsMixin,
                vlantransparent_db.Vlantransparent_db_mixin,
                extradhcpopt_db.ExtraDhcpOptMixin,
                address_scope_db.AddressScopeDbMixin,
                service_type_db.SubnetServiceTypeMixin):

    """Implement the Neutron L2 abstractions using modules.

    Ml2Plugin is a Neutron plugin based on separately extensible sets
    of network types and mechanisms for connecting to networks of
    those types. The network types and mechanisms are implemented as
    drivers loaded via Python entry points. Networks can be made up of
    multiple segments (not yet fully implemented).
    """

    # This attribute specifies whether the plugin supports or not
    # bulk/pagination/sorting operations. Name mangling is used in
    # order to ensure it is qualified by class
    __native_bulk_support = True
    __native_pagination_support = True
    __native_sorting_support = True

    # List of supported extensions
    _supported_extension_aliases = ["provider", "external-net", "binding",
                                    "quotas", "security-group", "agent",
                                    "dhcp_agent_scheduler",
                                    "multi-provider", "allowed-address-pairs",
                                    "extra_dhcp_opt", "subnet_allocation",
                                    "net-mtu", "vlan-transparent",
                                    "address-scope",
                                    "availability_zone",
                                    "network_availability_zone",
                                    "default-subnetpools",
                                    "subnet-service-types"]

    @property
    def supported_extension_aliases(self):
        if not hasattr(self, '_aliases'):
            aliases = self._supported_extension_aliases[:]
            aliases += self.extension_manager.extension_aliases()
            sg_rpc.disable_security_group_extension_by_config(aliases)
            vlantransparent.disable_extension_by_config(aliases)
            self._aliases = aliases
        return self._aliases

    def __new__(cls, *args, **kwargs):
        model_query.register_hook(
            models_v2.Port,
            "ml2_port_bindings",
            query_hook=None,
            filter_hook=None,
            result_filters=_ml2_port_result_filter_hook)
        return super(Ml2Plugin, cls).__new__(cls, *args, **kwargs)

    @resource_registry.tracked_resources(
        network=models_v2.Network,
        port=models_v2.Port,
        subnet=models_v2.Subnet,
        subnetpool=models_v2.SubnetPool,
        security_group=sg_models.SecurityGroup,
        security_group_rule=sg_models.SecurityGroupRule)
    def __init__(self):
        # First load drivers, then initialize DB, then initialize drivers
        self.type_manager = managers.TypeManager()
        self.extension_manager = managers.ExtensionManager()
        self.mechanism_manager = managers.MechanismManager()
        super(Ml2Plugin, self).__init__()
        self.type_manager.initialize()
        self.extension_manager.initialize()
        self.mechanism_manager.initialize()
        self._setup_dhcp()
        self._start_rpc_notifiers()
        self.add_agent_status_check_worker(self.agent_health_check)
        self.add_workers(self.mechanism_manager.get_workers())
        self._verify_service_plugins_requirements()
        LOG.info(_LI("Modular L2 Plugin initialization complete"))

    def _setup_rpc(self):
        """Initialize components to support agent communication."""
        self.endpoints = [
            rpc.RpcCallbacks(self.notifier, self.type_manager),
            securitygroups_rpc.SecurityGroupServerRpcCallback(),
            dvr_rpc.DVRServerRpcCallback(),
            dhcp_rpc.DhcpRpcCallback(),
            agents_db.AgentExtRpcCallback(),
            metadata_rpc.MetadataRpcCallback(),
            resources_rpc.ResourcesPullRpcCallback()
        ]

    def _setup_dhcp(self):
        """Initialize components to support DHCP."""
        self.network_scheduler = importutils.import_object(
            cfg.CONF.network_scheduler_driver
        )
        self.add_periodic_dhcp_agent_status_check()

    def _verify_service_plugins_requirements(self):
        for service_plugin in cfg.CONF.service_plugins:
            extension_drivers = SERVICE_PLUGINS_REQUIRED_DRIVERS.get(
                service_plugin, []
            )
            for extension_driver in extension_drivers:
                if extension_driver not in self.extension_manager.names():
                    raise ml2_exc.ExtensionDriverNotFound(
                        driver=extension_driver, service_plugin=service_plugin
                    )

    @registry.receives(resources.PORT,
                       [provisioning_blocks.PROVISIONING_COMPLETE])
    def _port_provisioned(self, rtype, event, trigger, context, object_id,
                          **kwargs):
        port_id = object_id
        port = db.get_port(context, port_id)
        if not port or not port.port_binding:
            LOG.debug("Port %s was deleted so its status cannot be updated.",
                      port_id)
            return
        if port.port_binding.vif_type in (portbindings.VIF_TYPE_BINDING_FAILED,
                                          portbindings.VIF_TYPE_UNBOUND):
            # NOTE(kevinbenton): we hit here when a port is created without
            # a host ID and the dhcp agent notifies that its wiring is done
            LOG.debug("Port %s cannot update to ACTIVE because it "
                      "is not bound.", port_id)
            return
        else:
            # port is bound, but we have to check for new provisioning blocks
            # one last time to detect the case where we were triggered by an
            # unbound port and the port became bound with new provisioning
            # blocks before 'get_port' was called above
            if provisioning_blocks.is_object_blocked(context, port_id,
                                                     resources.PORT):
                LOG.debug("Port %s had new provisioning blocks added so it "
                          "will not transition to active.", port_id)
                return
        self.update_port_status(context, port_id, const.PORT_STATUS_ACTIVE)

    @log_helpers.log_method_call
    def _start_rpc_notifiers(self):
        """Initialize RPC notifiers for agents."""
        self.ovo_notifier = ovo_rpc.OVOServerRpcInterface()
        self.notifier = rpc.AgentNotifierApi(topics.AGENT)
        self.agent_notifiers[const.AGENT_TYPE_DHCP] = (
            dhcp_rpc_agent_api.DhcpAgentNotifyAPI()
        )

    @log_helpers.log_method_call
    def start_rpc_listeners(self):
        """Start the RPC loop to let the plugin communicate with agents."""
        self._setup_rpc()
        self.topic = topics.PLUGIN
        self.conn = n_rpc.create_connection()
        self.conn.create_consumer(self.topic, self.endpoints, fanout=False)
        self.conn.create_consumer(
            topics.SERVER_RESOURCE_VERSIONS,
            [resources_rpc.ResourcesPushToServerRpcCallback()],
            fanout=True)
        # process state reports despite dedicated rpc workers
        self.conn.create_consumer(topics.REPORTS,
                                  [agents_db.AgentExtRpcCallback()],
                                  fanout=False)
        return self.conn.consume_in_threads()

    def start_rpc_state_reports_listener(self):
        self.conn_reports = n_rpc.create_connection()
        self.conn_reports.create_consumer(topics.REPORTS,
                                          [agents_db.AgentExtRpcCallback()],
                                          fanout=False)
        return self.conn_reports.consume_in_threads()

    def _filter_nets_provider(self, context, networks, filters):
        return [network
                for network in networks
                if self.type_manager.network_matches_filters(network, filters)
                ]

    def _check_mac_update_allowed(self, orig_port, port, binding):
        unplugged_types = (portbindings.VIF_TYPE_BINDING_FAILED,
                           portbindings.VIF_TYPE_UNBOUND)
        new_mac = port.get('mac_address')
        mac_change = (new_mac is not None and
                      orig_port['mac_address'] != new_mac)
        if (mac_change and binding.vif_type not in unplugged_types):
            raise exc.PortBound(port_id=orig_port['id'],
                                vif_type=binding.vif_type,
                                old_mac=orig_port['mac_address'],
                                new_mac=port['mac_address'])
        return mac_change

    def _process_port_binding(self, mech_context, attrs):
        plugin_context = mech_context._plugin_context
        binding = mech_context._binding
        port = mech_context.current
        port_id = port['id']
        changes = False

        host = const.ATTR_NOT_SPECIFIED
        if attrs and portbindings.HOST_ID in attrs:
            host = attrs.get(portbindings.HOST_ID) or ''

        original_host = binding.host
        if (validators.is_attr_set(host) and
            original_host != host):
            binding.host = host
            changes = True

        vnic_type = attrs and attrs.get(portbindings.VNIC_TYPE)
        if (validators.is_attr_set(vnic_type) and
            binding.vnic_type != vnic_type):
            binding.vnic_type = vnic_type
            changes = True

        # treat None as clear of profile.
        profile = None
        if attrs and portbindings.PROFILE in attrs:
            profile = attrs.get(portbindings.PROFILE) or {}

        if profile not in (None, const.ATTR_NOT_SPECIFIED,
                           self._get_profile(binding)):
            binding.profile = jsonutils.dumps(profile)
            if len(binding.profile) > models.BINDING_PROFILE_LEN:
                msg = _("binding:profile value too large")
                raise exc.InvalidInput(error_message=msg)
            changes = True

        # Unbind the port if needed.
        if changes:
            binding.vif_type = portbindings.VIF_TYPE_UNBOUND
            binding.vif_details = ''
            db.clear_binding_levels(plugin_context, port_id, original_host)
            mech_context._clear_binding_levels()
            port['status'] = const.PORT_STATUS_DOWN
            super(Ml2Plugin, self).update_port(
                mech_context._plugin_context, port_id,
                {attributes.PORT: {'status': const.PORT_STATUS_DOWN}})

        if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
            binding.vif_type = portbindings.VIF_TYPE_UNBOUND
            binding.vif_details = ''
            db.clear_binding_levels(plugin_context, port_id, original_host)
            mech_context._clear_binding_levels()
            binding.host = ''

        self._update_port_dict_binding(port, binding)
        binding.persist_state_to_session(plugin_context.session)
        return changes

    @db_api.retry_db_errors
    def _bind_port_if_needed(self, context, allow_notify=False,
                             need_notify=False):
        if not context.network.network_segments:
            LOG.debug("Network %s has no segments, skipping binding",
                      context.network.current['id'])
            return context
        for count in range(1, MAX_BIND_TRIES + 1):
            if count > 1:
                # yield for binding retries so that we give other threads a
                # chance to do their work
                greenthread.sleep(0)

                # multiple attempts shouldn't happen very often so we log each
                # attempt after the 1st.
                LOG.info(_LI("Attempt %(count)s to bind port %(port)s"),
                         {'count': count, 'port': context.current['id']})

            bind_context, need_notify, try_again = self._attempt_binding(
                context, need_notify)

            if count == MAX_BIND_TRIES or not try_again:
                if self._should_bind_port(context):
                    # At this point, we attempted to bind a port and reached
                    # its final binding state. Binding either succeeded or
                    # exhausted all attempts, thus no need to try again.
                    # Now, the port and its binding state should be committed.
                    context, need_notify, try_again = (
                        self._commit_port_binding(context, bind_context,
                                                  need_notify, try_again))
                else:
                    context = bind_context

            if not try_again:
                if allow_notify and need_notify:
                    self._notify_port_updated(context)
                return context

        LOG.error(_LE("Failed to commit binding results for %(port)s "
                      "after %(max)s tries"),
                  {'port': context.current['id'], 'max': MAX_BIND_TRIES})
        return context

    def _should_bind_port(self, context):
        return (context._binding.host and context._binding.vif_type
                in (portbindings.VIF_TYPE_UNBOUND,
                    portbindings.VIF_TYPE_BINDING_FAILED))

    def _attempt_binding(self, context, need_notify):
        try_again = False

        if self._should_bind_port(context):
            bind_context = self._bind_port(context)

            if bind_context.vif_type != portbindings.VIF_TYPE_BINDING_FAILED:
                # Binding succeeded. Suggest notifying of successful binding.
                need_notify = True
            else:
                # Current attempt binding failed, try to bind again.
                try_again = True
            context = bind_context

        return context, need_notify, try_again

    def _bind_port(self, orig_context):
        # Construct a new PortContext from the one from the previous
        # transaction.
        port = orig_context.current
        orig_binding = orig_context._binding
        new_binding = models.PortBinding(
            host=orig_binding.host,
            vnic_type=orig_binding.vnic_type,
            profile=orig_binding.profile,
            vif_type=portbindings.VIF_TYPE_UNBOUND,
            vif_details=''
        )
        self._update_port_dict_binding(port, new_binding)
        new_context = driver_context.PortContext(
            self, orig_context._plugin_context, port,
            orig_context.network.current, new_binding, None,
            original_port=orig_context.original)

        # Attempt to bind the port and return the context with the
        # result.
        self.mechanism_manager.bind_port(new_context)
        return new_context

    def _commit_port_binding(self, orig_context, bind_context,
                             need_notify, try_again):
        port_id = orig_context.current['id']
        plugin_context = orig_context._plugin_context
        orig_binding = orig_context._binding
        new_binding = bind_context._binding

        # After we've attempted to bind the port, we begin a
        # transaction, get the current port state, and decide whether
        # to commit the binding results.
        with db_api.context_manager.writer.using(plugin_context):
            # Get the current port state and build a new PortContext
            # reflecting this state as original state for subsequent
            # mechanism driver update_port_*commit() calls.
            try:
                port_db = self._get_port(plugin_context, port_id)
                cur_binding = port_db.port_binding
            except exc.PortNotFound:
                port_db, cur_binding = None, None
            if not port_db or not cur_binding:
                # The port has been deleted concurrently, so just
                # return the unbound result from the initial
                # transaction that completed before the deletion.
                LOG.debug("Port %s has been deleted concurrently", port_id)
                return orig_context, False, False
            # Since the mechanism driver bind_port() calls must be made
            # outside a DB transaction locking the port state, it is
            # possible (but unlikely) that the port's state could change
            # concurrently while these calls are being made. If another
            # thread or process succeeds in binding the port before this
            # thread commits its results, the already committed results are
            # used. If attributes such as binding:host_id, binding:profile,
            # or binding:vnic_type are updated concurrently, the try_again
            # flag is returned to indicate that the commit was unsuccessful.
            oport = self._make_port_dict(port_db)
            port = self._make_port_dict(port_db)
            network = bind_context.network.current
            if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
                # REVISIT(rkukura): The PortBinding instance from the
                # ml2_port_bindings table, returned as cur_binding
                # from port_db.port_binding above, is
                # currently not used for DVR distributed ports, and is
                # replaced here with the DistributedPortBinding instance from
                # the ml2_distributed_port_bindings table specific to the host
                # on which the distributed port is being bound. It
                # would be possible to optimize this code to avoid
                # fetching the PortBinding instance in the DVR case,
                # and even to avoid creating the unused entry in the
                # ml2_port_bindings table. But the upcoming resolution
                # for bug 1367391 will eliminate the
                # ml2_distributed_port_bindings table, use the
                # ml2_port_bindings table to store non-host-specific
                # fields for both distributed and non-distributed
                # ports, and introduce a new ml2_port_binding_hosts
                # table for the fields that need to be host-specific
                # in the distributed case. Since the PortBinding
                # instance will then be needed, it does not make sense
                # to optimize this code to avoid fetching it.
                cur_binding = db.get_distributed_port_binding_by_host(
                    plugin_context, port_id, orig_binding.host)
            cur_context = driver_context.PortContext(
                self, plugin_context, port, network, cur_binding, None,
                original_port=oport)

            # Commit our binding results only if port has not been
            # successfully bound concurrently by another thread or
            # process and no binding inputs have been changed.
            commit = ((cur_binding.vif_type in
                       [portbindings.VIF_TYPE_UNBOUND,
                        portbindings.VIF_TYPE_BINDING_FAILED]) and
                      orig_binding.host == cur_binding.host and
                      orig_binding.vnic_type == cur_binding.vnic_type and
                      orig_binding.profile == cur_binding.profile)

            if commit:
                # Update the port's binding state with our binding
                # results.
                cur_binding.vif_type = new_binding.vif_type
                cur_binding.vif_details = new_binding.vif_details
                db.clear_binding_levels(plugin_context, port_id,
                                        cur_binding.host)
                db.set_binding_levels(plugin_context,
                                      bind_context._binding_levels)
                # refresh context with a snapshot of updated state
                cur_context._binding = driver_context.InstanceSnapshot(
                    cur_binding)
                cur_context._binding_levels = bind_context._binding_levels

                # Update PortContext's port dictionary to reflect the
                # updated binding state.
                self._update_port_dict_binding(port, cur_binding)

                # Update the port status if requested by the bound driver.
                if (bind_context._binding_levels and
                    bind_context._new_port_status):
                    port_db.status = bind_context._new_port_status
                    port['status'] = bind_context._new_port_status

                # Call the mechanism driver precommit methods, commit
                # the results, and call the postcommit methods.
                self.mechanism_manager.update_port_precommit(cur_context)
        if commit:
            # Continue, using the port state as of the transaction that
            # just finished, whether that transaction committed new
            # results or discovered concurrent port state changes.
            # Also, Trigger notification for successful binding commit.
            kwargs = {
                'context': plugin_context,
                'port': self._make_port_dict(port_db),  # ensure latest state
                'mac_address_updated': False,
                'original_port': oport,
            }
            registry.notify(resources.PORT, events.AFTER_UPDATE,
                            self, **kwargs)
            self.mechanism_manager.update_port_postcommit(cur_context)
            need_notify = True
            try_again = False
        else:
            try_again = True

        return cur_context, need_notify, try_again

    def _update_port_dict_binding(self, port, binding):
        port[portbindings.VNIC_TYPE] = binding.vnic_type
        port[portbindings.PROFILE] = self._get_profile(binding)
        if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
            port[portbindings.HOST_ID] = ''
            port[portbindings.VIF_TYPE] = portbindings.VIF_TYPE_DISTRIBUTED
            port[portbindings.VIF_DETAILS] = {}
        else:
            port[portbindings.HOST_ID] = binding.host
            port[portbindings.VIF_TYPE] = binding.vif_type
            port[portbindings.VIF_DETAILS] = self._get_vif_details(binding)

    def _get_vif_details(self, binding):
        if binding.vif_details:
            try:
                return jsonutils.loads(binding.vif_details)
            except Exception:
                LOG.error(_LE("Serialized vif_details DB value '%(value)s' "
                              "for port %(port)s is invalid"),
                          {'value': binding.vif_details,
                           'port': binding.port_id})
        return {}

    def _get_profile(self, binding):
        if binding.profile:
            try:
                return jsonutils.loads(binding.profile)
            except Exception:
                LOG.error(_LE("Serialized profile DB value '%(value)s' for "
                              "port %(port)s is invalid"),
                          {'value': binding.profile,
                           'port': binding.port_id})
        return {}

    @staticmethod
    @resource_extend.extends([attributes.PORTS])
    def _ml2_extend_port_dict_binding(port_res, port_db):
        plugin = directory.get_plugin()
        # None when called during unit tests for other plugins.
        if port_db.port_binding:
            plugin._update_port_dict_binding(port_res, port_db.port_binding)

    # ML2's resource extend functions allow extension drivers that extend
    # attributes for the resources to add those attributes to the result.

    @staticmethod
    @resource_extend.extends([attributes.NETWORKS])
    def _ml2_md_extend_network_dict(result, netdb):
        plugin = directory.get_plugin()
        session = plugin._object_session_or_new_session(netdb)
        plugin.extension_manager.extend_network_dict(session, netdb, result)

    @staticmethod
    @resource_extend.extends([attributes.PORTS])
    def _ml2_md_extend_port_dict(result, portdb):
        plugin = directory.get_plugin()
        session = plugin._object_session_or_new_session(portdb)
        plugin.extension_manager.extend_port_dict(session, portdb, result)

    @staticmethod
    @resource_extend.extends([attributes.SUBNETS])
    def _ml2_md_extend_subnet_dict(result, subnetdb):
        plugin = directory.get_plugin()
        session = plugin._object_session_or_new_session(subnetdb)
        plugin.extension_manager.extend_subnet_dict(session, subnetdb, result)

    @staticmethod
    def _object_session_or_new_session(sql_obj):
        session = sqlalchemy.inspect(sql_obj).session
        if not session:
            session = db_api.get_reader_session()
        return session

    def _notify_port_updated(self, mech_context):
        port = mech_context.current
        segment = mech_context.bottom_bound_segment
        if not segment:
            # REVISIT(rkukura): This should notify agent to unplug port
            network = mech_context.network.current
            LOG.debug("In _notify_port_updated(), no bound segment for "
                      "port %(port_id)s on network %(network_id)s",
                      {'port_id': port['id'], 'network_id': network['id']})
            return
        self.notifier.port_update(mech_context._plugin_context, port,
                                  segment[api.NETWORK_TYPE],
                                  segment[api.SEGMENTATION_ID],
                                  segment[api.PHYSICAL_NETWORK])

    def _delete_objects(self, context, resource, objects):
        delete_op = getattr(self, 'delete_%s' % resource)
        for obj in objects:
            try:
                delete_op(context, obj['result']['id'])
            except KeyError:
                LOG.exception(_LE("Could not find %s to delete."),
                              resource)
            except Exception:
                LOG.exception(_LE("Could not delete %(res)s %(id)s."),
                              {'res': resource,
                               'id': obj['result']['id']})

    def _create_bulk_ml2(self, resource, context, request_items):
        objects = []
        collection = "%ss" % resource
        items = request_items[collection]
        obj_before_create = getattr(self, '_before_create_%s' % resource)
        for item in items:
            obj_before_create(context, item)
        with db_api.context_manager.writer.using(context):
            obj_creator = getattr(self, '_create_%s_db' % resource)
            for item in items:
                try:
                    attrs = item[resource]
                    result, mech_context = obj_creator(context, item)
                    objects.append({'mech_context': mech_context,
                                    'result': result,
                                    'attributes': attrs})

                except Exception as e:
                    with excutils.save_and_reraise_exception():
                        utils.attach_exc_details(
                            e, _LE("An exception occurred while creating "
                                   "the %(resource)s:%(item)s"),
                            {'resource': resource, 'item': item})

        postcommit_op = getattr(self, '_after_create_%s' % resource)
        for obj in objects:
            try:
                postcommit_op(context, obj['result'], obj['mech_context'])
            except Exception:
                with excutils.save_and_reraise_exception():
                    resource_ids = [res['result']['id'] for res in objects]
                    LOG.exception(_LE("ML2 _after_create_%(res)s "
                                      "failed for %(res)s: "
                                      "'%(failed_id)s'. Deleting "
                                      "%(res)ss %(resource_ids)s"),
                                  {'res': resource,
                                   'failed_id': obj['result']['id'],
                                   'resource_ids': ', '.join(resource_ids)})
                    # _after_handler will have deleted the object that threw
                    to_delete = [o for o in objects if o != obj]
                    self._delete_objects(context, resource, to_delete)
        return objects

    def _get_network_mtu(self, network):
        mtus = []
        try:
            segments = network[mpnet.SEGMENTS]
        except KeyError:
            segments = [network]
        for s in segments:
            segment_type = s[provider_net.NETWORK_TYPE]
            try:
                type_driver = self.type_manager.drivers[segment_type].obj
            except KeyError:
                # NOTE(ihrachys) This can happen when type driver is not loaded
                # for an existing segment, or simply when the network has no
                # segments at the specific time this is computed.
                # In the former case, while it's probably an indication of
                # a bad setup, it's better to be safe than sorry here. Also,
                # several unit tests use non-existent driver types that may
                # trigger the exception here.
                if segment_type and s[provider_net.SEGMENTATION_ID]:
                    LOG.warning(
                        _LW("Failed to determine MTU for segment "
                            "%(segment_type)s:%(segment_id)s; network "
                            "%(network_id)s MTU calculation may be not "
                            "accurate"),
                        {
                            'segment_type': segment_type,
                            'segment_id': s[provider_net.SEGMENTATION_ID],
                            'network_id': network['id'],
                        }
                    )
            else:
                mtu = type_driver.get_mtu(s[provider_net.PHYSICAL_NETWORK])
                # Some drivers, like 'local', may return None; the assumption
                # then is that for the segment type, MTU has no meaning or
                # unlimited, and so we should then ignore those values.
                if mtu:
                    mtus.append(mtu)
        return min(mtus) if mtus else 0

    def _before_create_network(self, context, network):
        net_data = network[attributes.NETWORK]
        registry.notify(resources.NETWORK, events.BEFORE_CREATE, self,
                        context=context, network=net_data)

    def _create_network_db(self, context, network):
        net_data = network[attributes.NETWORK]
        tenant_id = net_data['tenant_id']
        with db_api.context_manager.writer.using(context):
            net_db = self.create_network_db(context, network)
            result = self._make_network_dict(net_db, process_extensions=False,
                                             context=context)
            self.extension_manager.process_create_network(context, net_data,
                                                          result)
            self._process_l3_create(context, result, net_data)
            net_data['id'] = result['id']
            self.type_manager.create_network_segments(context, net_data,
                                                      tenant_id)
            self.type_manager.extend_network_dict_provider(context, result)
            # Update the transparent vlan if configured
            if utils.is_extension_supported(self, 'vlan-transparent'):
                vlt = vlantransparent.get_vlan_transparent(net_data)
                net_db['vlan_transparent'] = vlt
                result['vlan_transparent'] = vlt

            result[api.MTU] = self._get_network_mtu(result)

            if az_ext.AZ_HINTS in net_data:
                self.validate_availability_zones(context, 'network',
                                                 net_data[az_ext.AZ_HINTS])
                az_hints = az_ext.convert_az_list_to_string(
                                                net_data[az_ext.AZ_HINTS])
                net_db[az_ext.AZ_HINTS] = az_hints
                result[az_ext.AZ_HINTS] = az_hints
            registry.notify(resources.NETWORK, events.PRECOMMIT_CREATE, self,
                            context=context, request=net_data, network=result)

            resource_extend.apply_funcs('networks', result, net_db)
            mech_context = driver_context.NetworkContext(self, context,
                                                         result)
            self.mechanism_manager.create_network_precommit(mech_context)
        return result, mech_context

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def create_network(self, context, network):
        self._before_create_network(context, network)
        result, mech_context = self._create_network_db(context, network)
        return self._after_create_network(context, result, mech_context)

    def _after_create_network(self, context, result, mech_context):
        kwargs = {'context': context, 'network': result}
        registry.notify(resources.NETWORK, events.AFTER_CREATE, self, **kwargs)
        try:
            self.mechanism_manager.create_network_postcommit(mech_context)
        except ml2_exc.MechanismDriverError:
            with excutils.save_and_reraise_exception():
                LOG.error(_LE("mechanism_manager.create_network_postcommit "
                              "failed, deleting network '%s'"), result['id'])
                self.delete_network(context, result['id'])

        return result

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def create_network_bulk(self, context, networks):
        objects = self._create_bulk_ml2(attributes.NETWORK, context, networks)
        return [obj['result'] for obj in objects]

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def update_network(self, context, id, network):
        net_data = network[attributes.NETWORK]
        provider._raise_if_updates_provider_attributes(net_data)

        with db_api.context_manager.writer.using(context):
            original_network = super(Ml2Plugin, self).get_network(context, id)
            updated_network = super(Ml2Plugin, self).update_network(context,
                                                                    id,
                                                                    network)
            self.extension_manager.process_update_network(context, net_data,
                                                          updated_network)
            self._process_l3_update(context, updated_network, net_data)
            self.type_manager.extend_network_dict_provider(context,
                                                           updated_network)

            # ToDO(QoS): This would change once EngineFacade moves out
            db_network = self._get_network(context, id)
            # Expire the db_network in current transaction, so that the join
            # relationship can be updated.
            context.session.expire(db_network)
            updated_network = self._make_network_dict(
                db_network, context=context)

            kwargs = {'context': context, 'network': updated_network,
                      'original_network': original_network,
                      'request': net_data}
            registry.notify(
                resources.NETWORK, events.PRECOMMIT_UPDATE, self, **kwargs)

            # TODO(QoS): Move out to the extension framework somehow.
            need_network_update_notify = (
                qos_consts.QOS_POLICY_ID in net_data and
                original_network[qos_consts.QOS_POLICY_ID] !=
                updated_network[qos_consts.QOS_POLICY_ID])

            mech_context = driver_context.NetworkContext(
                self, context, updated_network,
                original_network=original_network)
            self.mechanism_manager.update_network_precommit(mech_context)

        # TODO(apech) - handle errors raised by update_network, potentially
        # by re-calling update_network with the previous attributes. For
        # now the error is propagated to the caller, which is expected to
        # either undo/retry the operation or delete the resource.
        kwargs = {'context': context, 'network': updated_network,
                  'original_network': original_network}
        registry.notify(resources.NETWORK, events.AFTER_UPDATE, self, **kwargs)
        self.mechanism_manager.update_network_postcommit(mech_context)
        if need_network_update_notify:
            self.notifier.network_update(context, updated_network)
        return updated_network

    @db_api.retry_if_session_inactive()
    def get_network(self, context, id, fields=None):
        with db_api.context_manager.reader.using(context):
            result = super(Ml2Plugin, self).get_network(context, id, None)
            self.type_manager.extend_network_dict_provider(context, result)
            result[api.MTU] = self._get_network_mtu(result)

        return db_utils.resource_fields(result, fields)

    @db_api.retry_if_session_inactive()
    def get_networks(self, context, filters=None, fields=None,
                     sorts=None, limit=None, marker=None, page_reverse=False):
        with db_api.context_manager.reader.using(context):
            nets = super(Ml2Plugin,
                         self).get_networks(context, filters, None, sorts,
                                            limit, marker, page_reverse)
            self.type_manager.extend_networks_dict_provider(context, nets)

            nets = self._filter_nets_provider(context, nets, filters)

            for net in nets:
                net[api.MTU] = self._get_network_mtu(net)

        return [db_utils.resource_fields(net, fields) for net in nets]

    def get_network_contexts(self, context, network_ids):
        """Return a map of network_id to NetworkContext for network_ids."""
        net_filters = {'id': list(set(network_ids))}
        nets_by_netid = {
            n['id']: n for n in self.get_networks(context,
                                                  filters=net_filters)
        }
        segments_by_netid = segments_db.get_networks_segments(
            context, list(nets_by_netid.keys()))
        netctxs_by_netid = {
            net_id: driver_context.NetworkContext(
                self, context, nets_by_netid[net_id],
                segments=segments_by_netid[net_id])
            for net_id in nets_by_netid.keys()
        }
        return netctxs_by_netid

    @utils.transaction_guard
    def delete_network(self, context, id):
        # the only purpose of this override is to protect this from being
        # called inside of a transaction.
        return super(Ml2Plugin, self).delete_network(context, id)

    @registry.receives(resources.NETWORK, [events.PRECOMMIT_DELETE])
    def _network_delete_precommit_handler(self, rtype, event, trigger,
                                          context, network_id, **kwargs):
        network = self.get_network(context, network_id)
        mech_context = driver_context.NetworkContext(self,
                                                     context,
                                                     network)
        # TODO(kevinbenton): move this mech context into something like
        # a 'delete context' so it's not polluting the real context object
        setattr(context, '_mech_context', mech_context)
        self.mechanism_manager.delete_network_precommit(
            mech_context)

    @registry.receives(resources.NETWORK, [events.AFTER_DELETE])
    def _network_delete_after_delete_handler(self, rtype, event, trigger,
                                             context, network, **kwargs):
        try:
            self.mechanism_manager.delete_network_postcommit(
                context._mech_context)
        except ml2_exc.MechanismDriverError:
            # TODO(apech) - One or more mechanism driver failed to
            # delete the network.  Ideally we'd notify the caller of
            # the fact that an error occurred.
            LOG.error(_LE("mechanism_manager.delete_network_postcommit"
                          " failed"))
        self.notifier.network_delete(context, network['id'])

    def _before_create_subnet(self, context, subnet):
        # TODO(kevinbenton): BEFORE notification should be added here
        pass

    def _create_subnet_db(self, context, subnet):
        with db_api.context_manager.writer.using(context):
            result, net_db, ipam_sub = self._create_subnet_precommit(
                context, subnet)
            self.extension_manager.process_create_subnet(
                context, subnet[attributes.SUBNET], result)
            network = self._make_network_dict(net_db, context=context)
            self.type_manager.extend_network_dict_provider(context, network)
            network[api.MTU] = self._get_network_mtu(network)
            mech_context = driver_context.SubnetContext(self, context,
                                                        result, network)
            self.mechanism_manager.create_subnet_precommit(mech_context)

        # TODO(kevinbenton): move this to '_after_subnet_create'
        # db base plugin post commit ops
        self._create_subnet_postcommit(context, result, net_db, ipam_sub)

        return result, mech_context

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def create_subnet(self, context, subnet):
        self._before_create_subnet(context, subnet)
        result, mech_context = self._create_subnet_db(context, subnet)
        return self._after_create_subnet(context, result, mech_context)

    def _after_create_subnet(self, context, result, mech_context):
        kwargs = {'context': context, 'subnet': result}
        registry.notify(resources.SUBNET, events.AFTER_CREATE, self, **kwargs)
        try:
            self.mechanism_manager.create_subnet_postcommit(mech_context)
        except ml2_exc.MechanismDriverError:
            with excutils.save_and_reraise_exception():
                LOG.error(_LE("mechanism_manager.create_subnet_postcommit "
                              "failed, deleting subnet '%s'"), result['id'])
                self.delete_subnet(context, result['id'])
        return result

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def create_subnet_bulk(self, context, subnets):
        objects = self._create_bulk_ml2(attributes.SUBNET, context, subnets)
        return [obj['result'] for obj in objects]

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def update_subnet(self, context, id, subnet):
        with db_api.context_manager.writer.using(context):
            updated_subnet, original_subnet = self._update_subnet_precommit(
                context, id, subnet)
            self.extension_manager.process_update_subnet(
                context, subnet[attributes.SUBNET], updated_subnet)
            updated_subnet = self.get_subnet(context, id)
            mech_context = driver_context.SubnetContext(
                self, context, updated_subnet, network=None,
                original_subnet=original_subnet)
            self.mechanism_manager.update_subnet_precommit(mech_context)

        self._update_subnet_postcommit(context, original_subnet,
                                       updated_subnet)
        # TODO(apech) - handle errors raised by update_subnet, potentially
        # by re-calling update_subnet with the previous attributes. For
        # now the error is propagated to the caller, which is expected to
        # either undo/retry the operation or delete the resource.
        self.mechanism_manager.update_subnet_postcommit(mech_context)
        return updated_subnet

    @utils.transaction_guard
    def delete_subnet(self, context, id):
        # the only purpose of this override is to protect this from being
        # called inside of a transaction.
        return super(Ml2Plugin, self).delete_subnet(context, id)

    @registry.receives(resources.SUBNET, [events.PRECOMMIT_DELETE])
    def _subnet_delete_precommit_handler(self, rtype, event, trigger,
                                         context, subnet_id, **kwargs):
        record = self._get_subnet(context, subnet_id)
        subnet = self._make_subnet_dict(record, context=context)
        network = self.get_network(context, subnet['network_id'])
        mech_context = driver_context.SubnetContext(self, context,
                                                    subnet, network)
        # TODO(kevinbenton): move this mech context into something like
        # a 'delete context' so it's not polluting the real context object
        setattr(context, '_mech_context', mech_context)
        self.mechanism_manager.delete_subnet_precommit(mech_context)

    @registry.receives(resources.SUBNET, [events.AFTER_DELETE])
    def _subnet_delete_after_delete_handler(self, rtype, event, trigger,
                                            context, subnet, **kwargs):
        try:
            self.mechanism_manager.delete_subnet_postcommit(
                context._mech_context)
        except ml2_exc.MechanismDriverError:
            # TODO(apech) - One or more mechanism driver failed to
            # delete the subnet.  Ideally we'd notify the caller of
            # the fact that an error occurred.
            LOG.error(_LE("mechanism_manager.delete_subnet_postcommit failed"))

    # TODO(yalei) - will be simplified after security group and address pair be
    # converted to ext driver too.
    def _portsec_ext_port_create_processing(self, context, port_data, port):
        attrs = port[attributes.PORT]
        port_security = ((port_data.get(psec.PORTSECURITY) is None) or
                         port_data[psec.PORTSECURITY])

        # allowed address pair checks
        if self._check_update_has_allowed_address_pairs(port):
            if not port_security:
                raise addr_pair.AddressPairAndPortSecurityRequired()
        else:
            # remove ATTR_NOT_SPECIFIED
            attrs[addr_pair.ADDRESS_PAIRS] = []

        if port_security:
            self._ensure_default_security_group_on_port(context, port)
        elif self._check_update_has_security_groups(port):
            raise psec_exc.PortSecurityAndIPRequiredForSecurityGroups()

    def _setup_dhcp_agent_provisioning_component(self, context, port):
        subnet_ids = [f['subnet_id'] for f in port['fixed_ips']]
        if (db.is_dhcp_active_on_any_subnet(context, subnet_ids) and
            len(self.get_dhcp_agents_hosting_networks(context,
                                                      [port['network_id']]))):
            # the agents will tell us when the dhcp config is ready so we setup
            # a provisioning component to prevent the port from going ACTIVE
            # until a dhcp_ready_on_port notification is received.
            provisioning_blocks.add_provisioning_component(
                context, port['id'], resources.PORT,
                provisioning_blocks.DHCP_ENTITY)
        else:
            provisioning_blocks.remove_provisioning_component(
                context, port['id'], resources.PORT,
                provisioning_blocks.DHCP_ENTITY)

    def _before_create_port(self, context, port):
        attrs = port[attributes.PORT]
        if not attrs.get('status'):
            attrs['status'] = const.PORT_STATUS_DOWN

        registry.notify(resources.PORT, events.BEFORE_CREATE, self,
                        context=context, port=attrs)
        # NOTE(kevinbenton): triggered outside of transaction since it
        # emits 'AFTER' events if it creates.
        self._ensure_default_security_group(context, attrs['tenant_id'])

    def _create_port_db(self, context, port):
        attrs = port[attributes.PORT]
        with db_api.context_manager.writer.using(context):
            dhcp_opts = attrs.get(edo_ext.EXTRADHCPOPTS, [])
            port_db = self.create_port_db(context, port)
            result = self._make_port_dict(port_db, process_extensions=False)
            self.extension_manager.process_create_port(context, attrs, result)
            self._portsec_ext_port_create_processing(context, result, port)

            # sgids must be got after portsec checked with security group
            sgids = self._get_security_groups_on_port(context, port)
            self._process_port_create_security_group(context, result, sgids)
            network = self.get_network(context, result['network_id'])
            binding = db.add_port_binding(context, result['id'])
            mech_context = driver_context.PortContext(self, context, result,
                                                      network, binding, None)
            self._process_port_binding(mech_context, attrs)

            result[addr_pair.ADDRESS_PAIRS] = (
                self._process_create_allowed_address_pairs(
                    context, result,
                    attrs.get(addr_pair.ADDRESS_PAIRS)))
            self._process_port_create_extra_dhcp_opts(context, result,
                                                      dhcp_opts)
            kwargs = {'context': context, 'port': result}
            registry.notify(
                resources.PORT, events.PRECOMMIT_CREATE, self, **kwargs)
            self.mechanism_manager.create_port_precommit(mech_context)
            self._setup_dhcp_agent_provisioning_component(context, result)

        resource_extend.apply_funcs('ports', result, port_db)
        return result, mech_context

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def create_port(self, context, port):
        self._before_create_port(context, port)
        result, mech_context = self._create_port_db(context, port)
        return self._after_create_port(context, result, mech_context)

    def _after_create_port(self, context, result, mech_context):
        # notify any plugin that is interested in port create events
        kwargs = {'context': context, 'port': result}
        registry.notify(resources.PORT, events.AFTER_CREATE, self, **kwargs)

        try:
            self.mechanism_manager.create_port_postcommit(mech_context)
        except ml2_exc.MechanismDriverError:
            with excutils.save_and_reraise_exception():
                LOG.error(_LE("mechanism_manager.create_port_postcommit "
                              "failed, deleting port '%s'"), result['id'])
                self.delete_port(context, result['id'], l3_port_check=False)
        try:
            bound_context = self._bind_port_if_needed(mech_context)
        except ml2_exc.MechanismDriverError:
            with excutils.save_and_reraise_exception():
                LOG.error(_LE("_bind_port_if_needed "
                              "failed, deleting port '%s'"), result['id'])
                self.delete_port(context, result['id'], l3_port_check=False)

        return bound_context.current

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def create_port_bulk(self, context, ports):
        objects = self._create_bulk_ml2(attributes.PORT, context, ports)
        return [obj['result'] for obj in objects]

    # TODO(yalei) - will be simplified after security group and address pair be
    # converted to ext driver too.
    def _portsec_ext_port_update_processing(self, updated_port, context, port,
                                            id):
        port_security = ((updated_port.get(psec.PORTSECURITY) is None) or
                         updated_port[psec.PORTSECURITY])

        if port_security:
            return

        # check the address-pairs
        if self._check_update_has_allowed_address_pairs(port):
            #  has address pairs in request
            raise addr_pair.AddressPairAndPortSecurityRequired()
        elif (not
         self._check_update_deletes_allowed_address_pairs(port)):
            # not a request for deleting the address-pairs
            updated_port[addr_pair.ADDRESS_PAIRS] = (
                    self.get_allowed_address_pairs(context, id))

            # check if address pairs has been in db, if address pairs could
            # be put in extension driver, we can refine here.
            if updated_port[addr_pair.ADDRESS_PAIRS]:
                raise addr_pair.AddressPairAndPortSecurityRequired()

        # checks if security groups were updated adding/modifying
        # security groups, port security is set
        if self._check_update_has_security_groups(port):
            raise psec_exc.PortSecurityAndIPRequiredForSecurityGroups()
        elif (not
          self._check_update_deletes_security_groups(port)):
            if not utils.is_extension_supported(self, 'security-group'):
                return
            # Update did not have security groups passed in. Check
            # that port does not have any security groups already on it.
            filters = {'port_id': [id]}
            security_groups = (
                super(Ml2Plugin, self)._get_port_security_group_bindings(
                        context, filters)
                     )
            if security_groups:
                raise psec_exc.PortSecurityPortHasSecurityGroup()

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def update_port(self, context, id, port):
        attrs = port[attributes.PORT]
        need_port_update_notify = False
        bound_mech_contexts = []
        with db_api.context_manager.writer.using(context):
            port_db = self._get_port(context, id)
            binding = port_db.port_binding
            if not binding:
                raise exc.PortNotFound(port_id=id)
            mac_address_updated = self._check_mac_update_allowed(
                port_db, attrs, binding)
            need_port_update_notify |= mac_address_updated
            original_port = self._make_port_dict(port_db)
            updated_port = super(Ml2Plugin, self).update_port(context, id,
                                                              port)
            self.extension_manager.process_update_port(context, attrs,
                                                       updated_port)
            self._portsec_ext_port_update_processing(updated_port, context,
                                                     port, id)

            if (psec.PORTSECURITY in attrs) and (
                        original_port[psec.PORTSECURITY] !=
                        updated_port[psec.PORTSECURITY]):
                need_port_update_notify = True
            # TODO(QoS): Move out to the extension framework somehow.
            # Follow https://review.openstack.org/#/c/169223 for a solution.
            if (qos_consts.QOS_POLICY_ID in attrs and
                    original_port[qos_consts.QOS_POLICY_ID] !=
                    updated_port[qos_consts.QOS_POLICY_ID]):
                need_port_update_notify = True

            if addr_pair.ADDRESS_PAIRS in attrs:
                need_port_update_notify |= (
                    self.update_address_pairs_on_port(context, id, port,
                                                      original_port,
                                                      updated_port))
            need_port_update_notify |= self.update_security_group_on_port(
                context, id, port, original_port, updated_port)
            network = self.get_network(context, original_port['network_id'])
            need_port_update_notify |= self._update_extra_dhcp_opts_on_port(
                context, id, port, updated_port)
            levels = db.get_binding_levels(context, id, binding.host)
            # one of the operations above may have altered the model call
            # _make_port_dict again to ensure latest state is reflected so mech
            # drivers, callback handlers, and the API caller see latest state.
            # We expire here to reflect changed relationships on the obj.
            # Repeatable read will ensure we still get the state from this
            # transaction in spite of concurrent updates/deletes.
            context.session.expire(port_db)
            updated_port.update(self._make_port_dict(port_db))
            mech_context = driver_context.PortContext(
                self, context, updated_port, network, binding, levels,
                original_port=original_port)
            need_port_update_notify |= self._process_port_binding(
                mech_context, attrs)

            kwargs = {
                'context': context,
                'port': updated_port,
                'original_port': original_port,
            }
            registry.notify(
                resources.PORT, events.PRECOMMIT_UPDATE, self, **kwargs)

            # For DVR router interface ports we need to retrieve the
            # DVRPortbinding context instead of the normal port context.
            # The normal Portbinding context does not have the status
            # of the ports that are required by the l2pop to process the
            # postcommit events.

            # NOTE:Sometimes during the update_port call, the DVR router
            # interface port may not have the port binding, so we cannot
            # create a generic bindinglist that will address both the
            # DVR and non-DVR cases here.
            # TODO(Swami): This code need to be revisited.
            if port_db['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
                dist_binding_list = db.get_distributed_port_bindings(context,
                                                                     id)
                for dist_binding in dist_binding_list:
                    levels = db.get_binding_levels(context, id,
                                                   dist_binding.host)
                    dist_mech_context = driver_context.PortContext(
                        self, context, updated_port, network,
                        dist_binding, levels, original_port=original_port)
                    self.mechanism_manager.update_port_precommit(
                        dist_mech_context)
                    bound_mech_contexts.append(dist_mech_context)
            else:
                self.mechanism_manager.update_port_precommit(mech_context)
                self._setup_dhcp_agent_provisioning_component(
                    context, updated_port)
                bound_mech_contexts.append(mech_context)

        # Commit hooks may have modified the port object. Reload it.
        port_db = self._get_port(context, id)
        updated_port.update(self._make_port_dict(port_db))

        # Notifications must be sent after the above transaction is complete
        kwargs = {
            'context': context,
            'port': updated_port,
            'mac_address_updated': mac_address_updated,
            'original_port': original_port,
        }
        registry.notify(resources.PORT, events.AFTER_UPDATE, self, **kwargs)

        # Note that DVR Interface ports will have bindings on
        # multiple hosts, and so will have multiple mech_contexts,
        # while other ports typically have just one.
        # Since bound_mech_contexts has both the DVR and non-DVR
        # contexts we can manage just with a single for loop.
        try:
            for mech_context in bound_mech_contexts:
                self.mechanism_manager.update_port_postcommit(
                    mech_context)
        except ml2_exc.MechanismDriverError:
            LOG.error(_LE("mechanism_manager.update_port_postcommit "
                          "failed for port %s"), id)

        self.check_and_notify_security_group_member_changed(
            context, original_port, updated_port)
        need_port_update_notify |= self.is_security_group_member_updated(
            context, original_port, updated_port)

        if original_port['admin_state_up'] != updated_port['admin_state_up']:
            need_port_update_notify = True
        # NOTE: In the case of DVR ports, the port-binding is done after
        # router scheduling when sync_routers is called and so this call
        # below may not be required for DVR routed interfaces. But still
        # since we don't have the mech_context for the DVR router interfaces
        # at certain times, we just pass the port-context and return it, so
        # that we don't disturb other methods that are expecting a return
        # value.
        bound_context = self._bind_port_if_needed(
            mech_context,
            allow_notify=True,
            need_notify=need_port_update_notify)
        return bound_context.current

    def _process_distributed_port_binding(self, mech_context, context, attrs):
        plugin_context = mech_context._plugin_context
        binding = mech_context._binding
        port = mech_context.current
        port_id = port['id']

        if binding.vif_type != portbindings.VIF_TYPE_UNBOUND:
            binding.vif_details = ''
            binding.vif_type = portbindings.VIF_TYPE_UNBOUND
            if binding.host:
                db.clear_binding_levels(plugin_context, port_id, binding.host)
            binding.host = ''

        self._update_port_dict_binding(port, binding)
        binding.host = attrs and attrs.get(portbindings.HOST_ID)
        binding.router_id = attrs and attrs.get('device_id')
        # merge into session to reflect changes
        binding.persist_state_to_session(plugin_context.session)

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def update_distributed_port_binding(self, context, id, port):
        attrs = port[attributes.PORT]

        host = attrs and attrs.get(portbindings.HOST_ID)
        host_set = validators.is_attr_set(host)

        if not host_set:
            LOG.error(_LE("No Host supplied to bind DVR Port %s"), id)
            return

        binding = db.get_distributed_port_binding_by_host(context,
                                                          id, host)
        device_id = attrs and attrs.get('device_id')
        router_id = binding and binding.get('router_id')
        update_required = (not binding or
            binding.vif_type == portbindings.VIF_TYPE_BINDING_FAILED or
            router_id != device_id)
        if update_required:
            try:
                with db_api.context_manager.writer.using(context):
                    orig_port = self.get_port(context, id)
                    if not binding:
                        binding = db.ensure_distributed_port_binding(
                            context, id, host, router_id=device_id)
                    network = self.get_network(context,
                                               orig_port['network_id'])
                    levels = db.get_binding_levels(context, id, host)
                    mech_context = driver_context.PortContext(self,
                        context, orig_port, network,
                        binding, levels, original_port=orig_port)
                    self._process_distributed_port_binding(
                        mech_context, context, attrs)
            except (os_db_exception.DBReferenceError, exc.PortNotFound):
                LOG.debug("DVR Port %s has been deleted concurrently", id)
                return
            self._bind_port_if_needed(mech_context)

    def _pre_delete_port(self, context, port_id, port_check):
        """Do some preliminary operations before deleting the port."""
        LOG.debug("Deleting port %s", port_id)
        try:
            # notify interested parties of imminent port deletion;
            # a failure here prevents the operation from happening
            kwargs = {
                'context': context,
                'port_id': port_id,
                'port_check': port_check
            }
            registry.notify(
                resources.PORT, events.BEFORE_DELETE, self, **kwargs)
        except exceptions.CallbackFailure as e:
            # NOTE(armax): preserve old check's behavior
            if len(e.errors) == 1:
                raise e.errors[0].error
            raise exc.ServicePortInUse(port_id=port_id, reason=e)

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def delete_port(self, context, id, l3_port_check=True):
        self._pre_delete_port(context, id, l3_port_check)
        # TODO(armax): get rid of the l3 dependency in the with block
        router_ids = []
        l3plugin = directory.get_plugin(const.L3)

        with db_api.context_manager.writer.using(context):
            try:
                port_db = self._get_port(context, id)
                binding = port_db.port_binding
            except exc.PortNotFound:
                LOG.debug("The port '%s' was deleted", id)
                return
            port = self._make_port_dict(port_db)

            network = self.get_network(context, port['network_id'])
            bound_mech_contexts = []
            device_owner = port['device_owner']
            if device_owner == const.DEVICE_OWNER_DVR_INTERFACE:
                bindings = db.get_distributed_port_bindings(context,
                                                            id)
                for bind in bindings:
                    levels = db.get_binding_levels(context, id,
                                                   bind.host)
                    mech_context = driver_context.PortContext(
                        self, context, port, network, bind, levels)
                    self.mechanism_manager.delete_port_precommit(mech_context)
                    bound_mech_contexts.append(mech_context)
            else:
                levels = db.get_binding_levels(context, id,
                                               binding.host)
                mech_context = driver_context.PortContext(
                    self, context, port, network, binding, levels)
                self.mechanism_manager.delete_port_precommit(mech_context)
                bound_mech_contexts.append(mech_context)
            if l3plugin:
                router_ids = l3plugin.disassociate_floatingips(
                    context, id, do_notify=False)

            LOG.debug("Calling delete_port for %(port_id)s owned by %(owner)s",
                      {"port_id": id, "owner": device_owner})
            super(Ml2Plugin, self).delete_port(context, id)

        self._post_delete_port(
            context, port, router_ids, bound_mech_contexts)

    def _post_delete_port(
        self, context, port, router_ids, bound_mech_contexts):
        kwargs = {
            'context': context,
            'port': port,
            'router_ids': router_ids,
        }
        registry.notify(resources.PORT, events.AFTER_DELETE, self, **kwargs)
        try:
            # Note that DVR Interface ports will have bindings on
            # multiple hosts, and so will have multiple mech_contexts,
            # while other ports typically have just one.
            for mech_context in bound_mech_contexts:
                self.mechanism_manager.delete_port_postcommit(mech_context)
        except ml2_exc.MechanismDriverError:
            # TODO(apech) - One or more mechanism driver failed to
            # delete the port.  Ideally we'd notify the caller of the
            # fact that an error occurred.
            LOG.error(_LE("mechanism_manager.delete_port_postcommit failed for"
                          " port %s"), port['id'])
        self.notifier.port_delete(context, port['id'])

    @utils.transaction_guard
    @db_api.retry_if_session_inactive(context_var_name='plugin_context')
    def get_bound_port_context(self, plugin_context, port_id, host=None,
                               cached_networks=None):
        with db_api.context_manager.reader.using(plugin_context) as session:
            try:
                port_db = (session.query(models_v2.Port).
                           enable_eagerloads(False).
                           filter(models_v2.Port.id.startswith(port_id)).
                           one())
            except sa_exc.NoResultFound:
                LOG.info(_LI("No ports have port_id starting with %s"),
                         port_id)
                return
            except sa_exc.MultipleResultsFound:
                LOG.error(_LE("Multiple ports have port_id starting with %s"),
                          port_id)
                return
            port = self._make_port_dict(port_db)
            network = (cached_networks or {}).get(port['network_id'])

            if not network:
                network = self.get_network(plugin_context, port['network_id'])

            if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
                binding = db.get_distributed_port_binding_by_host(
                    plugin_context, port['id'], host)
                if not binding:
                    LOG.error(_LE("Binding info for DVR port %s not found"),
                              port_id)
                    return None
                levels = db.get_binding_levels(plugin_context,
                                               port_db.id, host)
                port_context = driver_context.PortContext(
                    self, plugin_context, port, network, binding, levels)
            else:
                # since eager loads are disabled in port_db query
                # related attribute port_binding could disappear in
                # concurrent port deletion.
                # It's not an error condition.
                binding = port_db.port_binding
                if not binding:
                    LOG.info(_LI("Binding info for port %s was not found, "
                                 "it might have been deleted already."),
                             port_id)
                    return
                levels = db.get_binding_levels(plugin_context, port_db.id,
                                               port_db.port_binding.host)
                port_context = driver_context.PortContext(
                    self, plugin_context, port, network, binding, levels)

        return self._bind_port_if_needed(port_context)

    @utils.transaction_guard
    @db_api.retry_if_session_inactive(context_var_name='plugin_context')
    def get_bound_ports_contexts(self, plugin_context, dev_ids, host=None):
        result = {}
        with db_api.context_manager.reader.using(plugin_context):
            dev_to_full_pids = db.partial_port_ids_to_full_ids(
                plugin_context, dev_ids)
            # get all port objects for IDs
            port_dbs_by_id = db.get_port_db_objects(
                plugin_context, dev_to_full_pids.values())
            # get all networks for PortContext construction
            netctxs_by_netid = self.get_network_contexts(
                plugin_context,
                {p.network_id for p in port_dbs_by_id.values()})
            for dev_id in dev_ids:
                port_id = dev_to_full_pids.get(dev_id)
                port_db = port_dbs_by_id.get(port_id)
                if (not port_id or not port_db or
                        port_db.network_id not in netctxs_by_netid):
                    result[dev_id] = None
                    continue
                port = self._make_port_dict(port_db)
                if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
                    binding = db.get_distributed_port_binding_by_host(
                        plugin_context, port['id'], host)
                    bindlevelhost_match = host
                else:
                    binding = port_db.port_binding
                    bindlevelhost_match = binding.host if binding else None
                if not binding:
                    LOG.info(_LI("Binding info for port %s was not found, "
                                 "it might have been deleted already."),
                             port_id)
                    result[dev_id] = None
                    continue
                levels = [l for l in port_db.binding_levels
                          if l.host == bindlevelhost_match]
                levels = sorted(levels, key=lambda l: l.level)
                network_ctx = netctxs_by_netid.get(port_db.network_id)
                port_context = driver_context.PortContext(
                    self, plugin_context, port, network_ctx, binding, levels)
                result[dev_id] = port_context

        return {d: self._bind_port_if_needed(pctx) if pctx else None
                for d, pctx in result.items()}

    def update_port_status(self, context, port_id, status, host=None,
                           network=None):
        """
        Returns port_id (non-truncated uuid) if the port exists.
        Otherwise returns None.
        'network' is deprecated and has no effect
        """
        full = db.partial_port_ids_to_full_ids(context, [port_id])
        if port_id not in full:
            return None
        port_id = full[port_id]
        return self.update_port_statuses(
            context, {port_id: status}, host)[port_id]

    @utils.transaction_guard
    @db_api.retry_if_session_inactive()
    def update_port_statuses(self, context, port_id_to_status, host=None):
        result = {}
        port_ids = port_id_to_status.keys()
        port_dbs_by_id = db.get_port_db_objects(context, port_ids)
        for port_id, status in port_id_to_status.items():
            if not port_dbs_by_id.get(port_id):
                LOG.debug("Port %(port)s update to %(val)s by agent not found",
                          {'port': port_id, 'val': status})
                result[port_id] = None
                continue
            result[port_id] = self._safe_update_individual_port_db_status(
                context, port_dbs_by_id[port_id], status, host)
        return result

    def _safe_update_individual_port_db_status(self, context, port,
                                               status, host):
        port_id = port.id
        try:
            return self._update_individual_port_db_status(
                context, port, status, host)
        except Exception:
            with excutils.save_and_reraise_exception() as ectx:
                # don't reraise if port doesn't exist anymore
                ectx.reraise = bool(db.get_port(context, port_id))

    def _update_individual_port_db_status(self, context, port, status, host):
        updated = False
        network = None
        port_id = port.id
        with db_api.context_manager.writer.using(context):
            context.session.add(port)  # bring port into writer session
            if (port.status != status and
                port['device_owner'] != const.DEVICE_OWNER_DVR_INTERFACE):
                original_port = self._make_port_dict(port)
                port.status = status
                # explicit flush before _make_port_dict to ensure extensions
                # listening for db events can modify the port if necessary
                context.session.flush()
                updated_port = self._make_port_dict(port)
                levels = db.get_binding_levels(context, port.id,
                                               port.port_binding.host)
                mech_context = driver_context.PortContext(
                    self, context, updated_port, network, port.port_binding,
                    levels, original_port=original_port)
                self.mechanism_manager.update_port_precommit(mech_context)
                updated = True
            elif port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
                binding = db.get_distributed_port_binding_by_host(
                    context, port['id'], host)
                if not binding:
                    return
                binding.status = status
                updated = True

        if (updated and
            port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE):
            with db_api.context_manager.writer.using(context):
                port = db.get_port(context, port_id)
                if not port:
                    LOG.warning(_LW("Port %s not found during update"),
                                port_id)
                    return
                original_port = self._make_port_dict(port)
                network = network or self.get_network(
                    context, original_port['network_id'])
                port.status = db.generate_distributed_port_status(context,
                                                                  port['id'])
                updated_port = self._make_port_dict(port)
                levels = db.get_binding_levels(context, port_id, host)
                mech_context = (driver_context.PortContext(
                    self, context, updated_port, network,
                    binding, levels, original_port=original_port))
                self.mechanism_manager.update_port_precommit(mech_context)

        if updated:
            self.mechanism_manager.update_port_postcommit(mech_context)
            kwargs = {'context': context, 'port': mech_context.current,
                      'original_port': original_port}
            if status == const.PORT_STATUS_ACTIVE:
                # NOTE(kevinbenton): this kwarg was carried over from
                # the RPC handler that used to call this. it's not clear
                # who uses it so maybe it can be removed. added in commit
                # 3f3874717c07e2b469ea6c6fd52bcb4da7b380c7
                kwargs['update_device_up'] = True
            registry.notify(resources.PORT, events.AFTER_UPDATE, self,
                            **kwargs)

        if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
            db.delete_distributed_port_binding_if_stale(context, binding)

        return port['id']

    @db_api.retry_if_session_inactive()
    def port_bound_to_host(self, context, port_id, host):
        if not host:
            return
        port = db.get_port(context, port_id)
        if not port:
            LOG.debug("No Port match for: %s", port_id)
            return
        if port['device_owner'] == const.DEVICE_OWNER_DVR_INTERFACE:
            bindings = db.get_distributed_port_bindings(context,
                                                        port_id)
            for b in bindings:
                if b.host == host:
                    return port
            LOG.debug("No binding found for DVR port %s", port['id'])
            return
        else:
            port_host = db.get_port_binding_host(context, port_id)
            return port if (port_host == host) else None

    @db_api.retry_if_session_inactive()
    def get_ports_from_devices(self, context, devices):
        port_ids_to_devices = dict(
            (self._device_to_port_id(context, device), device)
            for device in devices)
        port_ids = list(port_ids_to_devices.keys())
        ports = db.get_ports_and_sgs(context, port_ids)
        for port in ports:
            # map back to original requested id
            port_id = next((port_id for port_id in port_ids
                           if port['id'].startswith(port_id)), None)
            port['device'] = port_ids_to_devices.get(port_id)

        return ports

    @staticmethod
    def _device_to_port_id(context, device):
        # REVISIT(rkukura): Consider calling into MechanismDrivers to
        # process device names, or having MechanismDrivers supply list
        # of device prefixes to strip.
        for prefix in n_const.INTERFACE_PREFIXES:
            if device.startswith(prefix):
                return device[len(prefix):]
        # REVISIT(irenab): Consider calling into bound MD to
        # handle the get_device_details RPC
        if not uuidutils.is_uuid_like(device):
            port = db.get_port_from_device_mac(context, device)
            if port:
                return port.id
        return device

    def filter_hosts_with_network_access(
            self, context, network_id, candidate_hosts):
        segments = segments_db.get_network_segments(context, network_id)
        return self.mechanism_manager.filter_hosts_with_segment_access(
            context, segments, candidate_hosts, self.get_agents)

    def check_segment_for_agent(self, segment, agent):
        for mech_driver in self.mechanism_manager.ordered_mech_drivers:
            driver_agent_type = getattr(mech_driver.obj, 'agent_type', None)
            if driver_agent_type and driver_agent_type == agent['agent_type']:
                if mech_driver.obj.check_segment_for_agent(segment, agent):
                    return True
        return False

    @registry.receives(resources.SEGMENT, (events.PRECOMMIT_CREATE,
                                           events.PRECOMMIT_DELETE,
                                           events.AFTER_CREATE,
                                           events.AFTER_DELETE))
    def _handle_segment_change(self, rtype, event, trigger, context, segment):
        if (event == events.PRECOMMIT_CREATE and
            not isinstance(trigger, segments_plugin.Plugin)):
            # TODO(xiaohhui): Now, when create network, ml2 will reserve
            # segment and trigger this event handler. This event handler
            # will reserve segment again, which will lead to error as the
            # segment has already been reserved. This check could be removed
            # by unifying segment creation procedure.
            return

        network_id = segment.get('network_id')

        if event == events.PRECOMMIT_CREATE:
            updated_segment = self.type_manager.reserve_network_segment(
                context, segment)
            # The segmentation id might be from ML2 type driver, update it
            # in the original segment.
            segment[api.SEGMENTATION_ID] = updated_segment[api.SEGMENTATION_ID]
        elif event == events.PRECOMMIT_DELETE:
            self.type_manager.release_network_segment(context, segment)

        try:
            self._notify_mechanism_driver_for_segment_change(
                event, context, network_id)
        except ml2_exc.MechanismDriverError:
            with excutils.save_and_reraise_exception():
                LOG.error(_LE("mechanism_manager error occurred when "
                              "handle event %(event)s for segment "
                              "'%(segment)s'"),
                          {'event': event, 'segment': segment['id']})

    def _notify_mechanism_driver_for_segment_change(self, event,
                                                    context, network_id):
        network_with_segments = self.get_network(context, network_id)
        mech_context = driver_context.NetworkContext(
            self, context, network_with_segments,
            original_network=network_with_segments)
        if (event == events.PRECOMMIT_CREATE or
            event == events.PRECOMMIT_DELETE):
            self.mechanism_manager.update_network_precommit(mech_context)
        elif event == events.AFTER_CREATE or event == events.AFTER_DELETE:
            self.mechanism_manager.update_network_postcommit(mech_context)
