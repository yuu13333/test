# Copyright 2015 Cloudbase Solutions.
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

import contextlib
import functools
import os
import time

import eventlet
from oslo_config import cfg
from oslo_log import log as logging
from oslo_utils import timeutils

from neutron._i18n import _LE
from neutron.agent.common import config
from neutron.common import utils as neutron_utils
from neutron.conf.agent.database import agents_db


if os.name == 'nt':
    from neutron.agent.windows import utils
else:
    from neutron.agent.linux import utils

# Should match cfg.CONF.ha_vrrp_advert_int.
#TODO(jschwarz): using cfg.CONF.ha_vrrp_advert_int necessitates an import,
# which results in a recursive import. This should be fixed into being the
# actual config value.
DEFAULT_THROTTLER_VALUE = 2
DEFAULT_THROTTLER_MULTIPLIER = 1.2

LOG = logging.getLogger(__name__)
config.register_root_helper(cfg.CONF)
agents_db.register_agent_opts()

INTERFACE_NAMESPACE = 'neutron.interface_drivers'


execute = utils.execute


class throttler(object):
    """Throttle number of calls to a function to only once per 'threshold'."""

    def __init__(self, func):
        """Decorate a function with throttler."""
        self._threshold = DEFAULT_THROTTLER_VALUE

        self._last_time_called = time.time() - self.throttle_threshold
        self._func = func
        # Is the function already running and should future calls be deferred?
        self._is_deferred = False

    @property
    def throttle_threshold(self):
        return self._threshold

    @throttle_threshold.setter
    def throttle_threshold(self, value):
        self._last_time_called += self._threshold - value
        self._threshold = value

    @contextlib.contextmanager
    def defer_call(self):
        self._is_deferred = True
        try:
            yield
        finally:
            self._is_deferred = False

    def sleep(self):
        while True:
            time_to_wait = (self._last_time_called - time.time()
                            + self.throttle_threshold)
            if time_to_wait < 0:
                break

            eventlet.sleep(time_to_wait)

    def __call__(self, *args, **kwargs):
        """Check if another thread hasn't already scheduled 'function' to run.

        If it has been, then it will do so in less than
        'self.throttle_threshold' seconds.
        """
        #NOTE(jschwarz): If we ever stop using greenthreads, we'd need some
        # sort of a locking mechanism here.
        if not self._is_deferred:
            time_since_last_call = time.time() - self._last_time_called
            if time_since_last_call < self.throttle_threshold:
                # We're in timeout, so we should throttle future calls.
                with self.defer_call():
                    self.sleep()
                    self._last_time_called = time.time()
                    return self._func(*args, **kwargs)

            else:
                self._last_time_called = time.time()
                return self._func(*args, **kwargs)

    def __get__(self, obj, objtype):
        return functools.partial(self.__call__, obj)


def load_interface_driver(conf):
    """Load interface driver for agents like DHCP or L3 agent.

    :param conf: driver configuration object
    :raises SystemExit of 1 if driver cannot be loaded
    """

    try:
        loaded_class = neutron_utils.load_class_by_alias_or_classname(
                INTERFACE_NAMESPACE, conf.interface_driver)
        return loaded_class(conf)
    except ImportError:
        LOG.error(_LE("Error loading interface driver '%s'"),
                  conf.interface_driver)
        raise SystemExit(1)


def is_agent_down(heart_beat_time):
    return timeutils.is_older_than(heart_beat_time,
                                   cfg.CONF.agent_down_time)
