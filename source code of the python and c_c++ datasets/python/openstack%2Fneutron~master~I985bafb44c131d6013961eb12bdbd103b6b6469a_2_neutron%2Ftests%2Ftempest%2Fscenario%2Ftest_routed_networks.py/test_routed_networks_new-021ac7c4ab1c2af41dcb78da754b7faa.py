# Copyright 2016 Red Hat, Inc.
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
import subprocess
import time

from neutron.tests.tempest.api import base as base_api
from neutron.tests.tempest import config
from neutron.tests.tempest.scenario import constants
from tempest.common import waiters
from tempest.lib.common.utils import data_utils
from tempest import test

CONF = config.CONF


class RoutedNetworkTest(base_api.BaseAdminNetworkTest):

    @classmethod
    def resource_setup(self):
        super(base_api.BaseAdminNetworkTest, self).resource_setup()
        self.servers = []
        self.keypairs = []
        self.security_rules = []

    @classmethod
    def resource_cleanup(self):
        for keypair in self.keypairs:
            self.manager.keypairs_client.delete_keypair(
                keypair_name=keypair['name'])

        super(base_api.BaseAdminNetworkTest, self).resource_cleanup()

    def _server_cleanup(self, servers):
        for server in servers:
            self.manager.servers_client.delete_server(server)
            waiters.wait_for_server_termination(self.manager.servers_client,
                                                server)

    def _create_keypair(self, client=None):
        name = data_utils.rand_name('keypair-test')
        client = client or self.manager.keypairs_client
        body = client.create_keypair(name=name)
        self.keypairs.append(body['keypair'])
        return body['keypair']

    def _create_secgroup_rules(self, rule_list, secgroup_id=None):
        client = self.manager.network_client
        if not secgroup_id:
            sgs = client.list_security_groups()['security_groups']
            for sg in sgs:
                if sg['name'] == constants.DEFAULT_SECURITY_GROUP:
                    secgroup_id = sg['id']
                    break
        for rule in rule_list:
            direction = rule.pop('direction')
            rule = client.create_security_group_rule(
                direction=direction,
                security_group_id=secgroup_id,
                **rule)
            self.security_rules.append(rule)

    def _create_loginable_secgroup_rule(self, secgroup_id=None):
        """This rule is intended to permit inbound ssh

        Allowing ssh traffic traffic from all sources, so no group_id is
        provided.
        Setting a group_id would only permit traffic from ports
        belonging to the same security group.
        """

        rule_list = [{'protocol': 'tcp',
                      'direction': 'ingress',
                      'port_range_min': 22,
                      'port_range_max': 22,
                      'remote_ip_prefix': '0.0.0.0/0',
                      'description': "ssh test"},

                     ]
        self._create_secgroup_rules(rule_list, secgroup_id=secgroup_id)

    def _create_server(self, flavor_ref, image_ref, key_name, networks,
                       name=None):
        name = name or data_utils.rand_name('server-test')
        server = self.manager.servers_client.create_server(
            name=name, flavorRef=flavor_ref,
            imageRef=image_ref,
            key_name=key_name,
            networks=networks)
        self.servers.append(server['server']['id'])
        return server

    def _check_connectivity(self, port, namespace):
        for fixed_ip in port['fixed_ips']:
            ip = fixed_ip['ip_address']
            self._ssh_check(CONF.validation.image_ssh_user,
                            ip, namespace,
                            self.keypair['private_key'])

    def _ssh_check(self, username, ip, namespace, private_key, retries=10):

        """Though the instance is up, the network maybe not ready to response
         to any request. So we should wait for its ready
        """
        ret = 1
        key_file_path = '/tmp/testkey.dat'
        ssh_commands = 'ssh -o UserKnownHostsFile=/dev/null -o ' \
                       'StrictHostKeyChecking=no -o ConnectTimeout=10 ' \
                       '-i %s %s@%s id' % (key_file_path, username, ip)

        all_cmd = 'sudo ip net exec %s %s' % (namespace, ssh_commands)

        with open(key_file_path, "w") as private_key_file:
            private_key_file.write(private_key)
        for i in range(0, retries):
            ret = subprocess.call(all_cmd, shell=True,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
            if ret == 0:
                break
            time.sleep(3)

        subprocess.call('rm -f %s' % key_file_path, shell=True,
                        stdout=subprocess.PIPE)
        self.assertEqual(0, ret, 'instance is down')

    def _create_routed_network(self, phy_network, network_type):

        segment_id = 2016
        network_name = data_utils.rand_name("test-routed_network")
        segments = [{"provider:segmentation_id": segment_id,
                     "provider:physical_network": phy_network,
                     "provider:network_type": network_type}]
        kwargs = {'shared': True,
                  'segments': segments,
                  "router:external": "False"}
        return self.create_shared_network(
            network_name=network_name, **kwargs)

    def _create_routed_subnet(self, phy_network, network, ip_version=4):
        kwargs_net = {'network_id': network['id'],
                      'physical_network': phy_network
                      }
        ret_segment = self.list_segments(**kwargs_net)
        kwargs_subnet = {
            'name': data_utils.rand_name("test-routed_subnet"),
            'segment_id': ret_segment['segments'][0]['id']
        }
        return self.create_subnet(
            self.network, client=self.admin_client, ip_version=ip_version,
            **kwargs_subnet)

    def _setup_network_and_server(self, phy_network="physnet1",
                                  network_type='vlan', pre_port=False):

        if self.shared_networks:
            self.network = self.shared_networks[0]
        else:
            self.network = self._create_routed_network(phy_network,
                                                       network_type)
        if self.subnets:
            self.subnet = self.subnets[0]
        else:
            self.subnet = self._create_routed_subnet(phy_network, self.network)
        if self.keypairs:
            self.keypair = self.keypairs[0]
        else:
            self.keypair = self._create_keypair()
        if not self.security_rules:
            self._create_loginable_secgroup_rule()
        if pre_port is True:
            port = self.create_port(self.network)
            server = self._create_server(
                flavor_ref=CONF.compute.flavor_ref,
                image_ref=CONF.compute.image_ref,
                key_name=self.keypair['name'],
                networks=[{'port': port['id']}])
        else:
            server = self._create_server(
                flavor_ref=CONF.compute.flavor_ref,
                image_ref=CONF.compute.image_ref,
                key_name=self.keypair['name'],
                networks=[{'uuid': self.network['id']}])

        waiters.wait_for_server_status(self.manager.servers_client,
                                       server['server']['id'],
                                       constants.SERVER_STATUS_ACTIVE)
        namespace = 'qdhcp-' + self.network['id']
        port = self.client.list_ports(
            network_id=self.network['id'],
            device_id=server['server']['id'])['ports'][0]
        return server['server']['id'], port, namespace

    @test.idempotent_id('953d4048-0388-4fb5-beca-f85e07ba8b1e')
    def test_routed_network(self):
        (server, port, namespace) = self._setup_network_and_server(
            pre_port=False)
        self._check_connectivity(port, namespace)
        servers = [server]
        self._server_cleanup(servers)

    @test.idempotent_id('73aaeccb-82de-4cc7-81bc-a62a8cf440c0')
    def test_routed_network_pre_allocate_port(self):
        (server, port, namespace) = self._setup_network_and_server(
            pre_port=True)
        self._check_connectivity(port, namespace)
        servers = [server]
        self._server_cleanup(servers)
