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

import mock

from nova import context as nova_context
from nova import exception
from nova import objects
from nova.scheduler import weights
from nova.tests import fixtures as nova_fixtures
from nova.tests.functional import integrated_helpers
from nova.tests.unit.image import fake as fake_image
from nova import utils


class HostNameWeigher(weights.BaseHostWeigher):
    # TestMultiCellMigrate creates host1 in cell1 and host2 in cell2.
    # Something about migrating from host1 to host2 teases out failures
    # which probably has to do with cell1 being the default cell DB in
    # our base test class setup, so prefer host1 to make the tests
    # deterministic.
    _weights = {'host1': 100, 'host2': 50}

    def _weigh_object(self, host_state, weight_properties):
        # Any undefined host gets no weight.
        return self._weights.get(host_state.host, 0)


class TestMultiCellMigrate(integrated_helpers.ProviderUsageBaseTestCase):
    """Tests for cross-cell cold migration (resize)"""

    NUMBER_OF_CELLS = 2
    compute_driver = 'fake.MediumFakeDriver'

    def setUp(self):
        # Use our custom weigher defined above to make sure that we have
        # a predictable scheduling sort order during server create.
        self.flags(weight_classes=[__name__ + '.HostNameWeigher'],
                   group='filter_scheduler')
        super(TestMultiCellMigrate, self).setUp()
        self.cinder = self.useFixture(nova_fixtures.CinderFixture(self))

        self._enable_cross_cell_resize()
        self.created_images = []  # list of image IDs created during resize

        # Adjust the polling interval and timeout for long RPC calls.
        self.flags(rpc_response_timeout=1)
        self.flags(long_rpc_timeout=60)

        # Set up 2 compute services in different cells
        self.host_to_cell_mappings = {
            'host1': 'cell1', 'host2': 'cell2'}

        for host in sorted(self.host_to_cell_mappings):
            cell_name = self.host_to_cell_mappings[host]
            # Start the compute service on the given host in the given cell.
            self._start_compute(host, cell_name=cell_name)
            # Create an aggregate where the AZ name is the cell name.
            agg_id = self._create_aggregate(
                cell_name, availability_zone=cell_name)
            # Add the host to the aggregate.
            body = {'add_host': {'host': host}}
            self.admin_api.post_aggregate_action(agg_id, body)

    def _enable_cross_cell_resize(self):
        # Enable cross-cell resize policy since it defaults to not allow
        # anyone to perform that type of operation. For these tests we'll
        # just allow admins to perform cross-cell resize.
        # TODO(mriedem): Uncomment this when the policy rule is added and
        # used in the compute API _allow_cross_cell_resize method. For now
        # we just stub that method to return True.
        # self.policy_fixture.set_rules({
        #     servers_policies.CROSS_CELL_RESIZE:
        #         base_policies.RULE_ADMIN_API},
        #     overwrite=False)
        self.stub_out('nova.compute.api.API._allow_cross_cell_resize',
                      lambda *a, **kw: True)

    def assertFlavorMatchesAllocation(self, flavor, allocation,
                                      volume_backed=False):
        self.assertEqual(flavor['vcpus'], allocation['VCPU'])
        self.assertEqual(flavor['ram'], allocation['MEMORY_MB'])
        # Volume-backed instances won't have DISK_GB allocations.
        if volume_backed:
            self.assertNotIn('DISK_GB', allocation)
        else:
            self.assertEqual(flavor['disk'], allocation['DISK_GB'])

    def assert_instance_fields_match_flavor(self, instance, flavor):
        self.assertEqual(instance.memory_mb, flavor['ram'])
        self.assertEqual(instance.vcpus, flavor['vcpus'])
        self.assertEqual(instance.root_gb, flavor['disk'])
        self.assertEqual(
            instance.ephemeral_gb, flavor['OS-FLV-EXT-DATA:ephemeral'])

    def _count_volume_attachments(self, server_id):
        attachment_ids = self.cinder.attachment_ids_for_instance(server_id)
        return len(attachment_ids)

    def assert_quota_usage(self, expected_num_instances):
        limits = self.api.get_limits()['absolute']
        self.assertEqual(expected_num_instances, limits['totalInstancesUsed'])

    def _create_server(self, flavor, volume_backed=False):
        """Creates a server and waits for it to be ACTIVE

        :param flavor: dict form of the flavor to use
        :param volume_backed: True if the server should be volume-backed
        :returns: server dict response from the GET /servers/{server_id} API
        """
        # Provide a VIF tag for the pre-existing port. Since VIF tags are
        # stored in the virtual_interfaces table in the cell DB, we want to
        # make sure those survive the resize to another cell.
        networks = [{
            'port': self.neutron.port_1['id'],
            'tag': 'private'
        }]
        image_uuid = fake_image.get_valid_image_id()
        server = self._build_minimal_create_server_request(
            self.api, 'test_cross_cell_resize',
            image_uuid=image_uuid,
            flavor_id=flavor['id'],
            networks=networks)
        # Put a tag on the server to make sure that survives the resize.
        server['tags'] = ['test']
        if volume_backed:
            bdms = [{
                'boot_index': 0,
                'uuid': nova_fixtures.CinderFixture.IMAGE_BACKED_VOL,
                'source_type': 'volume',
                'destination_type': 'volume',
                'tag': 'root'
            }]
            server['block_device_mapping_v2'] = bdms
            # We don't need the imageRef for volume-backed servers.
            server.pop('imageRef', None)

        server = self.api.post_server({'server': server})
        server = self._wait_for_state_change(self.admin_api, server, 'ACTIVE')
        # For volume-backed make sure there is one attachment to start.
        if volume_backed:
            self.assertEqual(1, self._count_volume_attachments(server['id']),
                             self.cinder.volume_to_attachment)
        return server

    def stub_image_create(self):
        """Stubs the _FakeImageService.create method to track created images"""
        original_create = self.image_service.create

        def image_create_snooper(*args, **kwargs):
            image = original_create(*args, **kwargs)
            self.created_images.append(image['id'])
            return image

        _p = mock.patch.object(
            self.image_service, 'create', side_effect=image_create_snooper)
        _p.start()
        self.addCleanup(_p.stop)

    def _resize_and_validate(self, volume_backed=False, stopped=False,
                             target_host=None):
        """Creates and resizes the server to another cell. Validates various
        aspects of the server and its related records (allocations, migrations,
        actions, VIF tags, etc).

        :param volume_backed: True if the server should be volume-backed, False
            if image-backed.
        :param stopped: True if the server should be stopped prior to resize,
            False if the server should be ACTIVE
        :param target_host: If not None, triggers a cold migration to the
            specified host.
        :returns: tuple of:
            - server response object
            - source compute node resource provider uuid
            - target compute node resource provider uuid
            - old flavor
            - new flavor
        """
        # Create the server.
        flavors = self.api.get_flavors()
        old_flavor = flavors[0]
        server = self._create_server(old_flavor, volume_backed=volume_backed)
        original_host = server['OS-EXT-SRV-ATTR:host']
        image_uuid = None if volume_backed else server['image']['id']

        # Our HostNameWeigher ensures the server starts in cell1, so we expect
        # the server AZ to be cell1 as well.
        self.assertEqual('cell1', server['OS-EXT-AZ:availability_zone'])

        if stopped:
            # Stop the server before resizing it.
            self.api.post_server_action(server['id'], {'os-stop': None})
            self._wait_for_state_change(self.api, server, 'SHUTOFF')

        # Before resizing make sure quota usage is only 1 for total instances.
        self.assert_quota_usage(expected_num_instances=1)

        if target_host:
            # Cold migrate the server to the target host.
            new_flavor = old_flavor  # flavor does not change for cold migrate
            body = {'migrate': {'host': target_host}}
            expected_host = target_host
        else:
            # Resize it which should migrate the server to the host in the
            # other cell.
            new_flavor = flavors[1]
            body = {'resize': {'flavorRef': new_flavor['id']}}
            expected_host = 'host1' if original_host == 'host2' else 'host2'

        self.stub_image_create()

        self.api.post_server_action(server['id'], body)
        # Wait for the server to be resized and then verify the host has
        # changed to be the host in the other cell.
        server = self._wait_for_state_change(self.api, server, 'VERIFY_RESIZE')
        self.assertEqual(expected_host, server['OS-EXT-SRV-ATTR:host'])
        # Assert that the instance is only listed one time from the API (to
        # make sure it's not listed out of both cells).
        # Note that we only get one because of _get_unique_filter_method in
        # compute.api.API.get_all() which keys off uuid.
        servers = self.api.get_servers()
        self.assertEqual(1, len(servers),
                         'Unexpected number of servers: %s' % servers)
        self.assertEqual(expected_host, servers[0]['OS-EXT-SRV-ATTR:host'])

        # And that there is only one migration record.
        migrations = self.api.api_get(
            '/os-migrations?instance_uuid=%s' % server['id']
        ).body['migrations']
        self.assertEqual(1, len(migrations),
                         'Unexpected number of migrations records: %s' %
                         migrations)
        migration = migrations[0]
        self.assertEqual('finished', migration['status'])

        # There should be at least two actions, one for create and one for the
        # resize. There will be a third action if the server was stopped.
        actions = self.api.api_get(
            '/servers/%s/os-instance-actions' % server['id']
        ).body['instanceActions']
        expected_num_of_actions = 3 if stopped else 2
        self.assertEqual(expected_num_of_actions, len(actions), actions)
        # Each action should have events (make sure these were copied from
        # the source cell to the target cell).
        for action in actions:
            detail = self.api.api_get(
                '/servers/%s/os-instance-actions/%s' % (
                    server['id'], action['request_id'])).body['instanceAction']
            self.assertNotEqual(0, len(detail['events']), detail)

        # The tag should still be present on the server.
        self.assertEqual(1, len(server['tags']),
                         'Server tags not found in target cell.')
        self.assertEqual('test', server['tags'][0])

        # Confirm the source node has allocations for the old flavor and the
        # target node has allocations for the new flavor.
        source_rp_uuid = self._get_provider_uuid_by_host(original_host)
        # The source node allocations should be on the migration record.
        source_allocations = self._get_allocations_by_provider_uuid(
            source_rp_uuid)[migration['uuid']]['resources']
        self.assertFlavorMatchesAllocation(
            old_flavor, source_allocations, volume_backed=volume_backed)

        target_rp_uuid = self._get_provider_uuid_by_host(expected_host)
        # The target node allocations should be on the instance record.
        target_allocations = self._get_allocations_by_provider_uuid(
            target_rp_uuid)[server['id']]['resources']
        self.assertFlavorMatchesAllocation(
            new_flavor, target_allocations, volume_backed=volume_backed)

        # The instance, in the target cell DB, should have the old and new
        # flavor stored with it with the values we expect at this point.
        target_cell_name = self.host_to_cell_mappings[expected_host]
        self.assertEqual(
            target_cell_name, server['OS-EXT-AZ:availability_zone'])
        target_cell = self.cell_mappings[target_cell_name]
        admin_context = nova_context.get_admin_context()
        with nova_context.target_cell(admin_context, target_cell) as cctxt:
            inst = objects.Instance.get_by_uuid(
                cctxt, server['id'], expected_attrs=['flavor'])
            self.assertIsNotNone(
                inst.old_flavor,
                'instance.old_flavor not saved in target cell')
            self.assertIsNotNone(
                inst.new_flavor,
                'instance.new_flavor not saved in target cell')
            self.assertEqual(inst.flavor.flavorid, inst.new_flavor.flavorid)
            if target_host:  # cold migrate so flavor does not change
                self.assertEqual(
                    inst.flavor.flavorid, inst.old_flavor.flavorid)
            else:
                self.assertNotEqual(
                    inst.flavor.flavorid, inst.old_flavor.flavorid)
            self.assertEqual(old_flavor['id'], inst.old_flavor.flavorid)
            self.assertEqual(new_flavor['id'], inst.new_flavor.flavorid)
            # Assert the ComputeManager._set_instance_info fields
            # are correct after the resize.
            self.assert_instance_fields_match_flavor(inst, new_flavor)
            # The availability_zone field in the DB should also be updated.
            self.assertEqual(target_cell_name, inst.availability_zone)

        # Assert the VIF tag was carried through to the target cell DB.
        interface_attachments = self.api.get_port_interfaces(server['id'])
        self.assertEqual(1, len(interface_attachments))
        self.assertEqual('private', interface_attachments[0]['tag'])

        if volume_backed:
            # Assert the BDM tag was carried through to the target cell DB.
            volume_attachments = self.api.get_server_volumes(server['id'])
            self.assertEqual(1, len(volume_attachments))
            self.assertEqual('root', volume_attachments[0]['tag'])

        # Make sure the guest is no longer tracked on the source node.
        source_guest_uuids = (
            self.computes[original_host].manager.driver.list_instance_uuids())
        self.assertNotIn(server['id'], source_guest_uuids)
        # And the guest is on the target node hypervisor.
        target_guest_uuids = (
            self.computes[expected_host].manager.driver.list_instance_uuids())
        self.assertIn(server['id'], target_guest_uuids)

        # The source hypervisor continues to report usage in the hypervisors
        # API because even though the guest was destroyed there, the instance
        # resources are still claimed on that node in case the user reverts.
        self.assert_hypervisor_usage(source_rp_uuid, old_flavor, volume_backed)
        # The new flavor should show up with resource usage on the target host.
        self.assert_hypervisor_usage(target_rp_uuid, new_flavor, volume_backed)

        # While we have a copy of the instance in each cell database make sure
        # that quota usage is only reporting 1 (because one is hidden).
        self.assert_quota_usage(expected_num_instances=1)

        # For a volume-backed server, at this point there should be two volume
        # attachments for the instance: one tracked in the source cell and
        # one in the target cell.
        if volume_backed:
            self.assertEqual(2, self._count_volume_attachments(server['id']),
                             self.cinder.volume_to_attachment)

        # Assert the expected power state.
        expected_power_state = 4 if stopped else 1
        self.assertEqual(
            expected_power_state, server['OS-EXT-STS:power_state'],
            "Unexpected power state after resize.")

        # For an image-backed server, a snapshot image should have been created
        # and then deleted during the resize.
        if volume_backed:
            self.assertEqual('', server['image'])
            self.assertEqual(
                0, len(self.created_images),
                "Unexpected image create during volume-backed resize")
        else:
            # The original image for the server shown in the API should not
            # have changed even if a snapshot was used to create the guest
            # on the dest host.
            self.assertEqual(image_uuid, server['image']['id'])
            self.assertEqual(
                1, len(self.created_images),
                "Unexpected number of images created for image-backed resize")
            # Make sure the temporary snapshot image was deleted; we use the
            # compute images proxy API here which is deprecated so we force the
            # microversion to 2.1.
            with utils.temporary_mutation(self.api, microversion='2.1'):
                self.api.api_get('/images/%s' % self.created_images[0],
                                 check_response_status=[404])

        return server, source_rp_uuid, target_rp_uuid, old_flavor, new_flavor

    def test_resize_confirm_image_backed(self):
        """Creates an image-backed server in one cell and resizes it to the
        host in the other cell. The resize is confirmed.
        """
        self._resize_and_validate()

        # TODO(mriedem): See: https://review.openstack.org/#/c/603930/

    def test_resize_revert_volume_backed(self):
        """Tests a volume-backed resize to another cell where the resize
        is reverted back to the original source cell.
        """
        self._resize_and_validate(volume_backed=True)

        # TODO(mriedem): See: https://review.openstack.org/#/c/603930/

    def test_delete_while_in_verify_resize_status(self):
        """Tests that when deleting a server in VERIFY_RESIZE status, the
        data is cleaned from both the source and target cell.
        """
        server = self._resize_and_validate()[0]
        self.api.delete_server(server['id'])
        self._wait_until_deleted(server)
        # Now list servers to make sure it doesn't show up from the source cell
        servers = self.api.get_servers()
        self.assertEqual(0, len(servers), servers)
        # FIXME(mriedem): Need to cleanup from source cell in API method
        # _confirm_resize_on_deleting(). The above check passes because the
        # instance is still hidden in the source cell so the API filters it
        # out.
        target_host = server['OS-EXT-SRV-ATTR:host']
        source_host = 'host1' if target_host == 'host2' else 'host2'
        source_cell = self.cell_mappings[
            self.host_to_cell_mappings[source_host]]
        ctxt = nova_context.get_admin_context()
        with nova_context.target_cell(ctxt, source_cell) as cctxt:
            # Once the API is fixed this should raise InstanceNotFound.
            instance = objects.Instance.get_by_uuid(cctxt, server['id'])
            self.assertTrue(instance.hidden)

    def test_cold_migrate_target_host_in_other_cell(self):
        """Tests cold migrating to a target host in another cell. This is
        mostly just to ensure the API does not restrict the target host to
        the source cell when cross-cell resize is allowed by policy.
        """
        # _resize_and_validate creates the server on host1 which is in cell1.
        # To make things interesting, start a third host but in cell1 so we can
        # be sure the requested host from cell2 is honored.
        self._start_compute(
            'host3', cell_name=self.host_to_cell_mappings['host1'])
        self._resize_and_validate(target_host='host2')

    # TODO(mriedem): Test cross-cell list where the source cell has two
    # hosts so the CrossCellWeigher picks the other host in the source cell
    # and we do a traditional resize. Add a variant on this where the flavor
    # being resized to is only available, via aggregate, on the host in the
    # other cell so the CrossCellWeigher is overruled by the filters.

    # TODO(mriedem): Test a bunch of rollback scenarios.

    # TODO(mriedem): Test cross-cell anti-affinity group assumptions from
    # scheduler utils setup_instance_group where it assumes moves are within
    # the same cell, so:
    # 0. create 2 hosts in cell1 and 1 host in cell2
    # 1. create two servers in an anti-affinity group in cell1
    # 2. migrate one server to cell2
    # 3. migrate the other server to cell2 - this should fail during scheduling
    # because there is already a server from the anti-affinity group on the
    # host in cell2 but setup_instance_group code may not catch it.

    # TODO(mriedem): Perform a resize with at-capacity computes, meaning that
    # when we revert we can only fit the instance with the old flavor back
    # onto the source host in the source cell.

    def test_resize_confirm_from_stopped(self):
        """Tests resizing and confirming a server that was initially stopped
        so it should remain stopped through the resize.
        """
        self._resize_and_validate(volume_backed=True, stopped=True)
        # TODO(mriedem): Confirm the resize and assert the guest remains off

    def test_finish_snapshot_based_resize_at_dest_spawn_fails(self):
        """Negative test where the driver spawn fails on the dest host during
        finish_snapshot_based_resize_at_dest which triggers a rollback of the
        instance data in the target cell. Furthermore, the test will hard
        reboot the server in the source cell to recover it from ERROR status.
        """
        # Create a volume-backed server. This is more interesting for rollback
        # testing to make sure the volume attachments in the target cell were
        # cleaned up on failure.
        flavors = self.api.get_flavors()
        server = self._create_server(flavors[0], volume_backed=True)

        # Now mock out the spawn method on the destination host to fail
        # during _finish_snapshot_based_resize_at_dest_spawn and then resize
        # the server.
        error = exception.HypervisorUnavailable(host='host2')
        with mock.patch.object(self.computes['host2'].driver, 'spawn',
                               side_effect=error):
            flavor2 = flavors[1]['id']
            body = {'resize': {'flavorRef': flavor2}}
            self.api.post_server_action(server['id'], body)
            # The server should go to ERROR state with a fault record and
            # the API should still be showing the server from the source cell
            # because the instance mapping was not updated.
            server = self._wait_for_server_parameter(
                self.admin_api, server,
                {'status': 'ERROR', 'OS-EXT-STS:task_state': None})

        # The migration should be in 'error' status.
        self._wait_for_migration_status(server, ['error'])
        # Assert a fault was recorded.
        self.assertIn('fault', server)
        self.assertIn('Connection to the hypervisor is broken',
                      server['fault']['message'])
        # The instance in the target cell DB should have been hard-deleted.
        target_cell = self.cell_mappings['cell2']
        ctxt = nova_context.get_admin_context(read_deleted='yes')
        with nova_context.target_cell(ctxt, target_cell) as cctxt:
            self.assertRaises(
                exception.InstanceNotFound,
                objects.Instance.get_by_uuid, cctxt, server['id'])

        # Assert that there is only one volume attachment for the server, i.e.
        # the one in the target cell was deleted.
        self.assertEqual(1, self._count_volume_attachments(server['id']),
                         self.cinder.volume_to_attachment)

        # Assert that migration-based allocations were properly reverted.
        mig_uuid = self.get_migration_uuid_for_instance(server['id'])
        mig_allocs = self._get_allocations_by_server_uuid(mig_uuid)
        self.assertEqual({}, mig_allocs)
        source_rp_uuid = self._get_provider_uuid_by_host(
            server['OS-EXT-SRV-ATTR:host'])
        server_allocs = self._get_allocations_by_server_uuid(server['id'])
        self.assertFlavorMatchesAllocation(
            flavors[0], server_allocs[source_rp_uuid]['resources'],
            volume_backed=True)

        # Now hard reboot the server in the source cell and it should go back
        # to ACTIVE.
        self.api.post_server_action(server['id'], {'reboot': {'type': 'HARD'}})
        self._wait_for_state_change(self.admin_api, server, 'ACTIVE')

        # Now retry the resize without the fault in the target host to make
        # sure things are OK (no duplicate entry errors in the target DB).
        self.api.post_server_action(server['id'], body)
        self._wait_for_state_change(self.admin_api, server, 'VERIFY_RESIZE')
