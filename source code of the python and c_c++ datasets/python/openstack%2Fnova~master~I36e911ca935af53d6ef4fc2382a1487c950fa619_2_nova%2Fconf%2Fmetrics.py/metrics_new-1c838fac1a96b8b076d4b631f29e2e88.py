# Copyright 2019 Aptira Pty Ltd
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

from oslo_config import cfg


metrics_group = cfg.OptGroup(
    'metrics',
    title='Metrics Options',
    help="""
A collection of options specific to the publishing of operational metrics.
""")


metrics_opts = [
    cfg.IntOpt('collection_interval',
        default=60,
        min=-1,
        help="""
Number of seconds to wait between metrics collections.

Possible values:
* 0: run at the default rate.
* -1: disable
* Any other value
"""),
    cfg.StrOpt('push_gateway',
         default='localhost:9091',
         help="""
The hostname and port of a prometheus pushgateway service to push metrics to.

This should be in the form hostname:port as a string.
"""),
    ]


def register_opts(conf):
    conf.register_group(metrics_group)
    conf.register_opts(metrics_opts, group=metrics_group)


def list_opts():
    return {'metrics': metrics_opts}
