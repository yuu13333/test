# Copyright 2016 OVH SAS
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

from oslo_log import helpers as log_helpers
from oslo_log import log as logging

from neutron.api.rpc.callbacks import resources
from neutron.api.v2 import attributes as api_v2_attributes
from neutron.callbacks import events as callbacks_events
from neutron.callbacks import registry as callbacks_registry
from neutron.callbacks import resources as callbacks_resources
from neutron.extensions import portbindings
from neutron import manager
from neutron.plugins.ml2.drivers.linuxbridge.mech_driver import (
    mech_linuxbridge)
from neutron.plugins.ml2.drivers.mech_sriov.mech_driver import (
    mech_driver as mech_sriov)
from neutron.plugins.ml2.drivers.openvswitch.mech_driver import (
    mech_openvswitch)
from neutron.services.qos.notification_drivers import message_queue
from neutron.services.qos.notification_drivers import qos_base
from neutron.services.qos import qos_consts

LOG = logging.getLogger(__name__)

RULE_VALIDATION_IGNORED_FIELDS = ['qos_policy_id', 'id', 'type']

# TODO(slaweq/ajo): list of all mech_drivers and what VNIC/VIF type are
# supported by each of them should be provided by ml2 plugin in similar way
# like it is done for supported_qos_rule_types for example
VNIC_TYPE_TO_MECH_DRIVERS = {
    portbindings.VNIC_NORMAL: [mech_openvswitch.OpenvswitchMechanismDriver,
                               mech_linuxbridge.LinuxbridgeMechanismDriver],
    portbindings.VNIC_DIRECT: [mech_sriov.SriovNicSwitchMechanismDriver]}

VIF_TYPE_TO_MECH_DRIVERS = {
    portbindings.VIF_TYPE_OVS: [mech_openvswitch.OpenvswitchMechanismDriver],
    portbindings.VIF_TYPE_BRIDGE: [
        mech_linuxbridge.LinuxbridgeMechanismDriver],
}


class Ml2QoSServiceNotificationDriver(
    message_queue.RpcQosServiceNotificationDriver):
    """RPC message queue service notification driver with policy validation

    This is service notification driver for QoS with support for validate if
    policy can be applied on port(s) based on vif_type and/or vnic_type of
    ports.
    This driver is designed to work with ML2 core plugin.
    """

    def __init__(self):
        super(Ml2QoSServiceNotificationDriver, self).__init__()
        callbacks_registry.subscribe(
                self._validate_create_port_callback,
                callbacks_resources.PORT,
                callbacks_events.BEFORE_CREATE)
        callbacks_registry.subscribe(
                self._validate_update_port_callback,
                callbacks_resources.PORT,
                callbacks_events.BEFORE_UPDATE)
        callbacks_registry.subscribe(
                self._validate_update_network_callback,
                api_v2_attributes.NETWORK,
                callbacks_events.BEFORE_UPDATE)

    @log_helpers.log_method_call
    def validate_policy_for_port(self, context, policy, port):
        vnic_type = port.get(portbindings.VNIC_TYPE)
        vif_type = port.get(portbindings.VIF_TYPE)

        if vif_type and vif_type != portbindings.VIF_TYPE_UNBOUND:
            mechdrivers = VIF_TYPE_TO_MECH_DRIVERS.get(vif_type)
            check_by_vnic_type = False
        else:
            mechdrivers = VNIC_TYPE_TO_MECH_DRIVERS.get(vnic_type)
            check_by_vnic_type = True

        if not mechdrivers:
            raise qos_base.PolicyPortConflict(
                policy_id=policy['id'], port_id=port['id'])
        self._validate_port_rules_for_mech_drivers(
            port, policy.rules, mechdrivers, check_by_vnic_type)

    def _validate_create_port_callback(self, resource, event, trigger,
                                       **kwargs):
        context = kwargs.get('context')
        port = kwargs.get('port')
        policy_id = port.get(qos_consts.QOS_POLICY_ID)

        if policy_id is None:
            return

        policy = message_queue._get_qos_policy_cb(
            resources.QOS_POLICY, policy_id, context=context)
        self.validate_policy_for_port(context, policy, port)

    def _validate_update_port_callback(self, resource, event, trigger,
                                       **kwargs):
        context = kwargs.get('context')
        original_port = kwargs.get('original_port')
        updated_port = kwargs.get('updated_port')
        original_policy_id = original_port.get(qos_consts.QOS_POLICY_ID)
        policy_id = updated_port.get(qos_consts.QOS_POLICY_ID)

        if policy_id is None or policy_id == original_policy_id:
            return

        policy = message_queue._get_qos_policy_cb(
            resources.QOS_POLICY, policy_id, context=context)
        self.validate_policy_for_port(context, policy, updated_port)

    def _validate_update_network_callback(self, resource, event, trigger,
                                          **kwargs):
        context = kwargs.get('context')
        original_network = kwargs.get('original_network')
        updated_network = kwargs.get('updated_network')
        original_policy_id = original_network.get(qos_consts.QOS_POLICY_ID)
        policy_id = updated_network.get(qos_consts.QOS_POLICY_ID)

        if policy_id is None or policy_id == original_policy_id:
            return

        policy = message_queue._get_qos_policy_cb(
            resources.QOS_POLICY, policy_id, context=context)
        self.validate_policy_for_network(context, policy, updated_network)

    def _validate_port_rules_for_mech_drivers(self, port, rules, mechdrivers,
                                              check_by_vnic_type):
        core_plugin = manager.NeutronManager.get_plugin()
        validated_with_one_mech_driver = False

        for driver in core_plugin.mechanism_manager.ordered_mech_drivers:
            for mechdriver in mechdrivers:
                if (isinstance(driver.obj, mechdriver) and
                    driver.obj._supports_port_binding):

                    self._validate_port_rules_supported(
                        port, rules, driver.obj.supported_qos_rule_types)
                    validated_with_one_mech_driver = True

        if not validated_with_one_mech_driver and check_by_vnic_type:
            for mechdriver in mechdrivers:
                self._validate_port_rules_supported(
                    port, rules, mechdriver.supported_qos_rule_types)

    def _validate_port_rules_supported(self, port, rules, supported_rules):
        # we expect the mechanism supportd_qos_rule_types to look like this
        # {'DSCP_MARK': {'dscp_mark': None},
        #  'BANDWIDTH_LIMIT': {'max_kbps' : qos_consts.ANY_VALUE,
        #                      'direction': ['egress']}}
        # in this example, BANDWIDTH_LIMIT does not support max_burst_kbps and
        # directions others than egress
        for rule in rules:
            if rule.rule_type not in supported_rules.keys():
                raise qos_base.PolicyRuleNotSupportedForPort(
                    rule_type=rule.rule_type,
                    port_id=port.get("id"))
            self._validate_rule_parameters(port, rule, supported_rules)

    def _validate_rule_parameters(self, port, rule, supported_rules):
        for parameter, value in rule.to_dict().items():
            if parameter in RULE_VALIDATION_IGNORED_FIELDS:
                continue

            supported_parameters = supported_rules[rule.rule_type]

            supported_values = supported_parameters.get(parameter)
            if supported_values is None:
                # The parameter is not supported by the mech driver
                raise qos_base.PolicyRuleParameterNotSupportedForPort(
                        rule_type=rule.rule_type,
                        parameter=parameter,
                        port_id=port['id'])

            if (supported_values != qos_consts.ANY_VALUE and
                value not in supported_values):
                raise qos_base.PolicyRuleParameterValueNotSupportedForPort(
                        rule_type=rule.rule_type,
                        parameter=parameter,
                        value=value,
                        port_id=port['id'])
