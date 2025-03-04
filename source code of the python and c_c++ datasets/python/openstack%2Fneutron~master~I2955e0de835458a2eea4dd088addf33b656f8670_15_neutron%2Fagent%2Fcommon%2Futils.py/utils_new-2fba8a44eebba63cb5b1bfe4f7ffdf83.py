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


LOG = logging.getLogger(__name__)
config.register_root_helper(cfg.CONF)
agents_db.register_agent_opts()

INTERFACE_NAMESPACE = 'neutron.interface_drivers'


execute = utils.execute


class Throttler(object):
    """Throttle number of calls to a function to only once per 'threshold'.

    :param threshold: minimum time betwen 2 'function' calls, in seconds.
    """
    def __init__(self, threshold=0):
        self.threshold = threshold
        self.last_time_called = time.time() - threshold

        # Is the function already running and should future calls be deferred?
        self.is_deferred = False

    def call(self, function, *args, **kwargs):
        # Check if another thread hasn't already scheduled 'function' to run.
        # If it has been, then it will do so in less than 'self.threshold'
        # seconds.
        if not self.is_deferred:

            time_since_last_call = (
                time.time() - self.last_time_called)
            if time_since_last_call < self.threshold:
                # We're in timeout, so we should throttle future calls.
                self.is_deferred = True
                time_to_wait = self.threshold - time_since_last_call
                eventlet.sleep(time_to_wait)

            self.last_time_called = time.time()
            try:
                function(*args, **kwargs)
            finally:
                self.is_deferred = False


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
