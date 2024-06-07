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

from oslo_db import exception as db_exc
from oslo_log import log as logging
from oslo_versionedobjects import base
from oslo_versionedobjects import fields
import sqlalchemy as sa

from nova.api.openstack.placement import db_api
from nova.api.openstack.placement import exception
from nova.api.openstack.placement.objects import project as project_obj
from nova.api.openstack.placement.objects import user as user_obj
from nova.db.sqlalchemy import api_models as models

CONSUMER_TBL = models.Consumer.__table__
_ALLOC_TBL = models.Allocation.__table__

LOG = logging.getLogger(__name__)


@db_api.placement_context_manager.writer
def create_incomplete_consumers(ctx, batch_size):
    """Finds all the consumer records that are missing for allocations and
    creates consumer records for them, using the "incomplete consumer" project
    and user CONF options.

    Returns a tuple containing two identical elements with the number of
    consumer records created, since this is the expected return format for data
    migration routines.
    """
    # Create a record in the projects table for our incomplete project
    incomplete_proj_id = project_obj.ensure_incomplete_project(ctx)

    # Create a record in the users table for our incomplete user
    incomplete_user_id = user_obj.ensure_incomplete_user(ctx)

    # Create a consumer table record for all consumers where
    # allocations.consumer_id doesn't exist in the consumers table. Use the
    # incomplete consumer project and user ID.
    alloc_to_consumer = sa.outerjoin(
        _ALLOC_TBL, CONSUMER_TBL,
        _ALLOC_TBL.c.consumer_id == CONSUMER_TBL.c.uuid)
    cols = [
        _ALLOC_TBL.c.consumer_id,
        incomplete_proj_id,
        incomplete_user_id,
    ]
    sel = sa.select(cols)
    sel = sel.select_from(alloc_to_consumer)
    sel = sel.where(CONSUMER_TBL.c.id.is_(None))
    sel = sel.limit(batch_size)
    target_cols = ['uuid', 'project_id', 'user_id']
    ins_stmt = CONSUMER_TBL.insert().from_select(target_cols, sel)
    res = ctx.session.execute(ins_stmt)
    return res.rowcount, res.rowcount


@db_api.placement_context_manager.writer
def delete_consumer_if_no_allocations(ctx, consumer_uuid):
    """Looks to see if the supplied consumer has any allocations and if not,
    deletes the consumer record entirely.

    :param ctx: `nova.context.RequestContext` that contains an oslo_db Session
    :param consumer_uuid: UUID of the consumer to check and maybe delete
    """
    # First, do a fast lookup to see if there are any allocation records for
    # this consumer
    sel = sa.select([_ALLOC_TBL.c.consumer_id])
    sel = sel.where(_ALLOC_TBL.c.consumer_id == consumer_uuid)
    sel = sel.limit(1)
    res = ctx.session.execute(sel).fetchall()
    if res:
        return
    # Delete the consumer since there's no allocation records
    del_stmt = CONSUMER_TBL.delete()
    del_stmt = del_stmt.where(CONSUMER_TBL.c.uuid == consumer_uuid)
    LOG.debug("Deleting consumer record for consumer %s since there are no "
              "more allocations for this consumer.", consumer_uuid)
    ctx.session.execute(del_stmt)


@db_api.placement_context_manager.reader
def _get_consumer_by_uuid(ctx, uuid):
    # The SQL for this looks like the following:
    # SELECT
    #   c.id, c.uuid,
    #   p.id AS project_id, p.external_id AS project_external_id,
    #   u.id AS user_id, u.external_id AS user_external_id,
    #   c.updated_at, c.created_at
    # FROM consumers c
    # INNER JOIN projects p
    #  ON c.project_id = p.id
    # INNER JOIN users u
    #  ON c.user_id = u.id
    # WHERE c.uuid = $uuid
    consumers = sa.alias(CONSUMER_TBL, name="c")
    projects = sa.alias(project_obj.PROJECT_TBL, name="p")
    users = sa.alias(user_obj.USER_TBL, name="u")
    cols = [
        consumers.c.id,
        consumers.c.uuid,
        projects.c.id.label("project_id"),
        projects.c.external_id.label("project_external_id"),
        users.c.id.label("user_id"),
        users.c.external_id.label("user_external_id"),
        consumers.c.generation,
        consumers.c.updated_at,
        consumers.c.created_at
    ]
    c_to_p_join = sa.join(
        consumers, projects, consumers.c.project_id == projects.c.id)
    c_to_u_join = sa.join(
        c_to_p_join, users, consumers.c.user_id == users.c.id)
    sel = sa.select(cols).select_from(c_to_u_join)
    sel = sel.where(consumers.c.uuid == uuid)
    res = ctx.session.execute(sel).fetchone()
    if not res:
        raise exception.ConsumerNotFound(uuid=uuid)

    return dict(res)


@db_api.placement_context_manager.writer
def _increment_consumer_generation(ctx, consumer):
    """Increments the supplied consumer's generation value, supplying the
    consumer object which contains the currently-known generation. Returns the
    newly-incremented generation.

    :param ctx: `nova.context.RequestContext` that contains an oslo_db Session
    :param consumer: `Consumer` whose generation should be updated.
    :returns: The newly-incremented generation.
    :raises nova.exception.ConcurrentUpdateDetected: if another thread updated
            the same consumer's view of its allocations in between the time
            when this object was originally read and the call which modified
            the consumer's state (e.g. replacing allocations for a consumer)
    """
    consumer_gen = consumer.generation
    new_generation = consumer_gen + 1
    upd_stmt = CONSUMER_TBL.update().where(sa.and_(
            CONSUMER_TBL.c.id == consumer.id,
            CONSUMER_TBL.c.generation == consumer_gen)).values(
                    generation=new_generation)

    res = ctx.session.execute(upd_stmt)
    if res.rowcount != 1:
        raise exception.ConcurrentUpdateDetected
    return new_generation


@base.VersionedObjectRegistry.register_if(False)
class Consumer(base.VersionedObject, base.TimestampedObject):

    fields = {
        'id': fields.IntegerField(read_only=True),
        'uuid': fields.UUIDField(nullable=False),
        'project': fields.ObjectField('Project', nullable=False),
        'user': fields.ObjectField('User', nullable=False),
        'generation': fields.IntegerField(nullable=False),
    }

    @staticmethod
    def _from_db_object(ctx, target, source):
        target.id = source['id']
        target.uuid = source['uuid']
        target.generation = source['generation']
        target.created_at = source['created_at']
        target.updated_at = source['updated_at']

        target.project = project_obj.Project(
            ctx, id=source['project_id'],
            external_id=source['project_external_id'])
        target.user = user_obj.User(
            ctx, id=source['user_id'],
            external_id=source['user_external_id'])

        target._context = ctx
        target.obj_reset_changes()
        return target

    @classmethod
    def get_by_uuid(cls, ctx, uuid):
        res = _get_consumer_by_uuid(ctx, uuid)
        return cls._from_db_object(ctx, cls(ctx), res)

    def create(self):
        @db_api.placement_context_manager.writer
        def _create_in_db(ctx):
            db_obj = models.Consumer(
                uuid=self.uuid, project_id=self.project.id,
                user_id=self.user.id)
            try:
                db_obj.save(ctx.session)
                # NOTE(jaypipes): We don't do the normal _from_db_object()
                # thing here because models.Consumer doesn't have a
                # project_external_id or user_external_id attribute.
                self.id = db_obj.id
                self.generation = db_obj.generation
            except db_exc.DBDuplicateEntry:
                raise exception.ConsumerExists(uuid=self.uuid)
        _create_in_db(self._context)
        self.obj_reset_changes()

    def increment_generation(self):
        """Increments the consumer's generation.

        :raises nova.exception.ConcurrentUpdateDetected: if another thread
            updated the same consumer's view of its allocations in between the
            time when this object was originally read and the call which
            modified the consumer's state (e.g. replacing allocations for a
            consumer)
        """
        self.generation = _increment_consumer_generation(self._context, self)
