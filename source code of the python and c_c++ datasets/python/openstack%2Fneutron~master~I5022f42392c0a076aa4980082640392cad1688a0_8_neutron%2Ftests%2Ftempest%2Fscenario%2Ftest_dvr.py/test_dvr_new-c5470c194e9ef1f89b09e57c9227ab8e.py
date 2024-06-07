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
from tempest import test

from neutron.tests.tempest import config
from neutron.tests.tempest.scenario import base
from neutron_lib import constants

CONF = config.CONF


class NetworkDvrTest(base.BaseTempestTestCase):
    credentials = ['primary', 'admin']
    force_tenant_isolation = False

    @classmethod
    @test.requires_ext(extension="dvr", service="network")
    def skip_checks(cls):
        super(NetworkDvrTest, cls).skip_checks()

    def check_connectivity_snat_down(self, network_id, fip, keypair):
        port_id = self.client.list_ports(
            network_id=network_id,
            device_owner=constants.DEVICE_OWNER_ROUTER_SNAT)['ports'][0]['id']
        port_status = {'admin_state_up': False}
        self.admin_manager.network_client.update_port(port_id, **port_status)
        self.check_connectivity(fip, CONF.validation.image_ssh_user, keypair)

    @test.idempotent_id('3d73ec1a-2ec6-45a9-b0f8-04a283d9d344')
    def test_vm_reachable_through_compute(self):
        """Check that the VM is reachable through compute node.

        The test is done by putting the SNAT port down on controller node.
        """
        self.setup_network_and_server()
        self.check_connectivity(self.fip['floating_ip_address'],
                                CONF.validation.image_ssh_user,
                                self.keypair['private_key'])
        self.check_connectivity_snat_down(
            self.network['id'], self.fip['floating_ip_address'],
            self.keypair['private_key'])

    @test.idempotent_id('23724222-483a-4129-bc15-7a9278f3828b')
    def test_update_centr_router_to_dvr(self):
        """Check that updating centralized router to be distributed works.
        """
        # Created a centralized router on a DVR setup
        tenant_id = self.client.tenant_id
        router = self.create_router_by_client(
            distributed=False, tenant_id=tenant_id, is_admin=True)
        self.setup_network_and_server(router=router)
        self.check_connectivity(self.fip['floating_ip_address'],
                                CONF.validation.image_ssh_user,
                                self.keypair['private_key'])

        # Update router to be distributed
        router_id = self.client.list_routers()['routers'][0]['id']
        self.admin_manager.network_client.update_router(
            router_id=router_id,
            admin_state_up=False, distributed=True)
        self.admin_manager.network_client.update_router(
            router_id=router_id, admin_state_up=True)
        self.check_connectivity(self.fip['floating_ip_address'],
                                CONF.validation.image_ssh_user,
                                self.keypair['private_key'])

        # Put the Router_SNAT port down, so the traffic flows through Compute
        self.check_connectivity_snat_down(
            self.network['id'], self.fip['floating_ip_address'],
            self.keypair['private_key'])
