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
"""Placement API handlers for setting and deleting allocations."""

import collections

from oslo_log import log as logging
from oslo_serialization import jsonutils
from oslo_utils import encodeutils
from oslo_utils import timeutils
import webob

from nova.api.openstack.placement import errors
from nova.api.openstack.placement import exception
from nova.api.openstack.placement import microversion
from nova.api.openstack.placement.objects import resource_provider as rp_obj
from nova.api.openstack.placement.policies import allocation as policies
from nova.api.openstack.placement.schemas import allocation as schema
from nova.api.openstack.placement import util
from nova.api.openstack.placement import wsgi_wrapper
from nova.i18n import _


LOG = logging.getLogger(__name__)


def _last_modified_from_allocations(allocations, want_version):
    """Given a set of allocation objects, returns the last modified timestamp.
    """
    # NOTE(cdent): The last_modified for an allocation will always be
    # based off the created_at column because allocations are only
    # ever inserted, never updated.
    last_modified = None
    # Only calculate last-modified if we are using a microversion that
    # supports it.
    get_last_modified = want_version and want_version.matches((1, 15))
    for allocation in allocations:
        if get_last_modified:
            last_modified = util.pick_last_modified(last_modified, allocation)

    last_modified = last_modified or timeutils.utcnow(with_timezone=True)
    return last_modified


def _serialize_allocations_for_consumer(allocations, want_version):
    """Turn a list of allocations into a dict by resource provider uuid.

    {
        'allocations': {
            RP_UUID_1: {
                'generation': GENERATION,
                'resources': {
                    'DISK_GB': 4,
                    'VCPU': 2
                }
            },
            RP_UUID_2: {
                'generation': GENERATION,
                'resources': {
                    'DISK_GB': 6,
                    'VCPU': 3
                }
            }
        },
        # project_id and user_id are added with microverion 1.12
        'project_id': PROJECT_ID,
        'user_id': USER_ID,
        # Generation for consumer >= 1.28
        'consumer_generation': 1
    }
    """
    allocation_data = collections.defaultdict(dict)
    for allocation in allocations:
        key = allocation.resource_provider.uuid
        if 'resources' not in allocation_data[key]:
            allocation_data[key]['resources'] = {}

        resource_class = allocation.resource_class
        allocation_data[key]['resources'][resource_class] = allocation.used
        generation = allocation.resource_provider.generation
        allocation_data[key]['generation'] = generation

    result = {'allocations': allocation_data}
    if allocations and want_version.matches((1, 12)):
        # We're looking at a list of allocations by consumer id so project and
        # user are consistent across the list
        consumer = allocations[0].consumer
        project_id = consumer.project.external_id
        user_id = consumer.user.external_id
        result['project_id'] = project_id
        result['user_id'] = user_id
        show_consumer_gen = want_version.matches((1, 28))
        if show_consumer_gen:
            result['consumer_generation'] = consumer.generation

    return result


def _serialize_allocations_for_resource_provider(allocations,
                                                 resource_provider,
                                                 want_version):
    """Turn a list of allocations into a dict by consumer id.

    {'resource_provider_generation': GENERATION,
     'allocations':
       CONSUMER_ID_1: {
           'resources': {
              'DISK_GB': 4,
              'VCPU': 2
           },
           # Generation for consumer >= 1.28
           'consumer_generation': 0
       },
       CONSUMER_ID_2: {
           'resources': {
              'DISK_GB': 6,
              'VCPU': 3
           },
           # Generation for consumer >= 1.28
           'consumer_generation': 0
       }
    }
    """
    show_consumer_gen = want_version.matches((1, 28))
    allocation_data = collections.defaultdict(dict)
    for allocation in allocations:
        key = allocation.consumer.uuid
        if 'resources' not in allocation_data[key]:
            allocation_data[key]['resources'] = {}

        resource_class = allocation.resource_class
        allocation_data[key]['resources'][resource_class] = allocation.used

        if show_consumer_gen:
            consumer_gen = None
            if allocation.consumer is not None:
                consumer_gen = allocation.consumer.generation
            allocation_data[key]['consumer_generation'] = consumer_gen

    result = {'allocations': allocation_data}
    result['resource_provider_generation'] = resource_provider.generation
    return result


@wsgi_wrapper.PlacementWsgify
@util.check_accept('application/json')
def list_for_consumer(req):
    """List allocations associated with a consumer."""
    context = req.environ['placement.context']
    context.can(policies.ALLOC_LIST)
    consumer_id = util.wsgi_path_item(req.environ, 'consumer_uuid')
    want_version = req.environ[microversion.MICROVERSION_ENVIRON]

    # NOTE(cdent): There is no way for a 404 to be returned here,
    # only an empty result. We do not have a way to validate a
    # consumer id.
    allocations = rp_obj.AllocationList.get_all_by_consumer_id(
        context, consumer_id)

    output = _serialize_allocations_for_consumer(allocations, want_version)
    last_modified = _last_modified_from_allocations(allocations, want_version)
    allocations_json = jsonutils.dumps(output)

    response = req.response
    response.status = 200
    response.body = encodeutils.to_utf8(allocations_json)
    response.content_type = 'application/json'
    if want_version.matches((1, 15)):
        response.last_modified = last_modified
        response.cache_control = 'no-cache'
    return response


@wsgi_wrapper.PlacementWsgify
@util.check_accept('application/json')
def list_for_resource_provider(req):
    """List allocations associated with a resource provider."""
    # TODO(cdent): On a shared resource provider (for example a
    # giant disk farm) this list could get very long. At the moment
    # we have no facility for limiting the output. Given that we are
    # using a dict of dicts for the output we are potentially limiting
    # ourselves in terms of sorting and filtering.
    context = req.environ['placement.context']
    context.can(policies.RP_ALLOC_LIST)
    want_version = req.environ[microversion.MICROVERSION_ENVIRON]
    uuid = util.wsgi_path_item(req.environ, 'uuid')

    # confirm existence of resource provider so we get a reasonable
    # 404 instead of empty list
    try:
        rp = rp_obj.ResourceProvider.get_by_uuid(context, uuid)
    except exception.NotFound as exc:
        raise webob.exc.HTTPNotFound(
            _("Resource provider '%(rp_uuid)s' not found: %(error)s") %
            {'rp_uuid': uuid, 'error': exc})

    allocs = rp_obj.AllocationList.get_all_by_resource_provider(context, rp)

    output = _serialize_allocations_for_resource_provider(
        allocs, rp, want_version)
    last_modified = _last_modified_from_allocations(allocs, want_version)
    allocations_json = jsonutils.dumps(output)

    response = req.response
    response.status = 200
    response.body = encodeutils.to_utf8(allocations_json)
    response.content_type = 'application/json'
    if want_version.matches((1, 15)):
        response.last_modified = last_modified
        response.cache_control = 'no-cache'
    return response


def _new_allocations(context, resource_provider_uuid, consumer_uuid,
                     resources, project_id, user_id, consumer_generation,
                     want_version):
    """Create new allocation objects for a set of resources

    Returns a list of Allocation objects.

    :param context: The placement context.
    :param resource_provider_uuid: The uuid of the resource provider that
                                   has the resources.
    :param consumer_uuid: The uuid of the consumer of the resources.
    :param resources: A dict of resource classes and values.
    :param project_id: The project consuming the resources.
    :param user_id: The user consuming the resources.
    :param consumer_generation: The generation supplied by the user when
                                PUT/POST'ing allocations. May be None if
                                the microversion is <1.28
    :param want_version: The microversion object from the context.
    """
    allocations = []
    try:
        resource_provider = rp_obj.ResourceProvider.get_by_uuid(
            context, resource_provider_uuid)
    except exception.NotFound:
        raise webob.exc.HTTPBadRequest(
            _("Allocation for resource provider '%(rp_uuid)s' "
              "that does not exist.") %
            {'rp_uuid': resource_provider_uuid})
    consumer = util.ensure_consumer(
        context, consumer_uuid, project_id, user_id, consumer_generation,
        want_version)
    for resource_class in resources:
        allocation = rp_obj.Allocation(
            resource_provider=resource_provider,
            consumer=consumer,
            resource_class=resource_class,
            used=resources[resource_class])
        allocations.append(allocation)
    return allocations


def _set_allocations_for_consumer(req, schema):
    context = req.environ['placement.context']
    context.can(policies.ALLOC_UPDATE)
    consumer_uuid = util.wsgi_path_item(req.environ, 'consumer_uuid')
    data = util.extract_json(req.body, schema)
    allocation_data = data['allocations']

    # Normalize allocation data to dict.
    want_version = req.environ[microversion.MICROVERSION_ENVIRON]
    if not want_version.matches((1, 12)):
        allocations_dict = {}
        # Allocation are list-ish, transform to dict-ish
        for allocation in allocation_data:
            resource_provider_uuid = allocation['resource_provider']['uuid']
            allocations_dict[resource_provider_uuid] = {
                'resources': allocation['resources']
            }
        allocation_data = allocations_dict

    # If the body includes an allocation for a resource provider
    # that does not exist, raise a 400.
    allocation_objects = []
    if not allocation_data:
        # The allocations are empty, which means wipe them out. Internal
        # to the allocation object this is signalled by a used value of 0.
        # We still need to verify the consumer's generation, though, which
        # we do in _ensure_consumer()
        # NOTE(jaypipes): This will only occur 1.28+. The JSONSchema will
        # prevent an empty allocations object from being passed when there is
        # no consumer generation, so this is safe to do.
        util.ensure_consumer(context, consumer_uuid, data.get('project_id'),
             data.get('user_id'), data.get('consumer_generation'),
             want_version)
        allocations = rp_obj.AllocationList.get_all_by_consumer_id(
            context, consumer_uuid)
        for allocation in allocations:
            allocation.used = 0
            allocation_objects.append(allocation)
    else:
        for resource_provider_uuid, allocation in allocation_data.items():
            new_allocations = _new_allocations(context,
                                               resource_provider_uuid,
                                               consumer_uuid,
                                               allocation['resources'],
                                               data.get('project_id'),
                                               data.get('user_id'),
                                               data.get('consumer_generation'),
                                               want_version)
            allocation_objects.extend(new_allocations)

    allocations = rp_obj.AllocationList(
        context, objects=allocation_objects)

    try:
        allocations.create_all()
        LOG.debug("Successfully wrote allocations %s", allocations)
    # InvalidInventory is a parent for several exceptions that
    # indicate either that Inventory is not present, or that
    # capacity limits have been exceeded.
    except exception.NotFound as exc:
        raise webob.exc.HTTPBadRequest(
                _("Unable to allocate inventory for consumer "
                  "%(consumer_uuid)s: %(error)s") %
            {'consumer_uuid': consumer_uuid, 'error': exc})
    except exception.InvalidInventory as exc:
        raise webob.exc.HTTPConflict(
            _('Unable to allocate inventory: %(error)s') % {'error': exc})
    except exception.ConcurrentUpdateDetected as exc:
        raise webob.exc.HTTPConflict(
            _('Inventory and/or allocations changed while attempting to '
              'allocate: %(error)s') % {'error': exc},
              comment=errors.CONCURRENT_UPDATE)

    req.response.status = 204
    req.response.content_type = None
    return req.response


@wsgi_wrapper.PlacementWsgify
@microversion.version_handler('1.0', '1.7')
@util.require_content('application/json')
def set_allocations_for_consumer(req):
    return _set_allocations_for_consumer(req, schema.ALLOCATION_SCHEMA)


@wsgi_wrapper.PlacementWsgify  # noqa
@microversion.version_handler('1.8', '1.11')
@util.require_content('application/json')
def set_allocations_for_consumer(req):
    return _set_allocations_for_consumer(req, schema.ALLOCATION_SCHEMA_V1_8)


@wsgi_wrapper.PlacementWsgify  # noqa
@microversion.version_handler('1.12', '1.27')
@util.require_content('application/json')
def set_allocations_for_consumer(req):
    return _set_allocations_for_consumer(req, schema.ALLOCATION_SCHEMA_V1_12)


@wsgi_wrapper.PlacementWsgify  # noqa
@microversion.version_handler('1.28')
@util.require_content('application/json')
def set_allocations_for_consumer(req):
    return _set_allocations_for_consumer(req, schema.ALLOCATION_SCHEMA_V1_28)


@wsgi_wrapper.PlacementWsgify
@microversion.version_handler('1.13')
@util.require_content('application/json')
def set_allocations(req):
    context = req.environ['placement.context']
    context.can(policies.ALLOC_MANAGE)
    want_version = req.environ[microversion.MICROVERSION_ENVIRON]
    want_schema = schema.POST_ALLOCATIONS_V1_13
    if want_version.matches((1, 28)):
        want_schema = schema.POST_ALLOCATIONS_V1_28
    data = util.extract_json(req.body, want_schema)

    # Create a sequence of allocation objects to be used in an
    # AllocationList.create_all() call, which will mean all the changes
    # happen within a single transaction and with resource provider
    # generations check all in one go.
    allocation_objects = []

    for consumer_uuid in data:
        project_id = data[consumer_uuid]['project_id']
        user_id = data[consumer_uuid]['user_id']
        allocations = data[consumer_uuid]['allocations']
        consumer_generation = data[consumer_uuid].get('consumer_generation')
        if allocations:
            for resource_provider_uuid in allocations:
                resources = allocations[resource_provider_uuid]['resources']
                new_allocations = _new_allocations(context,
                                                   resource_provider_uuid,
                                                   consumer_uuid,
                                                   resources,
                                                   project_id,
                                                   user_id,
                                                   consumer_generation,
                                                   want_version)
                allocation_objects.extend(new_allocations)
        else:
            # The allocations are empty, which means wipe them out.
            # Internal to the allocation object this is signalled by a
            # used value of 0.
            allocations = rp_obj.AllocationList.get_all_by_consumer_id(
                context, consumer_uuid)
            for allocation in allocations:
                allocation.used = 0
                allocation_objects.append(allocation)

    allocations = rp_obj.AllocationList(
        context, objects=allocation_objects)

    try:
        allocations.create_all()
        LOG.debug("Successfully wrote allocations %s", allocations)
    except exception.NotFound as exc:
        raise webob.exc.HTTPBadRequest(
            _("Unable to allocate inventory %(error)s") % {'error': exc})
    except exception.InvalidInventory as exc:
        # InvalidInventory is a parent for several exceptions that
        # indicate either that Inventory is not present, or that
        # capacity limits have been exceeded.
        raise webob.exc.HTTPConflict(
            _('Unable to allocate inventory: %(error)s') % {'error': exc})
    except exception.ConcurrentUpdateDetected as exc:
        raise webob.exc.HTTPConflict(
            _('Inventory and/or allocations changed while attempting to '
              'allocate: %(error)s') % {'error': exc},
              comment=errors.CONCURRENT_UPDATE)

    req.response.status = 204
    req.response.content_type = None
    return req.response


@wsgi_wrapper.PlacementWsgify
def delete_allocations(req):
    context = req.environ['placement.context']
    context.can(policies.ALLOC_DELETE)
    consumer_uuid = util.wsgi_path_item(req.environ, 'consumer_uuid')

    allocations = rp_obj.AllocationList.get_all_by_consumer_id(
        context, consumer_uuid)
    if allocations:
        try:
            allocations.delete_all()
        # NOTE(pumaranikar): Following NotFound exception added in the case
        # when allocation is deleted from allocations list by some other
        # activity. In that case, delete_all() will throw a NotFound exception.
        except exception.NotFound as exc:
            raise webob.exc.HTTPNotFound(
                  _("Allocation for consumer with id %(id)s not found."
                    "error: %(error)s") %
                  {'id': consumer_uuid, 'error': exc})
    else:
        raise webob.exc.HTTPNotFound(
            _("No allocations for consumer '%(consumer_uuid)s'") %
            {'consumer_uuid': consumer_uuid})
    LOG.debug("Successfully deleted allocations %s", allocations)

    req.response.status = 204
    req.response.content_type = None
    return req.response
