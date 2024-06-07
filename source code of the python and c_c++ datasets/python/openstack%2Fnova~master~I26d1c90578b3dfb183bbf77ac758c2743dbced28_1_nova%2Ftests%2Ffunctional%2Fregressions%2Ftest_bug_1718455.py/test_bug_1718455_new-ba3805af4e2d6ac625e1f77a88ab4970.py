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

from nova import test
from nova.tests import fixtures as nova_fixtures
from nova.tests.functional import integrated_helpers
from nova.tests.unit import fake_network
import nova.tests.unit.image.fake
from nova.tests.unit import policy_fixture
from nova.virt import fake


class TestLiveMigrateOneOfConcurrentlyCreatedInstances(
        test.TestCase, integrated_helpers.InstanceHelperMixin):
    """Regression tests for bug #1718455

    When creating multiple instances at the same time, the RequestSpec record
    is persisting the number of concurrent instances.
    Once we want to move or rebuild one of them, the scheduler shouldn't
    lookup that specific record unless we don't pass a list of instance UUIDs
    to move. It was partially fixed by bug #1708961 but we forgot to amend
    some place in the scheduler so that the right number of hosts was returned
    to the scheduler method calling both the Placement API and filters/weighers
    but we were verifying that returned size of hosts against a wrong number,
    which is the number of instances created concurrently.

    That test will create 2 concurrent instances and verify that when
    live-migrating one of them, we end up with a NoValidHost exception.
    """

    microversion = 'latest'

    def setUp(self):
        super(TestLiveMigrateOneOfConcurrentlyCreatedInstances, self).setUp()

        self.useFixture(policy_fixture.RealPolicyFixture())
        self.useFixture(nova_fixtures.NeutronFixture(self))
        self.useFixture(nova_fixtures.PlacementFixture())

        api_fixture = self.useFixture(nova_fixtures.OSAPIFixture(
            api_version='v2.1'))

        self.api = api_fixture.admin_api
        self.api.microversion = self.microversion

        nova.tests.unit.image.fake.stub_out_image_service(self)

        self.start_service('conductor')
        self.start_service('scheduler')

        self.addCleanup(nova.tests.unit.image.fake.FakeImageService_reset)

        # set_nodes() is needed to have each compute service return a
        # different nodename, so we get two hosts in the list of candidates
        # for scheduling. Otherwise both hosts will have the same default
        # nodename "fake-mini". The host passed to start_service controls the
        # "host" attribute and set_nodes() sets the "nodename" attribute.
        # We set_nodes() to make host and nodename the same for each compute.
        fake.set_nodes(['host1'])
        self.addCleanup(fake.restore_nodes)
        self.start_service('compute', host='host1')
        fake.set_nodes(['host2'])
        self.addCleanup(fake.restore_nodes)
        self.start_service('compute', host='host2')

        fake_network.set_stub_network_methods(self)

        flavors = self.api.get_flavors()
        self.flavor1 = flavors[0]

    def _boot_servers(self, num_servers=1):
        server_req = self._build_minimal_create_server_request(
            self.api, 'some-server', flavor_id=self.flavor1['id'],
            image_uuid='155d900f-4e14-4e4c-a73d-069cbf4541e6',
            networks='none')
        if num_servers > 1:
            server_req.update({'min_count': str(num_servers)})
        created_server_1 = self.api.post_server({'server': server_req})
        return self._wait_for_state_change(
            self.api, created_server_1, 'ACTIVE')

    def test_live_migrate_one_instance(self):
        # Boot a server
        self._boot_servers(num_servers=2)

        servers = self.api.get_servers()

        # Take the first instance and verify which host the instance is there
        server = servers[0]
        original_host = server['OS-EXT-SRV-ATTR:host']
        target_host = 'host1' if original_host == 'host2' else 'host2'

        # Initiate live migration for that instance by targeting the other host
        post = {'os-migrateLive': {'block_migration': 'auto',
                                   'host': target_host}}
        self.api.post_server_action(server['id'], post)

        server = self._wait_for_state_change(self.api, server, 'ACTIVE')

        # Verify that the migration failed as the instance is still on the
        # source node.
        self.assertEqual(original_host, server['OS-EXT-SRV-ATTR:host'])
        # Check the migration by itself
        migrations = self.api.get_migrations()
        self.assertEqual(1, len(migrations))
        self.assertEqual('live-migration', migrations[0]['migration_type'])
        self.assertEqual(server['id'], migrations[0]['instance_uuid'])
        self.assertEqual(original_host, migrations[0]['source_compute'])
        self.assertEqual('error', migrations[0]['status'])
