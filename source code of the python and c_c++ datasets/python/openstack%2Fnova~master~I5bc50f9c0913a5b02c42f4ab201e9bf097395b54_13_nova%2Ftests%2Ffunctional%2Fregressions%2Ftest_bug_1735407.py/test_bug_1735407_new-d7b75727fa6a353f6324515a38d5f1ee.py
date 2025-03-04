# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import time

from oslo_log import log as logging

from nova import test
from nova.tests import fixtures as nova_fixtures
from nova.tests.functional import integrated_helpers
from nova.tests.unit import fake_network
import nova.tests.unit.image.fake
from nova.tests.unit import policy_fixture
from nova.virt import fake

LOG = logging.getLogger(__name__)


class TestParallelEvacuationWithServerGroup(
        test.TestCase, integrated_helpers.InstanceHelperMixin):
    """Verifies that the server group policy is not violated during parallel
    evacuation.
    """

    def setUp(self):
        super(TestParallelEvacuationWithServerGroup, self).setUp()

        self.useFixture(policy_fixture.RealPolicyFixture())

        # The NeutronFixture is needed to stub out validate_networks in API.
        self.useFixture(nova_fixtures.NeutronFixture(self))

        # This stubs out the network allocation in compute.
        fake_network.set_stub_network_methods(self)

        # We need the computes reporting into placement for the filter
        # scheduler to pick a host.
        self.useFixture(nova_fixtures.PlacementFixture())

        api_fixture = self.useFixture(nova_fixtures.OSAPIFixture(
            api_version='v2.1'))
        self.api = api_fixture.admin_api
        # needed for force_down
        self.api.microversion = '2.11'

        # the image fake backend needed for image discovery
        nova.tests.unit.image.fake.stub_out_image_service(self)
        self.addCleanup(nova.tests.unit.image.fake.FakeImageService_reset)

        self.start_service('conductor')
        self.start_service('scheduler')

        # We start two compute services because we need two instances with
        # anti-affinity server group policy to be booted
        fake.set_nodes(['host1'])
        self.addCleanup(fake.restore_nodes)
        self.compute1 = self.start_service('compute', host='host1')
        fake.set_nodes(['host2'])
        self.addCleanup(fake.restore_nodes)
        self.compute2 = self.start_service('compute', host='host2')

        self.image_id = self.api.get_images()[0]['id']
        self.flavor_id = self.api.get_flavors()[0]['id']

        manager_class = nova.compute.manager.ComputeManager
        original_rebuild = manager_class._do_rebuild_instance

        def fake_rebuild(self, context, instance, *args, **kwargs):
            # Simulate that the rebuild request of one of the instances
            # reaches the target compute manager significantly later so the
            # rebuild of the other instance can finish before the late
            # validation of the first rebuild.
            # We cannot simply delay the virt driver's rebuild or the
            # manager's _rebuild_default_impl as those run after the late
            # validation
            if instance.host == 'host1':
                time.sleep(0.5)

            original_rebuild(self, context, instance, *args, **kwargs)

        self.stub_out('nova.compute.manager.ComputeManager.'
                      '_do_rebuild_instance', fake_rebuild)

    def test_parallel_evacuate_with_server_group(self):
        group_req = {'name': 'a-name', 'policies': ['anti-affinity']}
        group = self.api.post_server_groups(group_req)

        # boot two instances with anti-affinity
        server = {'name': 'server',
                  'imageRef': self.image_id,
                  'flavorRef': self.flavor_id}
        hints = {'group': group['id']}
        created_server1 = self.api.post_server({'server': server,
                                                'os:scheduler_hints': hints})
        server1 = self._wait_for_state_change(self.api,
                                              created_server1, 'ACTIVE')

        created_server2 = self.api.post_server({'server': server,
                                                'os:scheduler_hints': hints})
        server2 = self._wait_for_state_change(self.api,
                                              created_server2, 'ACTIVE')

        # assert that the anti-affinity policy is enforced during the boot
        self.assertNotEqual(server1['OS-EXT-SRV-ATTR:host'],
                            server2['OS-EXT-SRV-ATTR:host'])

        # simulate compute failure on both compute host to allow evacuation
        self.compute1.stop()
        # force it down to avoid waiting for the service group to time out
        self.api.force_down_service('host1', 'nova-compute', True)

        self.compute2.stop()
        self.api.force_down_service('host2', 'nova-compute', True)

        # start a third compute to have place for one of the instances
        fake.set_nodes(['host3'])
        self.compute3 = self.start_service('compute', host='host3')

        # evacuate both instances
        post = {'evacuate': {'onSharedStorage': False}}
        self.api.post_server_action(server1['id'], post)
        self.api.post_server_action(server2['id'], post)

        # make sure that the rebuild is started and then finished
        server1 = self._wait_for_server_parameter(
            self.api, server1, {'OS-EXT-STS:task_state': 'rebuilding'})
        server2 = self._wait_for_server_parameter(
            self.api, server2, {'OS-EXT-STS:task_state': 'rebuilding'})
        server1 = self._wait_for_server_parameter(
            self.api, server1, {'OS-EXT-STS:task_state': None})
        server2 = self._wait_for_server_parameter(
            self.api, server2, {'OS-EXT-STS:task_state': None})

        # NOTE(gibi): The instance.host set _after_ the instance state and
        # tast_state is set back to normal so it is not enough to wait for
        # that. The only thing that happens after the instance.host is set to
        # the target host is the migration status setting to done. So we have
        # to wait for that to avoid asserting the wrong host below.
        self._wait_for_migration_status(server1, 'done')
        self._wait_for_migration_status(server2, 'done')

        # get the servers again to have the latest information about their
        # hosts
        server1 = self.api.get_server(server1['id'])
        server2 = self.api.get_server(server2['id'])

        # assert that the anti-affinity policy is enforced during the
        # evacuation
        # NOTE(gibi): This shows bug 1735407 as both instance ends up on the
        # same host.
        self.assertEqual(server1['OS-EXT-SRV-ATTR:host'],
                         server2['OS-EXT-SRV-ATTR:host'])
        # After the bug 1735407 is fixed the following is expected:
        # self.assertNotEqual(server1['OS-EXT-SRV-ATTR:host'],
        #                     server2['OS-EXT-SRV-ATTR:host'])

        # assert that one of the evacuation was successful and that server is
        # moved to another host and the evacuation of the other server is
        # failed
        # NOTE(gibi): This shows bug 1735407 as both instance is moved
        self.assertNotIn(server1['OS-EXT-SRV-ATTR:host'], {'host1', 'host2'})
        self.assertNotIn(server2['OS-EXT-SRV-ATTR:host'], {'host1', 'host2'})
        # After fixing the bug 1735407 the following is expected
        # if server1['status'] == 'ERROR':
        #     failed_server = server1
        #     evacuated_server = server2
        # else:
        #     failed_server = server2
        #     evacuated_server = server1
        # self.assertEqual('ERROR', failed_server['status'])
        # self.assertNotEqual('host3', failed_server['OS-EXT-SRV-ATTR:host'])
        # self.assertEqual('ACTIVE', evacuated_server['status'])
        # self.assertEqual('host3', evacuated_server['OS-EXT-SRV-ATTR:host'])
