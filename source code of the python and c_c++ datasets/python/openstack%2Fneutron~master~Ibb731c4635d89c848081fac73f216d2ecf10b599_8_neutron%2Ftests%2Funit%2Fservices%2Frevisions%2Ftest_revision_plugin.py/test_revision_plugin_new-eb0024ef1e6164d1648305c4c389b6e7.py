# All rights reserved.
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
#

import netaddr
from neutron_lib import constants
from neutron_lib import context as nctx
from neutron_lib.plugins import directory
from oslo_utils import uuidutils

from neutron.db import models_v2
from neutron.objects import ports as port_obj
from neutron.plugins.ml2 import config
from neutron.tests.unit.plugins.ml2 import test_plugin


class TestRevisionPlugin(test_plugin.Ml2PluginV2TestCase):

    l3_plugin = ('neutron.tests.unit.extensions.test_extraroute.'
                 'TestExtraRouteL3NatServicePlugin')

    _extension_drivers = ['qos']

    def get_additional_service_plugins(self):
        p = super(TestRevisionPlugin, self).get_additional_service_plugins()
        p.update({'revision_plugin_name': 'revisions',
                  'qos_plugin_name': 'qos',
                  'tag_name': 'tag'})
        return p

    def setUp(self):
        config.cfg.CONF.set_override('extension_drivers',
                                     self._extension_drivers,
                                     group='ml2')
        super(TestRevisionPlugin, self).setUp()
        self.cp = directory.get_plugin()
        self.l3p = directory.get_plugin(constants.L3)
        self._ctx = nctx.get_admin_context()

    @property
    def ctx(self):
        # TODO(kevinbenton): return ctx without expire_all after switch to
        # enginefacade complete. We expire_all here because the switch to
        # the new engine facade is resulting in changes being spread over
        # other sessions so we can end up getting stale reads in the parent
        # session if objects remain in the identity map.
        if not self._ctx.session.is_active:
            self._ctx.session.expire_all()
        return self._ctx

    def test_handle_expired_object(self):
        rp = directory.get_plugin('revision_plugin')
        with self.port():
            with self.ctx.session.begin():
                ipal_objs = port_obj.IPAllocation.get_objects(self.ctx)
                if ipal_objs:
                    ipal_obj = ipal_objs[0]
                # load port into our session
                port = self.ctx.session.query(models_v2.Port).one()
                # simulate concurrent delete in another session
                other_ctx = nctx.get_admin_context()
                other_ctx.session.delete(
                    other_ctx.session.query(models_v2.Port).first()
                )
                # expire the port so the revision bumping code will trigger a
                # lookup on its attributes and encounter an ObjectDeletedError
                self.ctx.session.expire(port)
                rp._bump_related_revisions(self.ctx.session, ipal_obj)

    def test_port_name_update_revises(self):
        with self.port() as port:
            rev = port['port']['revision_number']
            new = {'port': {'name': 'seaweed'}}
            response = self._update('ports', port['port']['id'], new)
            new_rev = response['port']['revision_number']
            self.assertGreater(new_rev, rev)

    def test_port_ip_update_revises(self):
        with self.port() as port:
            rev = port['port']['revision_number']
            new = {'port': {'fixed_ips': port['port']['fixed_ips']}}
            # ensure adding an IP allocation updates the port
            next_ip = str(netaddr.IPAddress(
                  new['port']['fixed_ips'][0]['ip_address']) + 1)
            new['port']['fixed_ips'].append({'ip_address': next_ip})
            response = self._update('ports', port['port']['id'], new)
            self.assertEqual(2, len(response['port']['fixed_ips']))
            new_rev = response['port']['revision_number']
            self.assertGreater(new_rev, rev)
            # ensure deleting an IP allocation updates the port
            rev = new_rev
            new['port']['fixed_ips'].pop()
            response = self._update('ports', port['port']['id'], new)
            self.assertEqual(1, len(response['port']['fixed_ips']))
            new_rev = response['port']['revision_number']
            self.assertGreater(new_rev, rev)

    def test_security_group_rule_ops_bump_security_group(self):
        s = {'security_group': {'tenant_id': 'some_tenant', 'name': '',
                                'description': 's'}}
        sg = self.cp.create_security_group(self.ctx, s)
        s['security_group']['name'] = 'hello'
        updated = self.cp.update_security_group(self.ctx, sg['id'], s)
        self.assertGreater(updated['revision_number'], sg['revision_number'])
        # ensure rule changes bump parent SG
        r = {'security_group_rule': {'tenant_id': 'some_tenant',
                                     'port_range_min': 80, 'protocol': 6,
                                     'port_range_max': 90,
                                     'remote_ip_prefix': '0.0.0.0/0',
                                     'ethertype': 'IPv4',
                                     'remote_group_id': None,
                                     'direction': 'ingress',
                                     'security_group_id': sg['id']}}
        rule = self.cp.create_security_group_rule(self.ctx, r)
        sg = updated
        updated = self.cp.get_security_group(self.ctx, sg['id'])
        self.assertGreater(updated['revision_number'], sg['revision_number'])
        self.cp.delete_security_group_rule(self.ctx, rule['id'])
        sg = updated
        updated = self.cp.get_security_group(self.ctx, sg['id'])
        self.assertGreater(updated['revision_number'], sg['revision_number'])

    def test_router_interface_ops_bump_router(self):
        r = {'router': {'name': 'myrouter', 'tenant_id': 'some_tenant',
                        'admin_state_up': True}}
        router = self.l3p.create_router(self.ctx, r)
        r['router']['name'] = 'yourrouter'
        updated = self.l3p.update_router(self.ctx, router['id'], r)
        self.assertGreater(updated['revision_number'],
                           router['revision_number'])
        # add an intf and make sure it bumps rev
        with self.subnet(tenant_id='some_tenant', cidr='10.0.1.0/24') as s:
            interface_info = {'subnet_id': s['subnet']['id']}
        self.l3p.add_router_interface(self.ctx, router['id'],
                                      interface_info)
        router = updated
        updated = self.l3p.get_router(self.ctx, router['id'])
        self.assertGreater(updated['revision_number'],
                           router['revision_number'])
        # Add a route and make sure it bumps revision number
        router = updated
        body = {'router': {'routes': [{'destination': '192.168.2.0/24',
                                       'nexthop': '10.0.1.3'}]}}
        self.l3p.update_router(self.ctx, router['id'], body)
        updated = self.l3p.get_router(self.ctx, router['id'])
        self.assertGreater(updated['revision_number'],
                           router['revision_number'])
        router = updated
        body['router']['routes'] = []
        self.l3p.update_router(self.ctx, router['id'], body)
        updated = self.l3p.get_router(self.ctx, router['id'])
        self.assertGreater(updated['revision_number'],
                           router['revision_number'])
        self.l3p.remove_router_interface(self.ctx, router['id'],
                                         interface_info)
        router = updated
        updated = self.l3p.get_router(self.ctx, router['id'])
        self.assertGreater(updated['revision_number'],
                           router['revision_number'])

    def test_qos_policy_bump_port_revision(self):
        with self.port() as port:
            rev = port['port']['revision_number']
            qos_plugin = directory.get_plugin('QOS')
            qos_policy = {'policy': {'name': "policy1",
                                     'project_id': uuidutils.generate_uuid()}}
            qos_obj = qos_plugin.create_policy(self.ctx, qos_policy)
            data = {'port': {'qos_policy_id': qos_obj['id']}}
            response = self._update('ports', port['port']['id'], data)
            new_rev = response['port']['revision_number']
            self.assertGreater(new_rev, rev)

    def test_qos_policy_bump_network_revision(self):
        with self.network() as network:
            rev = network['network']['revision_number']
            qos_plugin = directory.get_plugin('QOS')
            qos_policy = {'policy': {'name': "policy1",
                                     'project_id': uuidutils.generate_uuid()}}
            qos_obj = qos_plugin.create_policy(self.ctx, qos_policy)
            data = {'network': {'qos_policy_id': qos_obj['id']}}
            response = self._update('networks', network['network']['id'], data)
            new_rev = response['network']['revision_number']
            self.assertGreater(new_rev, rev)

    def test_net_tag_bumps_net_revision(self):
        with self.network() as network:
            rev = network['network']['revision_number']
            tag_plugin = directory.get_plugin('TAG')
            tag_plugin.update_tag(self.ctx, 'networks',
                                  network['network']['id'], 'mytag')
            updated = directory.get_plugin().get_network(
                self.ctx, network['network']['id'])
            self.assertGreater(updated['revision_number'], rev)
            tag_plugin.delete_tag(self.ctx, 'networks',
                                  network['network']['id'], 'mytag')
            rev = updated['revision_number']
            updated = directory.get_plugin().get_network(
                self.ctx, network['network']['id'])
            self.assertGreater(updated['revision_number'], rev)
