# Copyright 2012 New Dream Network, LLC (DreamHost)
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
import grp
import os
import pwd

from oslo_config import cfg
from oslo_log import log as logging

from neutron.agent.linux import daemon
from neutron.agent.linux import utils as agent_utils
from neutron.common import config
from neutron.common import exceptions
from neutron.common import utils
from neutron.conf.agent.metadata import namespace_proxy as namespace

LOG = logging.getLogger(__name__)

PROXY_CONFIG_DIR = "ns-metadata-proxy"
_HAPROXY_CONFIG_TEMPLATE = """
global
    user        %(user)s
    group       %(group)s
    maxconn     1024

defaults
    mode                    http
    option http-server-close
    option forwardfor
    retries                 3
    timeout http-request    30s
    timeout connect         30s
    timeout client          32s
    timeout server          32s
    timeout http-keep-alive 30s

listen listener
    bind 0.0.0.0:%(port)s
    server metadata %(unix_socket_path)s
"""


class NetworkMetadataProxyHandler(object):
    """Proxy AF_INET metadata request through Unix Domain socket.

    The Unix domain socket allows the proxy access resource that are not
    accessible within the isolated tenant context.
    """

    def __init__(self, network_id=None, router_id=None, unix_socket_path=None,
                 port='9697', user=None, group=None, state_path=None):
        self.network_id = network_id
        self.router_id = router_id
        if network_id is None and router_id is None:
            raise exceptions.NetworkIdOrRouterIdRequiredError()

        self.port = port
        self.user = user or str(os.geteuid())
        self.group = group or str(os.getegid())
        self.state_path = state_path
        self.unix_socket_path = (unix_socket_path or
                                 cfg.CONF.metadata_proxy_socket)

    def _create_config_file(self):
        """Create the config file for haproxy."""
        # Need to convert uid/gid into username/group
        try:
            pw_user = pwd.getpwuid(int(self.user))
            username = pw_user[0]
        except ValueError:
            username = self.user

        try:
            groupname = grp.getgrgid(int(self.group)).gr_name
        except ValueError:
            groupname = self.group

        cfg_info = {
            'port': self.port,
            'unix_socket_path': self.unix_socket_path,
            'user': username,
            'group': groupname
        }

        haproxy_cfg = _HAPROXY_CONFIG_TEMPLATE % cfg_info
        if self.network_id:
            haproxy_cfg += (
                "    http-request add-header X-Neutron-Network-ID %s\n" %
                self.network_id)
        if self.router_id:
            haproxy_cfg += (
                "    http-request add-header X-Neutron-Router-ID %s\n" %
                self.router_id)
        LOG.debug("haproxy_cfg = %s", haproxy_cfg)
        cfg_dir = self.get_config_path(self.state_path)
        # uuid has to be included somewhere in the command line so that it can
        # be tracked by process_monitor.
        self.cfg_path = os.path.join(cfg_dir,
                                     (self.router_id or self.network_id) +
                                     ".conf")
        if not os.path.exists(cfg_dir):
            os.makedirs(cfg_dir)
        with open(self.cfg_path, "w") as cfg_file:
            cfg_file.write(haproxy_cfg)

    def run_proxy(self):
        """Start haproxy with the right config file."""
        self._create_config_file()
        haproxy_path = agent_utils.execute(['which', 'haproxy'],
                                           run_as_root=False).strip()
        LOG.debug("haproxy path: %s", haproxy_path)

        # Replace our process image with haproxy and keep our PID for tracking
        os.execl(haproxy_path, haproxy_path, '-f', self.cfg_path)

    @staticmethod
    def get_config_path(state_path):
        return os.path.join(state_path or cfg.CONF.state_path,
                            PROXY_CONFIG_DIR)


def cleanup_config_file(uuid, state_path=None):
    """Delete config file created when metadata proxy was spawned."""
    # Delete config file if it exists
    cfg_path = os.path.join(
        NetworkMetadataProxyHandler.get_config_path(state_path),
        uuid + ".conf")
    try:
        os.unlink(cfg_path)
    except OSError as ex:
        # It can happen that this function is called but metadata proxy
        # was never spawned so its config file won't exist
        if ex.errno != errno.ENOENT:
            raise


class ProxyDaemon(daemon.Daemon):
    def __init__(self, pidfile, port, network_id=None, router_id=None,
                 user=None, group=None, watch_log=True,
                 proxy_socket=None, state_path=''):
        uuid = network_id or router_id
        super(ProxyDaemon, self).__init__(pidfile, uuid=uuid, user=user,
                                          group=group, watch_log=watch_log)
        self.network_id = network_id
        self.router_id = router_id
        self.port = port
        self.proxy_socket = proxy_socket
        self.state_path = state_path

    def run(self):
        handler = NetworkMetadataProxyHandler(
            self.network_id,
            self.router_id,
            self.proxy_socket,
            self.port,
            self.user,
            self.group,
            self.state_path)
        handler.run_proxy()


def main():
    namespace.register_namespace_proxy_opts(cfg.CONF)
    # Don't read any default configuration file,  just handle cmdline opts
    cfg.CONF(project='neutron',
             default_config_files=[], default_config_dirs=[])
    config.setup_logging()
    utils.log_opt_values(LOG)

    proxy = ProxyDaemon(cfg.CONF.pid_file,
                        cfg.CONF.metadata_port,
                        network_id=cfg.CONF.network_id,
                        router_id=cfg.CONF.router_id,
                        user=cfg.CONF.metadata_proxy_user,
                        group=cfg.CONF.metadata_proxy_group,
                        proxy_socket=cfg.CONF.metadata_proxy_socket,
                        state_path=cfg.CONF.state_path)

    if cfg.CONF.daemonize:
        proxy.start()
    else:
        proxy.run()
