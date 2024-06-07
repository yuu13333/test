# Copyright (c) 2015 Quobyte Inc.
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

import errno
import os
from time import sleep

from oslo_concurrency import processutils
from oslo_log import log as logging
from oslo_utils import fileutils
import six

import nova.conf
from nova import exception as nova_exception
from nova.i18n import _
from nova.i18n import _LE
from nova.i18n import _LI
from nova import utils
from nova.virt.libvirt import utils as libvirt_utils
from nova.virt.libvirt.volume import fs

LOG = logging.getLogger(__name__)

CONF = nova.conf.CONF

SOURCE_PROTOCOL = 'quobyte'
SOURCE_TYPE = 'file'
DRIVER_CACHE = 'none'
DRIVER_IO = 'native'


def mount_volume(volume, mnt_base, configfile=None):
    """Wraps execute calls for mounting a Quobyte volume"""
    fileutils.ensure_tree(mnt_base)

    command = ['mount.quobyte', volume, mnt_base]
    if os.path.exists(" /run/systemd/system"):
        # Note(kaisers): with systemd this requires a separate CGROUP to
        # prevent Nova service stop/restarts from killing the mount.
        command = ['systemd-run', 'mount.quobyte', '-f', volume, mnt_base]
    if configfile:
        command.extend(['-c', configfile])

    LOG.debug('Mounting volume %s at mount point %s ...',
              volume,
              mnt_base)
    # Run mount command but do not fail on already mounted exit code
    utils.execute(*command, run_as_root=True)
    if validate_and_wait(mnt_base):
        LOG.info(_LI('Mounted volume: %s'), volume)


def umount_volume(mnt_base):
    """Wraps execute calls for unmouting a Quobyte volume"""
    try:
        utils.execute('umount.quobyte', mnt_base, run_as_root=True)
    except processutils.ProcessExecutionError as exc:
        if 'Device or resource busy' in six.text_type(exc):
            LOG.error(_LE("The Quobyte volume at %s is still in use."),
                      mnt_base)
        else:
            LOG.exception(_LE("Couldn't unmount the Quobyte Volume at %s"),
                          mnt_base)


def validate_and_wait(mnt_base, max_checks=30):
    """Loops iterations of validate_volume with up to 30 0.1s interrupts"""
    if max_checks > 30:
        max_checks = 30
    for x in range(0, max_checks):
        if validate_volume(mnt_base):
            return True
        else:
            sleep(0.1)

    return False


def validate_volume(mnt_base):
    """Wraps execute calls for checking validity of a Quobyte volume"""
    command = ['getfattr', "-n", "quobyte.info", mnt_base]
    try:
        utils.execute(*command)
    except processutils.ProcessExecutionError as exc:
        msg = (_("The mount %(mount_path)s is not a valid"
                 " Quobyte volume. Error: %(exc)s")
               % {'mount_path': mnt_base, 'exc': exc})
        raise nova_exception.InternalError(msg)
        return False

    if not os.access(mnt_base, os.W_OK | os.X_OK):
        msg = (_LE("Volume is not writable. Please broaden the file"
                   " permissions. Mount: %s") % mnt_base)
        raise nova_exception.InternalError(msg)

    return True


class LibvirtQuobyteVolumeDriver(fs.LibvirtBaseFileSystemVolumeDriver):
    """Class implements libvirt part of volume driver for Quobyte."""

    def _get_mount_point_base(self):
        return CONF.libvirt.quobyte_mount_point_base

    def get_config(self, connection_info, disk_info):
        conf = super(LibvirtQuobyteVolumeDriver,
                     self).get_config(connection_info, disk_info)
        data = connection_info['data']
        conf.source_protocol = SOURCE_PROTOCOL
        conf.source_type = SOURCE_TYPE
        conf.driver_cache = DRIVER_CACHE
        conf.driver_io = DRIVER_IO
        conf.driver_format = data.get('format', 'raw')

        conf.source_path = self._get_device_path(connection_info)

        return conf

    @utils.synchronized('connect_volume')
    def connect_volume(self, connection_info, disk_info):
        """Connect the volume."""
        data = connection_info['data']
        quobyte_volume = self._normalize_export(data['export'])
        mount_path = self._get_mount_path(connection_info)
        mounted = libvirt_utils.is_mounted(mount_path,
                                           SOURCE_PROTOCOL
                                           + '@' + quobyte_volume)
        if mounted:
            try:
                os.stat(mount_path)
            except OSError as exc:
                if exc.errno == errno.ENOTCONN:
                    mounted = False
                    LOG.info(_LI('Fixing previous mount %s which was not'
                                 ' unmounted correctly.'), mount_path)
                    umount_volume(mount_path)

        if not mounted:
            mount_volume(quobyte_volume,
                         mount_path,
                         CONF.libvirt.quobyte_client_cfg)

        validate_and_wait(mount_path)

    @utils.synchronized('connect_volume')
    def disconnect_volume(self, connection_info, disk_dev):
        """Disconnect the volume."""

        quobyte_volume = self._normalize_export(
                                        connection_info['data']['export'])
        mount_path = self._get_mount_path(connection_info)

        if libvirt_utils.is_mounted(mount_path, 'quobyte@' + quobyte_volume):
            umount_volume(mount_path)
        else:
            LOG.info(_LI("Trying to disconnected unmounted volume at %s"),
                     mount_path)

    def _normalize_export(self, export):
        protocol = SOURCE_PROTOCOL + "://"
        if export.startswith(protocol):
            export = export[len(protocol):]
        return export
