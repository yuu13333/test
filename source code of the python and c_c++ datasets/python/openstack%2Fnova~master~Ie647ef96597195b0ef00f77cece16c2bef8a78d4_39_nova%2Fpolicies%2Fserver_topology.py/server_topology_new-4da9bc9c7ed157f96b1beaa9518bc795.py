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

from nova.policies import base
from oslo_policy import policy


BASE_POLICY_NAME = 'os_compute_api:servers:topology:%s'

server_topology_policies = [
    policy.DocumentedRuleDefault(
        BASE_POLICY_NAME % 'index',
        base.RULE_ADMIN_OR_OWNER,
        "Show NUMA topology data of a given server",
        [
            {
                'method': 'GET',
                'path': '/servers/{server_id}/topology'
            }
        ]),
    policy.DocumentedRuleDefault(
        # control host NUMA node and cpu pinning information
        BASE_POLICY_NAME % 'host_info:index',
        base.RULE_ADMIN_API,
        "List host NUMA id and cpu pinning information of given server",
        [
            {
                'method': 'GET',
                'path': '/servers/{server_id}/topology'
            }
        ]),
]


def list_rules():
    return server_topology_policies
