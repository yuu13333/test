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

import collections
import contextlib
import logging
import os.path
import six
import threading

from oslo_concurrency import processutils

import nova.conf
from nova import exception
from nova.i18n import _LE
from nova import utils

CONF = nova.conf.CONF
LOG = logging.getLogger(__name__)


class MountManager(object):
    __instance__ = None
    __instance_lock__ = threading.Lock()

    class _ManagedMount(object):
        def __init__(self):
            self.lock = threading.Lock()
            self.volumes = []
            self.mounted = False

    def __init__(self, host):
        self.mountpoints = collections.defaultdict(self._ManagedMount)

        for guest in host.list_guests(only_running=False):
            for disk in guest.get_all_disks():

                # All remote filesystem volumes are files
                if disk.type != 'file':
                    continue

                # NOTE(mdbooth): We're assuming that the mountpoint is our
                # immediate parent, which is currently true for all
                # volume drivers. We deliberately don't do anything clever
                # here, because we don't want to, e.g.:
                # * Add mountpoints for non-volume disks
                # * Get it wrong when a non-running domain references a
                #   volume which isn't mounted because the host just rebooted.
                # and this is good enough. We could probably do better here
                # with more thought.

                mountpoint = os.path.dirname(disk.source_path)
                if not os.path.ismount(mountpoint):
                    continue
                name = os.path.relpath(disk.source_path, mountpoint)

                # No locking required here because this is running before
                # we start servicing user requests
                mount = self.mountpoints[mountpoint]
                mount.volumes.append(name)
                mount.mounted = True

    @classmethod
    def get(cls):
        # We hold the instance lock here so that if the MountManager is
        # currently initialising we'll wait for it to complete rather than
        # fail.
        with cls.__instance_lock__:
            mount_manager = cls.__instance__
            if mount_manager is None:
                raise exception.HypervisorUnavailable(host=CONF.host)
            return mount_manager

    @classmethod
    def host_up(cls, host):
        with cls.__instance_lock__:
            cls.__instance__ = MountManager(host)

    @classmethod
    def host_down(cls):
        with cls.__instance_lock__:
            cls.__instance__ = None

    @contextlib.contextmanager
    def _get_locked(self, mountpoint):
        # This dance is because we delete locks. We need to be sure that the
        # lock we hold does not belong to an object which has been deleted.
        # We do this by checking that mountpoint still refers to this object
        # when we hold the lock. This is safe because:
        # * we only delete an object from mountpounts whilst holding its lock
        # * mountpoints is a defaultdict which will atomically create a new
        #   object on access
        while True:
            mount = self.mountpoints[mountpoint]
            with mount.lock:
                if self.mountpoints[mountpoint] is mount:
                    yield mount
                    break

    def mount(self, fstype, export, vol_name, mountpoint, options=None):
        with self._get_locked(mountpoint) as mount:
            if not mount.mounted:
                utils.execute('mkdir', '-p', mountpoint)

                mount_cmd = ['mount', '-t', fstype]
                if options is not None:
                    mount_cmd.extend(options)
                mount_cmd.extend([export, mountpoint])

                # We're not expecting to be mounted already, so we let errors
                # propagate
                try:
                    utils.execute(*mount_cmd, run_as_root=True)
                except Exception:
                    # If the mount failed there's no reason for us to keep a
                    # record of it. It will be created again if the caller
                    # retries.

                    # Delete while holding lock
                    del self.mountpoints[mountpoint]

                    raise

                mount.mounted = True

            mount.volumes.append(vol_name)

    def umount(self, vol_name, mountpoint):
        with self._get_locked(mountpoint) as mount:
            # This will raise ValueError if share isn't in volumes, which is
            # as good an error as any.
            mount.volumes.remove(vol_name)

            if len(mount.volumes) == 0:
                try:
                    utils.execute('umount', mountpoint, run_as_root=True,
                                  attempts=3, delay_on_retry=True)

                    # Delete while holding lock
                    del self.mountpoints[mountpoint]
                    self.mounted = False

                    utils.execute('rmdir', mountpoint)
                except processutils.ProcessExecutionError as ex:
                    LOG.error(_LE("Couldn't unmount %(mountpoint)s: %(msg)s"),
                              {'mountpoint': mountpoint,
                               'msg': six.text_type(ex)})
