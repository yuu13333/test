# Copyright (c) 2014 OpenStack Foundation.
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

import eventlet
from oslo_log import log as logging
from oslo_utils import fileutils
from oslo_utils import timeutils
import webob

from neutron._i18n import _LE, _LI, _LW
from neutron.agent.l3 import router_processing_queue as queue
from neutron.agent.linux import utils as agent_utils
from neutron.common import constants
from neutron.notifiers import batch_notifier

LOG = logging.getLogger(__name__)

KEEPALIVED_STATE_CHANGE_SERVER_BACKLOG = 4096

TRANSLATION_MAP = {'master': constants.HA_ROUTER_STATE_ACTIVE,
                   'backup': constants.HA_ROUTER_STATE_STANDBY,
                   'fault': constants.HA_ROUTER_STATE_STANDBY}


class KeepalivedStateChangeHandler(object):
    def __init__(self, agent):
        self.agent = agent

    @webob.dec.wsgify(RequestClass=webob.Request)
    def __call__(self, req):
        router_id = req.headers['X-Neutron-Router-Id']
        state = req.headers['X-Neutron-State']
        self.enqueue(router_id, state)

    def enqueue(self, router_id, state):
        LOG.debug('Handling notification for router '
                  '%(router_id)s, state %(state)s', {'router_id': router_id,
                                                     'state': state})
        self.agent.enqueue_state_change(router_id, state)


class L3AgentKeepalivedStateChangeServer(object):
    def __init__(self, agent, conf):
        self.agent = agent
        self.conf = conf

        agent_utils.ensure_directory_exists_without_file(
            self.get_keepalived_state_change_socket_path(self.conf))

    @classmethod
    def get_keepalived_state_change_socket_path(cls, conf):
        return os.path.join(conf.state_path, 'keepalived-state-change')

    def run(self):
        server = agent_utils.UnixDomainWSGIServer(
            'neutron-keepalived-state-change',
            num_threads=self.conf.ha_keepalived_state_change_server_threads)
        server.start(KeepalivedStateChangeHandler(self.agent),
                     self.get_keepalived_state_change_socket_path(self.conf),
                     workers=0,
                     backlog=KEEPALIVED_STATE_CHANGE_SERVER_BACKLOG)
        server.wait()


class AgentMixin(object):
    def __init__(self, host):
        self._init_ha_conf_path()
        super(AgentMixin, self).__init__(host)
        self.state_change_notifier = batch_notifier.BatchNotifier(
            self._calculate_batch_duration(), self.notify_server)
        eventlet.spawn(self._start_keepalived_notifications_server)

    def _get_router_info(self, router_id):
        try:
            return self.router_info[router_id]
        except KeyError:
            LOG.info(_LI('Router %s is not managed by this agent. It was '
                         'possibly deleted concurrently.'), router_id)

    def check_ha_state_for_router(self, router_id, current_state):
        ri = self._get_router_info(router_id)
        if ri and current_state != TRANSLATION_MAP[ri.ha_state]:
            LOG.debug("Updating server with state %(state)s for router "
                      "%(router_id)s", {'router_id': router_id,
                                        'state': ri.ha_state})
            self.state_change_notifier.queue_event((router_id, ri.ha_state))

    def _start_keepalived_notifications_server(self):
        state_change_server = (
            L3AgentKeepalivedStateChangeServer(self, self.conf))
        state_change_server.run()

    def _calculate_batch_duration(self):
        # Slave becomes the master after not hearing from it 3 times
        detection_time = self.conf.ha_vrrp_advert_int * 3

        # Keepalived takes a couple of seconds to configure the VIPs
        configuration_time = 2

        # Give it enough slack to batch all events due to the same failure
        return (detection_time + configuration_time) * 2

    def _get_router(self, router_id):
        try:
            routers = self.plugin_rpc.get_routers(self.context,
                                                  [router_id])
            if routers:
                return routers[0]
        except Exception:
            msg = _LE("Failed to fetch router information for '%s'")
            LOG.exception(msg, router_id)

    def enqueue_state_change(self, router_id, state):
        timestamp = timeutils.utcnow()
        router = self._get_router(router_id)

        if not router:
            LOG.warning(
                _LW("Will not do master state change actions "
                    "for HA router %s, unable to ensure that "
                    "the HA router still exists."), router_id)
            return

        router['ha_router_state'] = state
        update = queue.RouterUpdate(
            router_id,
            queue.PRIORITY_SYNC_ROUTERS_TASK,
            action=queue.HA_ROUTER_STATE_CHANGE,
            router=router,
            timestamp=timestamp)
        self._queue.add(update)

    def _ha_router_state_change_actions(self, router):
        router_id = router['id']
        state = router.get('ha_router_state', 'backup')
        LOG.info(_LI('Router %(router_id)s transitioned to %(state)s'),
                 {'router_id': router_id,
                  'state': state})

        ri = self._get_router_info(router_id)
        if ri is None:
            return

        self._configure_ipv6_ra_on_ext_gw_port_if_necessary(ri, state)
        if self.conf.enable_metadata_proxy:
            self._update_metadata_proxy(ri, router_id, state)
        self._update_radvd_daemon(ri, state)
        self.state_change_notifier.queue_event((router_id, state))

    def _configure_ipv6_ra_on_ext_gw_port_if_necessary(self, ri, state):
        # If ipv6 is enabled on the platform, ipv6_gateway config flag is
        # not set and external_network associated to the router does not
        # include any IPv6 subnet, enable the gateway interface to accept
        # Router Advts from upstream router for default route.
        ex_gw_port_id = ri.ex_gw_port and ri.ex_gw_port['id']
        if state == 'master' and ex_gw_port_id:
            interface_name = ri.get_external_device_name(ex_gw_port_id)
            if ri.router.get('distributed', False):
                namespace = ri.ha_namespace
            else:
                namespace = ri.ns_name
            ri._enable_ra_on_gw(ri.ex_gw_port, namespace, interface_name)

    def _update_metadata_proxy(self, ri, router_id, state):
        if state == 'master':
            LOG.debug('Spawning metadata proxy for router %s', router_id)
            self.metadata_driver.spawn_monitored_metadata_proxy(
                self.process_monitor, ri.ns_name, self.conf.metadata_port,
                self.conf, router_id=ri.router_id)
        else:
            LOG.debug('Closing metadata proxy for router %s', router_id)
            self.metadata_driver.destroy_monitored_metadata_proxy(
                self.process_monitor, ri.router_id, self.conf)

    def _update_radvd_daemon(self, ri, state):
        # Radvd has to be spawned only on the Master HA Router. If there are
        # any state transitions, we enable/disable radvd accordingly.
        if state == 'master':
            ri.enable_radvd()
        else:
            ri.disable_radvd()

    def notify_server(self, batched_events):
        translated_states = dict((router_id, TRANSLATION_MAP[state]) for
                                 router_id, state in batched_events)
        LOG.debug('Updating server with HA routers states %s',
                  translated_states)
        self.plugin_rpc.update_ha_routers_states(
            self.context, translated_states)

    def _init_ha_conf_path(self):
        ha_full_path = os.path.dirname("/%s/" % self.conf.ha_confs_path)
        fileutils.ensure_tree(ha_full_path, mode=0o755)
