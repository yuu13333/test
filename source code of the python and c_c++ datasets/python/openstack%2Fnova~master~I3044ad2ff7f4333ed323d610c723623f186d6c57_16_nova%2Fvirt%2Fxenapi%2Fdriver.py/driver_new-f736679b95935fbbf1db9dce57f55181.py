# Copyright (c) 2010 Citrix Systems, Inc.
# Copyright 2010 OpenStack Foundation
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

"""
A driver for XenServer or Xen Cloud Platform.

**Variable Naming Scheme**

- suffix "_ref" for opaque references
- suffix "_uuid" for UUIDs
- suffix "_rec" for record objects
"""

import math

from os_xenapi.client import session
from oslo_log import log as logging
from oslo_serialization import jsonutils
from oslo_utils import units
from oslo_utils import uuidutils
from oslo_utils import versionutils

import six.moves.urllib.parse as urlparse

import nova.conf
from nova import exception
from nova.i18n import _
from nova import rc_fields as fields
from nova.virt import driver
from nova.virt.xenapi import host
from nova.virt.xenapi import pool
from nova.virt.xenapi import vm_utils
from nova.virt.xenapi import vmops
from nova.virt.xenapi import volumeops

LOG = logging.getLogger(__name__)

CONF = nova.conf.CONF

OVERHEAD_BASE = 3
OVERHEAD_PER_MB = 0.00781
OVERHEAD_PER_VCPU = 1.5

VGPU_RP_PREFIX = "VGPU_"


def invalid_option(option_name, recommended_value):
    LOG.exception(_('Current value of '
                    'CONF.xenserver.%(option)s option incompatible with '
                    'CONF.xenserver.independent_compute=True.  '
                    'Consider using "%(recommended)s"'),
                  {'option': option_name,
                   'recommended': recommended_value})
    raise exception.NotSupportedWithOption(
        operation=option_name,
        option='CONF.xenserver.independent_compute')


class XenAPIDriver(driver.ComputeDriver):
    """A connection to XenServer or Xen Cloud Platform."""
    capabilities = {
        "has_imagecache": False,
        "supports_recreate": False,
        "supports_migrate_to_same_host": False,
        "supports_attach_interface": True,
        "supports_device_tagging": True,
        "supports_multiattach": False
    }

    def __init__(self, virtapi, read_only=False):
        super(XenAPIDriver, self).__init__(virtapi)

        url = CONF.xenserver.connection_url
        username = CONF.xenserver.connection_username
        password = CONF.xenserver.connection_password
        if not url or password is None:
            raise Exception(_('Must specify connection_url, '
                              'connection_username (optionally), and '
                              'connection_password to use '
                              'compute_driver=xenapi.XenAPIDriver'))

        self._session = session.XenAPISession(url, username, password,
                                              originator="nova")
        self._volumeops = volumeops.VolumeOps(self._session)
        self._host_state = None
        self._host = host.Host(self._session, self.virtapi)
        self._vmops = vmops.VMOps(self._session, self.virtapi)
        self._initiator = None
        self._hypervisor_hostname = None
        self._pool = pool.ResourcePool(self._session, self.virtapi)

    @property
    def host_state(self):
        if not self._host_state:
            self._host_state = host.HostState(self._session)
        return self._host_state

    def init_host(self, host):
        if CONF.xenserver.independent_compute:
            # Check various options are in the correct state:
            if CONF.xenserver.check_host:
                invalid_option('CONF.xenserver.check_host', False)
            if CONF.flat_injected:
                invalid_option('CONF.flat_injected', False)
            if CONF.default_ephemeral_format and \
               CONF.default_ephemeral_format != 'ext3':
                invalid_option('CONF.default_ephemeral_format', 'ext3')

        if CONF.xenserver.check_host:
            vm_utils.ensure_correct_host(self._session)

        if not CONF.xenserver.independent_compute:
            try:
                vm_utils.cleanup_attached_vdis(self._session)
            except Exception:
                LOG.exception(_('Failure while cleaning up attached VDIs'))

    def instance_exists(self, instance):
        """Checks existence of an instance on the host.

        :param instance: The instance to lookup

        Returns True if supplied instance exists on the host, False otherwise.

        NOTE(belliott): This is an override of the base method for
        efficiency.
        """
        return self._vmops.instance_exists(instance.name)

    def estimate_instance_overhead(self, instance_info):
        """Get virtualization overhead required to build an instance of the
        given flavor.

        :param instance_info: Instance/flavor to calculate overhead for.
        :returns: Overhead memory in MB.
        """

        # XenServer memory overhead is proportional to the size of the
        # VM.  Larger flavor VMs become more efficient with respect to
        # overhead.

        # interpolated formula to predict overhead required per vm.
        # based on data from:
        # https://wiki.openstack.org/wiki/XenServer/Overhead
        # Some padding is done to each value to fit all available VM data
        memory_mb = instance_info['memory_mb']
        vcpus = instance_info.get('vcpus', 1)
        overhead = ((memory_mb * OVERHEAD_PER_MB) + (vcpus * OVERHEAD_PER_VCPU)
                        + OVERHEAD_BASE)
        overhead = math.ceil(overhead)
        return {'memory_mb': overhead}

    def list_instances(self):
        """List VM instances."""
        return self._vmops.list_instances()

    def list_instance_uuids(self):
        """Get the list of nova instance uuids for VMs found on the
        hypervisor.
        """
        return self._vmops.list_instance_uuids()

    def _get_allocated_gpu_grp(self, allocations):
        # Currently, XenServer only supports one vGPU per instance, so only
        # the first vgpu allocation will be picked up. Resource provider uuid
        # in allocations is same with the corresponding gpu group uuid
        if not allocations:
            # If no allocations, there is no vGPU request.
            return None
        RC_VGPU = fields.ResourceClass.VGPU
        for rp in allocations:
            res = allocations[rp]['resources']
            if res and RC_VGPU in res and res[RC_VGPU] > 0:
                return rp
        return None

    def _get_vgpu_info(self, allocations):
        """Get vGPU info if any vGPU is allocated."""
        gpu_grp_uuid = self._get_allocated_gpu_grp(allocations)
        if not gpu_grp_uuid:
            return None
        # Get vgpu information by gpu group uuid.
        vgpu_type_info = self.host_state.find_an_enabled_vgpu_in_group(
            gpu_grp_uuid)
        if vgpu_type_info:
            return dict(gpu_grp_uuid=gpu_grp_uuid,
                        vgpu_type_uuid=vgpu_type_info['vgpu_type_uuid'])

        # No remaining vGPU available: e.g. the vGPU resource has been used by
        # other instance or the vGPU has been changed to be disabled.
        raise exception.ComputeResourcesUnavailable(
            reason='vGPU resource is not available')

    def spawn(self, context, instance, image_meta, injected_files,
              admin_password, allocations, network_info=None,
              block_device_info=None):
        """Create VM instance."""
        vgpu_info = self._get_vgpu_info(allocations)
        self._vmops.spawn(context, instance, image_meta, injected_files,
                          admin_password, network_info, block_device_info,
                          vgpu_info)

    def confirm_migration(self, context, migration, instance, network_info):
        """Confirms a resize, destroying the source VM."""
        self._vmops.confirm_migration(migration, instance, network_info)

    def finish_revert_migration(self, context, instance, network_info,
                                block_device_info=None, power_on=True):
        """Finish reverting a resize."""
        # NOTE(vish): Xen currently does not use network info.
        self._vmops.finish_revert_migration(context, instance,
                                            block_device_info,
                                            power_on)

    def finish_migration(self, context, migration, instance, disk_info,
                         network_info, image_meta, resize_instance,
                         block_device_info=None, power_on=True):
        """Completes a resize, turning on the migrated instance."""
        self._vmops.finish_migration(context, migration, instance, disk_info,
                                     network_info, image_meta, resize_instance,
                                     block_device_info, power_on)

    def snapshot(self, context, instance, image_id, update_task_state):
        """Create snapshot from a running VM instance."""
        self._vmops.snapshot(context, instance, image_id, update_task_state)

    def post_interrupted_snapshot_cleanup(self, context, instance):
        """Cleans up any resources left after a failed snapshot."""
        self._vmops.post_interrupted_snapshot_cleanup(context, instance)

    def reboot(self, context, instance, network_info, reboot_type,
               block_device_info=None, bad_volumes_callback=None):
        """Reboot VM instance."""
        self._vmops.reboot(instance, reboot_type,
                           bad_volumes_callback=bad_volumes_callback)

    def set_admin_password(self, instance, new_pass):
        """Set the root/admin password on the VM instance."""
        self._vmops.set_admin_password(instance, new_pass)

    def inject_file(self, instance, b64_path, b64_contents):
        """Create a file on the VM instance. The file path and contents
        should be base64-encoded.
        """
        self._vmops.inject_file(instance, b64_path, b64_contents)

    def change_instance_metadata(self, context, instance, diff):
        """Apply a diff to the instance metadata."""
        self._vmops.change_instance_metadata(instance, diff)

    def destroy(self, context, instance, network_info, block_device_info=None,
                destroy_disks=True):
        """Destroy VM instance."""
        self._vmops.destroy(instance, network_info, block_device_info,
                            destroy_disks)

    def cleanup(self, context, instance, network_info, block_device_info=None,
                destroy_disks=True, migrate_data=None, destroy_vifs=True):
        """Cleanup after instance being destroyed by Hypervisor."""
        pass

    def pause(self, instance):
        """Pause VM instance."""
        self._vmops.pause(instance)

    def unpause(self, instance):
        """Unpause paused VM instance."""
        self._vmops.unpause(instance)

    def migrate_disk_and_power_off(self, context, instance, dest,
                                   flavor, network_info,
                                   block_device_info=None,
                                   timeout=0, retry_interval=0):
        """Transfers the VHD of a running instance to another host, then shuts
        off the instance copies over the COW disk
        """
        # NOTE(vish): Xen currently does not use network info.
        # TODO(PhilDay): Add support for timeout (clean shutdown)
        return self._vmops.migrate_disk_and_power_off(context, instance,
                    dest, flavor, block_device_info)

    def suspend(self, context, instance):
        """suspend the specified instance."""
        self._vmops.suspend(instance)

    def resume(self, context, instance, network_info, block_device_info=None):
        """resume the specified instance."""
        self._vmops.resume(instance)

    def rescue(self, context, instance, network_info, image_meta,
               rescue_password):
        """Rescue the specified instance."""
        self._vmops.rescue(context, instance, network_info, image_meta,
                           rescue_password)

    def set_bootable(self, instance, is_bootable):
        """Set the ability to power on/off an instance."""
        self._vmops.set_bootable(instance, is_bootable)

    def unrescue(self, instance, network_info):
        """Unrescue the specified instance."""
        self._vmops.unrescue(instance)

    def power_off(self, instance, timeout=0, retry_interval=0):
        """Power off the specified instance."""
        # TODO(PhilDay): Add support for timeout (clean shutdown)
        self._vmops.power_off(instance)

    def power_on(self, context, instance, network_info,
                 block_device_info=None):
        """Power on the specified instance."""
        self._vmops.power_on(instance)

    def soft_delete(self, instance):
        """Soft delete the specified instance."""
        self._vmops.soft_delete(instance)

    def restore(self, instance):
        """Restore the specified instance."""
        self._vmops.restore(instance)

    def poll_rebooting_instances(self, timeout, instances):
        """Poll for rebooting instances."""
        self._vmops.poll_rebooting_instances(timeout, instances)

    def reset_network(self, instance):
        """reset networking for specified instance."""
        self._vmops.reset_network(instance)

    def inject_network_info(self, instance, nw_info):
        """inject network info for specified instance."""
        self._vmops.inject_network_info(instance, nw_info)

    def plug_vifs(self, instance, network_info):
        """Plug VIFs into networks."""
        self._vmops.plug_vifs(instance, network_info)

    def unplug_vifs(self, instance, network_info):
        """Unplug VIFs from networks."""
        self._vmops.unplug_vifs(instance, network_info)

    def get_info(self, instance):
        """Return data about VM instance."""
        return self._vmops.get_info(instance)

    def get_diagnostics(self, instance):
        """Return data about VM diagnostics."""
        return self._vmops.get_diagnostics(instance)

    def get_instance_diagnostics(self, instance):
        """Return data about VM diagnostics."""
        return self._vmops.get_instance_diagnostics(instance)

    def get_all_bw_counters(self, instances):
        """Return bandwidth usage counters for each interface on each
           running VM.
        """

        # we only care about VMs that correspond to a nova-managed
        # instance:
        imap = {inst['name']: inst['uuid'] for inst in instances}
        bwcounters = []

        # get a dictionary of instance names.  values are dictionaries
        # of mac addresses with values that are the bw counters:
        # e.g. {'instance-001' : { 12:34:56:78:90:12 : {'bw_in': 0, ....}}
        all_counters = self._vmops.get_all_bw_counters()
        for instance_name, counters in all_counters.items():
            if instance_name in imap:
                # yes these are stats for a nova-managed vm
                # correlate the stats with the nova instance uuid:
                for vif_counter in counters.values():
                    vif_counter['uuid'] = imap[instance_name]
                    bwcounters.append(vif_counter)
        return bwcounters

    def get_console_output(self, context, instance):
        """Return snapshot of console."""
        return self._vmops.get_console_output(instance)

    def get_vnc_console(self, context, instance):
        """Return link to instance's VNC console."""
        return self._vmops.get_vnc_console(instance)

    def get_volume_connector(self, instance):
        """Return volume connector information."""
        if not self._initiator or not self._hypervisor_hostname:
            stats = self.host_state.get_host_stats(refresh=True)
            try:
                self._initiator = stats['host_other-config']['iscsi_iqn']
                self._hypervisor_hostname = stats['host_hostname']
            except (TypeError, KeyError) as err:
                LOG.warning('Could not determine key: %s', err,
                            instance=instance)
                self._initiator = None
        return {
            'ip': self._get_block_storage_ip(),
            'initiator': self._initiator,
            'host': self._hypervisor_hostname
        }

    def _get_block_storage_ip(self):
        # If CONF.my_block_storage_ip is set, use it.
        if CONF.my_block_storage_ip != CONF.my_ip:
            return CONF.my_block_storage_ip
        return self.get_host_ip_addr()

    def get_host_ip_addr(self):
        xs_url = urlparse.urlparse(CONF.xenserver.connection_url)
        return xs_url.netloc

    def attach_volume(self, context, connection_info, instance, mountpoint,
                      disk_bus=None, device_type=None, encryption=None):
        """Attach volume storage to VM instance."""
        self._volumeops.attach_volume(connection_info,
                                      instance['name'],
                                      mountpoint)

    def detach_volume(self, context, connection_info, instance, mountpoint,
                      encryption=None):
        """Detach volume storage from VM instance."""
        self._volumeops.detach_volume(connection_info,
                                      instance['name'],
                                      mountpoint)

    def get_console_pool_info(self, console_type):
        xs_url = urlparse.urlparse(CONF.xenserver.connection_url)
        return {'address': xs_url.netloc,
                'username': CONF.xenserver.connection_username,
                'password': CONF.xenserver.connection_password}

    def _get_vgpu_total(self, vgpu_stats):
        # NOTE(jianghuaw): Now we only enable one vGPU type in one
        # compute node. So normally vgpu_stats should contain only
        # one GPU group. If there are multiple GPU groups, they
        # must contain the same vGPU type. So just add them up.
        total = 0
        for grp_id in vgpu_stats:
            total += vgpu_stats[grp_id]['total']
        return total

    def get_inventory(self, nodename):
        """Return a dict, keyed by resource class, of inventory information for
        the supplied node.
        """
        host_stats = self.host_state.get_host_stats(refresh=True)

        vcpus = host_stats['host_cpu_info']['cpu_count']
        memory_mb = int(host_stats['host_memory_total'] / units.Mi)
        disk_gb = int(host_stats['disk_total'] / units.Gi)
        vgpus = self._get_vgpu_total(host_stats['vgpu_stats'])

        result = {
            fields.ResourceClass.VCPU: {
                'total': vcpus,
                'min_unit': 1,
                'max_unit': vcpus,
                'step_size': 1,
            },
            fields.ResourceClass.MEMORY_MB: {
                'total': memory_mb,
                'min_unit': 1,
                'max_unit': memory_mb,
                'step_size': 1,
            },
            fields.ResourceClass.DISK_GB: {
                'total': disk_gb,
                'min_unit': 1,
                'max_unit': disk_gb,
                'step_size': 1,
            },
        }
        if vgpus > 0:
            # Only create inventory for vGPU when driver can supply vGPUs.
            # At the moment, XenAPI can support up to one vGPU per VM,
            # so max_unit is 1.
            result.update(
                {
                    fields.ResourceClass.VGPU: {
                        'total': vgpus,
                        'min_unit': 1,
                        'max_unit': 1,
                        'step_size': 1,
                    }
                }
            )
        return result

    def _update_vgpu_rp(self, rp_tree, host_stats, root_name_or_uuid):
        """Update the resource providers for vGPUs in the provider tree. If the
        resource provider doesn't exist, we create one and make it as the
        compute node's child, then attach the inventory to it.
        """
        vgpu_stats = host_stats['vgpu_stats']
        for grp_id, grp_value in vgpu_stats.items():
            # Each gpu group have a specified group id, so we add `grp_id` to
            # distinguish corresponding resource providers. Add vgpu type info
            # to show supported vgpus as well.
            if vgpu_stats[grp_id]['total'] <= 0:
                # If the gpu group has no resource, remove the rp if it exists
                # and jump out of the loop
                if rp_tree.exists(grp_id):
                    rp_tree.remove(grp_id)
                continue

            rp_name = (VGPU_RP_PREFIX + grp_id)
            inv = {
                fields.ResourceClass.VGPU: {
                    'total': grp_value['total'],
                    'min_unit': 1,
                    'max_unit': 1,
                    'step_size': 1,
                }
            }
            if not rp_tree.exists(rp_name):
                # Set gpu group uuid as the resource provider uuid. When nova
                # compute requests to spawn an instance, it will send it to
                # driver in `allocations`, then we could easily find the
                # corresponding gpu group on the host
                rp_tree.new_child(rp_name, root_name_or_uuid, uuid=grp_id)

            rp_tree.update_inventory(rp_name, inv)

    def _remove_children_rp(self, provider_tree, parent_uuid_or_name):
        children_rps = provider_tree.get_provider_uuids(parent_uuid_or_name)
        # keep root resource provider
        children_rps.pop(0)
        for children_rp in children_rps:
            rp_data = provider_tree.data(children_rp)
            if rp_data.name[:len(VGPU_RP_PREFIX)] == VGPU_RP_PREFIX:
                provider_tree.remove(children_rp)

    def update_provider_tree(self, provider_tree, nodename):
        """Nested resource provider support. Update resource in provider tree.
        """
        host_stats = self.host_state.get_host_stats(refresh=True)
        self._update_compute_rp(nodename, provider_tree, host_stats)
        self._remove_children_rp(provider_tree, nodename)
        self._update_vgpu_rp(provider_tree, host_stats, nodename)

    def _update_compute_rp(self, nodename, rp_tree, host_stats):
        """Nested resource provider support. Update compute node information.
        If the resource provider is not exist, create it and attach the
        inventory to it.
        """
        vcpus = host_stats['host_cpu_info']['cpu_count']
        memory_mb = int(host_stats['host_memory_total'] / units.Mi)
        disk_gb = int(host_stats['disk_total'] / units.Gi)

        result = {
            fields.ResourceClass.VCPU: {
                'total': vcpus,
                'min_unit': 1,
                'max_unit': vcpus,
                'step_size': 1,
            },
            fields.ResourceClass.MEMORY_MB: {
                'total': memory_mb,
                'min_unit': 1,
                'max_unit': memory_mb,
                'step_size': 1,
            },
            fields.ResourceClass.DISK_GB: {
                'total': disk_gb,
                'min_unit': 1,
                'max_unit': disk_gb,
                'step_size': 1,
            },
        }
        if not rp_tree.exists(nodename):
            rp_tree.new_root(nodename, uuidutils.generate_uuid())
        rp_tree.update_inventory(nodename, result)

    def get_available_resource(self, nodename):
        """Retrieve resource information.

        This method is called when nova-compute launches, and
        as part of a periodic task that records the results in the DB.

        :param nodename: ignored in this driver
        :returns: dictionary describing resources

        """
        host_stats = self.host_state.get_host_stats(refresh=True)

        # Updating host information
        total_ram_mb = host_stats['host_memory_total'] / units.Mi
        # NOTE(belliott) memory-free-computed is a value provided by XenServer
        # for gauging free memory more conservatively than memory-free.
        free_ram_mb = host_stats['host_memory_free_computed'] / units.Mi
        total_disk_gb = host_stats['disk_total'] / units.Gi
        used_disk_gb = host_stats['disk_used'] / units.Gi
        allocated_disk_gb = host_stats['disk_allocated'] / units.Gi
        hyper_ver = versionutils.convert_version_to_int(
                                                self._session.product_version)
        dic = {'vcpus': host_stats['host_cpu_info']['cpu_count'],
               'memory_mb': total_ram_mb,
               'local_gb': total_disk_gb,
               'vcpus_used': host_stats['vcpus_used'],
               'memory_mb_used': total_ram_mb - free_ram_mb,
               'local_gb_used': used_disk_gb,
               'hypervisor_type': 'XenServer',
               'hypervisor_version': hyper_ver,
               'hypervisor_hostname': host_stats['host_hostname'],
               'cpu_info': jsonutils.dumps(host_stats['cpu_model']),
               'disk_available_least': total_disk_gb - allocated_disk_gb,
               'supported_instances': host_stats['supported_instances'],
               'pci_passthrough_devices': jsonutils.dumps(
                   host_stats['pci_passthrough_devices']),
               'numa_topology': None}

        return dic

    def ensure_filtering_rules_for_instance(self, instance, network_info):
        # NOTE(salvatore-orlando): it enforces security groups on
        # host initialization and live migration.
        # In XenAPI we do not assume instances running upon host initialization
        return

    def check_can_live_migrate_destination(self, context, instance,
                src_compute_info, dst_compute_info,
                block_migration=False, disk_over_commit=False):
        """Check if it is possible to execute live migration.

        :param context: security context
        :param instance: nova.db.sqlalchemy.models.Instance object
        :param block_migration: if true, prepare for block migration
        :param disk_over_commit: if true, allow disk over commit
        :returns: a XenapiLiveMigrateData object
        """
        return self._vmops.check_can_live_migrate_destination(context,
                                                              instance,
                                                              block_migration,
                                                              disk_over_commit)

    def cleanup_live_migration_destination_check(self, context,
                                                 dest_check_data):
        """Do required cleanup on dest host after check_can_live_migrate calls

        :param context: security context
        :param dest_check_data: result of check_can_live_migrate_destination
        """
        pass

    def check_can_live_migrate_source(self, context, instance,
                                      dest_check_data, block_device_info=None):
        """Check if it is possible to execute live migration.

        This checks if the live migration can succeed, based on the
        results from check_can_live_migrate_destination.

        :param context: security context
        :param instance: nova.db.sqlalchemy.models.Instance
        :param dest_check_data: result of check_can_live_migrate_destination
                                includes the block_migration flag
        :param block_device_info: result of _get_instance_block_device_info
        :returns: a XenapiLiveMigrateData object
        """
        return self._vmops.check_can_live_migrate_source(context, instance,
                                                         dest_check_data)

    def get_instance_disk_info(self, instance,
                               block_device_info=None):
        """Used by libvirt for live migration. We rely on xenapi
        checks to do this for us.
        """
        pass

    def live_migration(self, context, instance, dest,
                       post_method, recover_method, block_migration=False,
                       migrate_data=None):
        """Performs the live migration of the specified instance.

        :param context: security context
        :param instance:
            nova.db.sqlalchemy.models.Instance object
            instance object that is migrated.
        :param dest: destination host
        :param post_method:
            post operation method.
            expected nova.compute.manager._post_live_migration.
        :param recover_method:
            recovery method when any exception occurs.
            expected nova.compute.manager._rollback_live_migration.
        :param block_migration: if true, migrate VM disk.
        :param migrate_data: a XenapiLiveMigrateData object
        """
        self._vmops.live_migrate(context, instance, dest, post_method,
                                 recover_method, block_migration, migrate_data)

    def rollback_live_migration_at_destination(self, context, instance,
                                               network_info,
                                               block_device_info,
                                               destroy_disks=True,
                                               migrate_data=None):
        """Performs a live migration rollback.

        :param context: security context
        :param instance: instance object that was being migrated
        :param network_info: instance network information
        :param block_device_info: instance block device information
        :param destroy_disks:
            if true, destroy disks at destination during cleanup
        :param migrate_data: A XenapiLiveMigrateData object
        """

        # NOTE(johngarbutt) Destroying the VM is not appropriate here
        # and in the cases where it might make sense,
        # XenServer has already done it.
        # NOTE(sulo): The only cleanup we do explicitly is to forget
        # any volume that was attached to the destination during
        # live migration. XAPI should take care of all other cleanup.
        self._vmops.rollback_live_migration_at_destination(instance,
                                                           network_info,
                                                           block_device_info)

    def pre_live_migration(self, context, instance, block_device_info,
                           network_info, disk_info, migrate_data):
        """Preparation live migration.

        :param block_device_info:
            It must be the result of _get_instance_volume_bdms()
            at compute manager.
        :returns: a XenapiLiveMigrateData object
        """
        return self._vmops.pre_live_migration(context, instance,
                block_device_info, network_info, disk_info, migrate_data)

    def post_live_migration(self, context, instance, block_device_info,
                            migrate_data=None):
        """Post operation of live migration at source host.

        :param context: security context
        :instance: instance object that was migrated
        :block_device_info: instance block device information
        :param migrate_data: a XenapiLiveMigrateData object
        """
        self._vmops.post_live_migration(context, instance, migrate_data)

    def post_live_migration_at_source(self, context, instance, network_info):
        """Unplug VIFs from networks at source.

        :param context: security context
        :param instance: instance object reference
        :param network_info: instance network information
        """
        self._vmops.post_live_migration_at_source(context, instance,
                                                  network_info)

    def post_live_migration_at_destination(self, context, instance,
                                           network_info,
                                           block_migration=False,
                                           block_device_info=None):
        """Post operation of live migration at destination host.

        :param context: security context
        :param instance:
            nova.db.sqlalchemy.models.Instance object
            instance object that is migrated.
        :param network_info: instance network information
        :param block_migration: if true, post operation of block_migration.

        """
        self._vmops.post_live_migration_at_destination(context, instance,
                network_info, block_device_info, block_device_info)

    def unfilter_instance(self, instance, network_info):
        """Removes security groups configured for an instance."""
        return self._vmops.unfilter_instance(instance, network_info)

    def refresh_security_group_rules(self, security_group_id):
        """Updates security group rules for all instances associated with a
        given security group.

        Invoked when security group rules are updated.
        """
        return self._vmops.refresh_security_group_rules(security_group_id)

    def refresh_instance_security_rules(self, instance):
        """Updates security group rules for specified instance.

        Invoked when instances are added/removed to a security group
        or when a rule is added/removed to a security group.
        """
        return self._vmops.refresh_instance_security_rules(instance)

    def get_available_nodes(self, refresh=False):
        stats = self.host_state.get_host_stats(refresh=refresh)
        return [stats["hypervisor_hostname"]]

    def host_power_action(self, action):
        """The only valid values for 'action' on XenServer are 'reboot' or
        'shutdown', even though the API also accepts 'startup'. As this is
        not technically possible on XenServer, since the host is the same
        physical machine as the hypervisor, if this is requested, we need to
        raise an exception.
        """
        if action in ("reboot", "shutdown"):
            return self._host.host_power_action(action)
        else:
            msg = _("Host startup on XenServer is not supported.")
            raise NotImplementedError(msg)

    def set_host_enabled(self, enabled):
        """Sets the compute host's ability to accept new instances."""
        return self._host.set_host_enabled(enabled)

    def get_host_uptime(self):
        """Returns the result of calling "uptime" on the target host."""
        return self._host.get_host_uptime()

    def host_maintenance_mode(self, host, mode):
        """Start/Stop host maintenance window. On start, it triggers
        guest VMs evacuation.
        """
        return self._host.host_maintenance_mode(host, mode)

    def add_to_aggregate(self, context, aggregate, host, **kwargs):
        """Add a compute host to an aggregate."""
        return self._pool.add_to_aggregate(context, aggregate, host, **kwargs)

    def remove_from_aggregate(self, context, aggregate, host, **kwargs):
        """Remove a compute host from an aggregate."""
        return self._pool.remove_from_aggregate(context,
                                                aggregate, host, **kwargs)

    def undo_aggregate_operation(self, context, op, aggregate,
                                  host, set_error=True):
        """Undo aggregate operation when pool error raised."""
        return self._pool.undo_aggregate_operation(context, op,
                aggregate, host, set_error)

    def resume_state_on_host_boot(self, context, instance, network_info,
                                  block_device_info=None):
        """resume guest state when a host is booted."""
        self._vmops.power_on(instance)

    def get_per_instance_usage(self):
        """Get information about instance resource usage.

        :returns: dict of  nova uuid => dict of usage info
        """
        return self._vmops.get_per_instance_usage()

    def attach_interface(self, context, instance, image_meta, vif):
        """Use hotplug to add a network interface to a running instance.

        The counter action to this is :func:`detach_interface`.

        :param context: The request context.
        :param nova.objects.instance.Instance instance:
            The instance which will get an additional network interface.
        :param nova.objects.ImageMeta image_meta:
            The metadata of the image of the instance.
        :param nova.network.model.VIF vif:
            The object which has the information about the interface to attach.

        :raise nova.exception.NovaException: If the attach fails.

        :return: None
        """
        self._vmops.attach_interface(instance, vif)

    def detach_interface(self, context, instance, vif):
        """Use hotunplug to remove a network interface from a running instance.

        The counter action to this is :func:`attach_interface`.

        :param context: The request context.
        :param nova.objects.instance.Instance instance:
            The instance which gets a network interface removed.
        :param nova.network.model.VIF vif:
            The object which has the information about the interface to detach.

        :raise nova.exception.NovaException: If the detach fails.

        :return: None
        """
        self._vmops.detach_interface(instance, vif)
