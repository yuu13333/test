# Copyright (c) 2014 Cisco Systems, Inc.
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

"""The Server Group API Extension."""

from oslo_log import log as logging
import webob
from webob import exc

from nova.api.openstack import api_version_request
from nova.api.openstack import common
from nova.api.openstack.compute.schemas import server_groups as schema
from nova.api.openstack import extensions
from nova.api.openstack import wsgi
from nova.api import validation
import nova.exception
from nova.i18n import _
from nova import objects
from nova.policies import server_groups as sg_policies

LOG = logging.getLogger(__name__)

ALIAS = "os-server-groups"


def _authorize_context(req, action):
    context = req.environ['nova.context']
    context.can(sg_policies.POLICY_ROOT % action)
    return context


class ServerGroupController(wsgi.Controller):
    """The Server group API controller for the OpenStack API."""

    def _format_server_group(self, context, group, req):
        # the id field has its value as the uuid of the server group
        # There is no 'uuid' key in server_group seen by clients.
        # In addition, clients see policies as a ["policy-name"] list;
        # and they see members as a ["server-id"] list.
        server_group = {}
        server_group['id'] = group.uuid
        server_group['name'] = group.name
        server_group['policies'] = group.policies or []
        # NOTE(danms): This has been exposed to the user, but never used.
        # Since we can't remove it, just make sure it's always empty.
        server_group['metadata'] = {}
        members = []
        if group.members:
            # Display the instances that are not deleted.
            filters = {'uuid': group.members, 'deleted': False}
            instances = objects.InstanceList.get_by_filters(
                context, filters=filters)
            members = [instance.uuid for instance in instances]
        server_group['members'] = members
        # Add project id information to the response data for
        # API version v2.13
        if api_version_request.is_supported(req, min_version="2.13"):
            server_group['project_id'] = group.project_id
            server_group['user_id'] = group.user_id
        return server_group

    @extensions.expected_errors(404)
    def show(self, req, id):
        """Return data about the given server group."""
        context = _authorize_context(req, 'show')
        try:
            sg = objects.InstanceGroup.get_by_uuid(context, id)
        except nova.exception.InstanceGroupNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        return {'server_group': self._format_server_group(context, sg, req)}

    @wsgi.response(204)
    @extensions.expected_errors(404)
    def delete(self, req, id):
        """Delete an server group."""
        context = _authorize_context(req, 'delete')
        try:
            sg = objects.InstanceGroup.get_by_uuid(context, id)
        except nova.exception.InstanceGroupNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())
        try:
            sg.destroy()
        except nova.exception.InstanceGroupNotFound as e:
            raise webob.exc.HTTPNotFound(explanation=e.format_message())

    @extensions.expected_errors(())
    def index(self, req):
        """Returns a list of server groups."""
        context = _authorize_context(req, 'index')
        project_id = context.project_id
        if 'all_projects' in req.GET and context.is_admin:
            sgs = objects.InstanceGroupList.get_all(context)
        else:
            sgs = objects.InstanceGroupList.get_by_project_id(
                    context, project_id)
        limited_list = common.limited(sgs.objects, req)
        result = [self._format_server_group(context, group, req)
                  for group in limited_list]
        return {'server_groups': result}

    @wsgi.Controller.api_version("2.1")
    @extensions.expected_errors((400, 403))
    @validation.schema(schema.create, "2.1", "2.14")
    @validation.schema(schema.create_v215, "2.15")
    def create(self, req, body):
        """Creates a new server group."""
        context = _authorize_context(req, 'create')

        count = objects.Quotas.count(context, 'server_groups',
                                     context.project_id, context.user_id)
        try:
            objects.Quotas.limit_check(context, server_groups=count + 1)
        except nova.exception.OverQuota:
            msg = _("Quota exceeded, too many server groups.")
            raise exc.HTTPForbidden(explanation=msg)

        vals = body['server_group']
        sg = objects.InstanceGroup(context)
        sg.project_id = context.project_id
        sg.user_id = context.user_id
        try:
            sg.name = vals.get('name')
            sg.policies = vals.get('policies')
            sg.create()
        except ValueError as e:
            raise exc.HTTPBadRequest(explanation=e)

        return {'server_group': self._format_server_group(context, sg, req)}


class ServerGroups(extensions.V21APIExtensionBase):
    """Server group support."""
    name = "ServerGroups"
    alias = ALIAS
    version = 1

    def get_resources(self):
        res = extensions.ResourceExtension(
                 ALIAS, controller=ServerGroupController(),
                 member_actions={"action": "POST", })
        return [res]

    def get_controller_extensions(self):
        return []
